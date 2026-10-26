#!/usr/bin/env python3
"""Check nano5g hardware AES, then decrypt an 8730 IMG1 through emCORE USB.

Requires the matching kernel from this tree. Only temporary device RAM is used.
No automatic retry of AES requests: a lost reply leaves the buffer state unknown.
"""
import argparse
import hashlib
from pathlib import Path
import struct

HEADER = 0x600
CHUNK = 0x10000


def image_body(data, fmt):
    if len(data) < HEADER or data[:7] != b"87302.0" or data[7] != fmt:
        raise ValueError("expected an 8730 v2.0 IMG1 in format %d" % fmt)
    size = struct.unpack_from("<I", data, 12)[0]
    padded = (size + 15) & ~15
    if not size or HEADER + padded > len(data):
        raise ValueError("IMG1 body is empty or truncated (including AES padding)")
    return size, padded


class Engine:
    def __init__(self, em):
        self.em = em
        self.addr = em.memalign(32, CHUNK + 64)
        if not self.addr or self.addr & 31:
            raise RuntimeError("aligned device allocation failed")

    def close(self):
        self.em.free(self.addr)

    def crypt(self, data, encrypt=False, keysel=1, key=b"", iv=bytes(16)):
        if not data or len(data) > CHUNK or len(data) & 15 or len(iv) != 16:
            raise ValueError("invalid AES buffer/IV length")
        if keysel == 0 and len(key) not in (16, 24, 32):
            raise ValueError("user key must be 16, 24 or 32 bytes")
        params = key.ljust(32, b"\0") + iv
        staged = params + data
        self.em.write(self.addr, staged)
        if bytes(self.em.read(self.addr, len(staged))) != staged:
            raise RuntimeError("AES upload read-back mismatch")
        rc = self.em.aescrypt_ext(encrypt, self.addr, len(data), keysel,
                                  len(key) * 8 if keysel == 0 else 128, False, True)
        if rc:
            raise RuntimeError("AES driver returned %d" % rc)
        result = bytes(self.em.read(self.addr + 32, 16 + len(data)))
        if len(result) != 16 + len(data):
            raise RuntimeError("short AES read-back")
        return result[16:], result[:16]

    def selftest(self):
        # FIPS-197 C.1 and SP 800-38A F.2 (first two CBC blocks).
        h = bytes.fromhex
        pt = h("00112233445566778899aabbccddeeff")
        key = bytes(range(16))
        ct = h("69c4e0d86a7b0430d8cdb78070b4c55a")
        if self.crypt(pt, True, 0, key)[0] != ct or self.crypt(ct, False, 0, key)[0] != pt:
            raise RuntimeError("FIPS-197 AES-128 check failed")
        pt = h("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51")
        for keyhex, cthex in (
            ("2b7e151628aed2a6abf7158809cf4f3c", "7649abac8119b246cee98e9b12e9197d5086cb9b507219ee95db113a917678b2"),
            ("8e73b0f7da0e6452c810f32b809079e562f8ead2522c6b7b", "4f021db243bc633d7178183a9fa071e8b4d9ada9ad7dedf4e5e738763f69145a"),
            ("603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4", "f58c4c04d6e5f1ba779eabfb5f7bfbd69cfc4e967edb808d679f777bc6702c7d"),
        ):
            key, ct = h(keyhex), h(cthex)
            for encrypt, src, expected in ((True, pt, ct), (False, ct, pt)):
                out, iv = self.crypt(src, encrypt, 0, key, bytes(range(16)))
                if out != expected or iv != ct[-16:]:
                    raise RuntimeError("CBC-%d check failed" % (len(key) * 8))
                # Also exercise chaining across USB commands.
                first, iv = self.crypt(src[:16], encrypt, 0, key, bytes(range(16)))
                second, iv = self.crypt(src[16:], encrypt, 0, key, iv)
                if first + second != expected or iv != ct[-16:]:
                    raise RuntimeError("CBC chunk chaining failed")


def decrypt_image(engine, data, reference=None):
    size, padded = image_body(data, 3)
    if reference is not None and image_body(reference, 4)[0] != size:
        raise ValueError("reference body length differs")
    out = bytearray(data)
    iv = bytes(16)
    for off in range(0, padded, CHUNK):
        src = data[HEADER + off:HEADER + min(off + CHUNK, padded)]
        plain, iv = engine.crypt(src, iv=iv)
        if iv != src[-16:]:
            raise RuntimeError("hardware-key CBC chaining value mismatch")
        out[HEADER + off:HEADER + off + len(plain)] = plain
    if reference is not None and out[HEADER:HEADER + size] != reference[HEADER:HEADER + size]:
        raise RuntimeError("decrypted body does not match the known plaintext")
    out[7] = 4
    return bytes(out)


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("image", nargs="?", type=Path)
    p.add_argument("-o", "--output", type=Path)
    p.add_argument("--reference", type=Path, help="known decrypted IMG1; compare every body byte")
    p.add_argument("--self-test", action="store_true", help="run user-key checks only")
    a = p.parse_args(argv)
    if a.self_test:
        if a.image or a.output or a.reference:
            p.error("--self-test takes no image/output/reference")
        data = reference = None
    else:
        if not a.image or not a.output:
            p.error("provide IMAGE -o OUTPUT, or --self-test")
        if a.output.exists():
            p.error("output already exists; choose a new file")
        data = a.image.read_bytes()
        image_body(data, 3)
        reference = a.reference.read_bytes() if a.reference else None
        if reference is not None and image_body(reference, 4)[0] != image_body(data, 3)[0]:
            p.error("reference body length differs")
    import libemcore
    em = libemcore.Emcore()
    engine = None
    try:
        if em.lib.dev.hwtypeid != 0x47354e49:
            raise RuntimeError("this tool requires an iPod nano 5G")
        em.bulk_write = False
        em.lib.bulkin = None
        engine = Engine(em)
        engine.selftest()
        print("User-key AES-128/192/256 and CBC chaining checks passed.")
        if data is not None:
            out = decrypt_image(engine, data, reference)
            with a.output.open("xb") as f:
                f.write(out)
            size, _ = image_body(out, 4)
            print("Saved %s; body SHA-256 %s" % (a.output, hashlib.sha256(out[HEADER:HEADER + size]).hexdigest()))
            print("Known plaintext matches." if reference is not None else
                  "Hardware-key plaintext is unverified; use --reference to verify it.")
    finally:
        try:
            if engine is not None:
                engine.close()
        finally:
            em.close()


if __name__ == "__main__":
    main()
