#!/usr/bin/env python3
"""n5g_mkimage.py -- pack the nano 5G emCORE first stage + kernel into an IMG1 "8730" file .

    n5g_mkimage.py --stage1 stage1.bin --payload emcore.bin [--stage1-elf stage1.elf] [--kernel-elf emcore.elf]
                   [--raw payload.bin] [--dfu-suffix] -o emcore-nano5g.dfu

Raw payload (what the ROM loads to mem:0x22000000 after it strips the header, ):

    +0x0000  stage1.bin                      (position dependent at 0x22000000, entry = first word)
    +A       u32 length of emcore.bin        A = align16(len(stage1.bin)) = the linker symbol _stage1_end
    +A+4     emcore.bin, padded to 4 bytes   (stage1 copies it to 0x08000000 and jumps there)

IMG1 container (same header as the 4G,  "IMG1 8730 v2.0", observed on fw/WTF.x1231.RELEASE.dfu.decrypted):

    0x00 "8730"  0x04 "2.0"  0x07 format (4 = plaintext body, 3 = encrypted body)  0x08 entry (0)
    0x0c body length, always 16-aligned (the payload is zero-padded into it)
    0x10 data length = cert offset + cert length  0x14 cert offset = body + 0x80 (16-aligned)
    0x18 cert length (--cert-len, default 0x300)
    0x40..0x4f header tag: left zero (the signing step computes it over hdr[0:0x40])
    rest of the 0x600 header zero; body at file+0x600; 0x80 zero bytes for the image signature at
    body+length; cert-length zero bytes for the certificates.
These are the rom_validate_image rules (padded = (len+15)&~15, data >= padded+0x300,
cert offset >= padded+0x80 and 16-aligned) and Apple's layout (raw_fw/*: signature at the padded body end,
as in its DFU images, whose body lengths are aligned). The signed digest covers the header and the
stored body. Unaligned bodies are refused rather than padded implicitly.

The result is UNSIGNED: prepared for an off-device signing step (which, for format 3, also encrypts the
padded body and then fills the tag, signature and certificates). A software model of the ROM's DFU load
accepts it as is once the signature check is patched out.

ROM limit : data length <= 0x24c00, i.e. body <= 0x24880 bytes.

--dfu-suffix appends the standard 16-byte DFU suffix (the ROM checks the last word = CRC-32, reflected table, init
0xffffffff, no final inversion, over everything before it). Off by default: the signing step is expected to
handle the container end itself.
"""
import argparse
import struct
import subprocess
import sys

HEADER = 0x600
SIG_LEN = 0x80
CERT_LEN = 0x300
DATALEN_MAX = 0x24c00          # rom_validate_header: datalen < 0x24c01
ROM_BUFFER = 0x25200           # ROM DFU load buffer 0x22000000..0x22025200 
STAGE1_BASE = 0x22000000
KERNEL_BASE = 0x08000000
STAGE1_MAX = 0x1000            # stage1.ld ASSERT
BODY_ALIGN = 16                # AES block; rom_validate_image pads the body length to it
BOOT_FILE_MAX = 120000        # project budget, including IMG1 header/footer and optional suffix


def align(n, a):
    return (n + a - 1) // a * a


def nm_symbols(elf, nm="arm-none-eabi-nm"):
    out = subprocess.run([nm, elf], capture_output=True, text=True, check=True).stdout
    syms = {}
    for line in out.splitlines():
        p = line.split()
        if len(p) == 3:
            syms[p[2]] = int(p[0], 16)
    return syms


def build_payload(stage1, kernel, stage1_end=None):
    """stage1.bin + padding to _stage1_end + {u32 len, kernel padded to 4 bytes}."""
    if len(stage1) > STAGE1_MAX:
        raise ValueError("stage 1 is %d bytes, limit %d" % (len(stage1), STAGE1_MAX))
    end = align(len(stage1), 16)
    if stage1_end is not None and stage1_end != STAGE1_BASE + end:
        raise ValueError("stage1.elf _stage1_end 0x%x != 0x%x derived from stage1.bin" % (stage1_end, STAGE1_BASE + end))
    klen = align(len(kernel), 4)
    return stage1 + b"\0" * (end - len(stage1)) + struct.pack("<I", klen) + kernel + b"\0" * (klen - len(kernel))


def align_body(body):
    """Zero-pad the IMG1 body to BODY_ALIGN bytes (stage 1 copies only the kernel length it was given)."""
    return body + b"\0" * (align(len(body), BODY_ALIGN) - len(body))


def build_img1(body, fmt=4, cert_len=CERT_LEN):
    """Aligned bodies only: body length == padded length, so the signature sits right at the body end."""
    if fmt not in (3, 4):
        raise ValueError("format must be 3 or 4")
    if len(body) % BODY_ALIGN:
        raise ValueError("body length 0x%x is not %d-byte aligned; pad it with align_body()" % (len(body), BODY_ALIGN))
    if cert_len < 0x300 - SIG_LEN:
        raise ValueError("certificate slot 0x%x too small: data length must be >= body + 0x300" % cert_len)
    cert_off = len(body) + SIG_LEN
    datalen = cert_off + cert_len
    if datalen > DATALEN_MAX:
        raise ValueError("data length 0x%x exceeds the ROM limit 0x%x (body %d bytes, max %d)" %
                         (datalen, DATALEN_MAX, len(body), DATALEN_MAX - SIG_LEN - cert_len))
    hdr = bytearray(HEADER)
    hdr[0:4] = b"8730"
    hdr[4:7] = b"2.0"
    hdr[7] = fmt
    struct.pack_into("<IIIII", hdr, 8, 0, len(body), datalen, cert_off, cert_len)
    return bytes(hdr) + body + b"\0" * (SIG_LEN + cert_len)


def dfu_crc32(data):
    crc = 0xffffffff
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ (0xedb88320 if crc & 1 else 0)
    return crc


def dfu_suffix(data, vid=0x05ac, pid=0x1231):
    """16-byte DFU suffix: bcdDevice, idProduct, idVendor, bcdDFU 0x0100, 'UFD', length 16, CRC-32."""
    s = struct.pack("<HHHH3sB", 0xffff, pid, vid, 0x0100, b"UFD", 16)
    return data + s + struct.pack("<I", dfu_crc32(data + s))


def check_boot_budget(image):
    if len(image) > BOOT_FILE_MAX:
        raise ValueError("boot image is %d bytes, project limit %d; move optional code into SDRAM modules" %
                         (len(image), BOOT_FILE_MAX))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--stage1", required=True)
    ap.add_argument("--payload", required=True, help="emcore.bin (raw kernel, linked for 0x08000000)")
    ap.add_argument("--stage1-elf")
    ap.add_argument("--kernel-elf")
    ap.add_argument("--raw", help="also write the raw payload (IMG1 body) here")
    ap.add_argument("--dfu-suffix", action="store_true")
    ap.add_argument("--format", type=int, choices=(3, 4), default=4,
                    help="IMG1 format byte: 4 plaintext body (default), 3 = the signing step encrypts the body")
    ap.add_argument("--cert-len", type=lambda v: int(v, 0), default=CERT_LEN,
                    help="certificate slot reserved for the signing step (default 0x300; Apple's chain is 0xbe3)")
    ap.add_argument("-o", "--output", required=True)
    a = ap.parse_args(argv)

    stage1 = open(a.stage1, "rb").read()
    kernel = open(a.payload, "rb").read()
    s1end = None
    if a.stage1_elf:
        s1end = nm_symbols(a.stage1_elf).get("_stage1_end")
    if a.kernel_elf:
        k = nm_symbols(a.kernel_elf)
        if "_inittailend" in k and k["_inittailend"] - KERNEL_BASE != len(kernel):
            print("warning: emcore.bin is %d bytes but _inittailend - 0x08000000 = %d" %
                  (len(kernel), k["_inittailend"] - KERNEL_BASE), file=sys.stderr)
    try:
        body = build_payload(stage1, kernel, s1end)
        body = align_body(body)
        img = build_img1(body, a.format, a.cert_len)
        if a.dfu_suffix:
            img = dfu_suffix(img)
        check_boot_budget(img)
    except ValueError as e:
        print("n5g_mkimage: %s" % e, file=sys.stderr)
        return 1
    if a.raw:
        open(a.raw, "wb").write(body)
    open(a.output, "wb").write(img)
    hdr = struct.unpack_from("<IIIII", img, 8)
    print("stage1 %d B + payload %d B -> body %d B (0x%x), IMG1 %d B, format %d, data length 0x%x of max 0x%x, "
          "cert offset 0x%x length 0x%x, UNSIGNED -> %s" %
          (len(stage1), len(kernel), len(body), len(body), len(img), img[7], hdr[2], DATALEN_MAX, hdr[3], hdr[4],
           a.output))
    return 0


if __name__ == "__main__":
    sys.exit(main())
