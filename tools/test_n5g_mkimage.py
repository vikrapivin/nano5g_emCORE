#!/usr/bin/env python3
"""Tests for tools/n5g_mkimage.py (header fields, payload layout, ROM size limit, DFU suffix)."""
import os
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import n5g_mkimage as M

WTF = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "fw", "WTF.x1231.RELEASE.dfu.decrypted")


class MkImage(unittest.TestCase):
    def test_payload_layout(self):
        s1 = bytes(range(100))
        k = b"K" * 10
        p = M.build_payload(s1, k)
        self.assertEqual(p[:100], s1)
        self.assertEqual(p[100:112], b"\0" * 12)              # padded to _stage1_end = 112
        self.assertEqual(struct.unpack_from("<I", p, 112)[0], 12)  # kernel length padded to 4
        self.assertEqual(p[116:], k + b"\0\0")

    def test_stage1_end_check(self):
        with self.assertRaises(ValueError):
            M.build_payload(b"x" * 100, b"y" * 4, stage1_end=0x22000000 + 100)
        M.build_payload(b"x" * 100, b"y" * 4, stage1_end=0x22000000 + 112)

    def test_header_fields(self):
        body = b"B" * 0x1000
        img = M.build_img1(body)
        self.assertEqual(img[:8], b"8730" + b"2.0" + b"\x04")
        w = struct.unpack_from("<IIIII", img, 8)
        self.assertEqual(w, (0, 0x1000, 0x1000 + 0x380, 0x1000 + 0x80, 0x300))
        self.assertEqual(len(img), 0x600 + 0x1000 + 0x380)
        self.assertEqual(img[0x600:0x1600], body)
        self.assertEqual(img[0x1600:], b"\0" * 0x380)

    def test_aligned_only_rom_rules(self):
        """ rom_validate_image rules; only 16-aligned bodies are packed."""
        with self.assertRaises(ValueError):
            M.build_img1(b"B" * 0xff44)
        body = M.align_body(b"B" * 0xff44)
        self.assertEqual(len(body), 0xff50)
        self.assertEqual(body[0xff44:], b"\0" * 12)
        for fmt, cert_len in ((4, 0x300), (3, 0xbe3)):
            img = M.build_img1(body, fmt, cert_len)
            entry, blen, data, cert_off, clen = struct.unpack_from("<IIIII", img, 8)
            padded = (blen + 15) & ~15
            self.assertEqual((img[7], blen, padded, clen), (fmt, 0xff50, 0xff50, cert_len))
            self.assertEqual(cert_off, padded + 0x80)
            self.assertEqual(cert_off & 15, 0)
            self.assertGreaterEqual(data, padded + 0x300)
            self.assertEqual(data, cert_off + cert_len)
            self.assertEqual(len(img), 0x600 + data)
            self.assertEqual(img[0x600:0x600 + blen], body)
            self.assertEqual(img[0x600 + blen:], b"\0" * (0x80 + cert_len))
            self.assertEqual(img[0x40:0x50], b"\0" * 16)          # header tag: the signing step's job
        with self.assertRaises(ValueError):
            M.build_img1(body, 5)
        with self.assertRaises(ValueError):
            M.build_img1(body, 4, 0x200)

    @unittest.skipUnless(os.path.exists(WTF), "fw/WTF image not present")
    def test_header_matches_apple_wtf(self):
        a = open(WTF, "rb").read()
        body = a[0x600:0x600 + struct.unpack_from("<I", a, 0xc)[0]]
        mine = M.build_img1(body)
        self.assertEqual(mine[:0x1c], a[:0x1c])              # magic, version, fmt, entry, lengths, cert fields
        self.assertEqual(len(mine), len(a))

    def test_rom_limit(self):
        maxbody = M.DATALEN_MAX - M.SIG_LEN - M.CERT_LEN
        M.build_img1(b"\0" * maxbody)
        with self.assertRaises(ValueError):
            M.build_img1(b"\0" * (maxbody + 1))
        self.assertLessEqual(M.HEADER + M.DATALEN_MAX, M.ROM_BUFFER)

    def test_stage1_limit(self):
        with self.assertRaises(ValueError):
            M.build_payload(b"\0" * (M.STAGE1_MAX + 1), b"")

    def test_project_boot_limit(self):
        image = M.build_img1(b'\0' * (M.BOOT_FILE_MAX - M.HEADER - M.SIG_LEN - M.CERT_LEN))
        M.check_boot_budget(image)
        with self.assertRaises(ValueError):
            M.check_boot_budget(image + b'\0')
        with self.assertRaises(ValueError):
            M.check_boot_budget(M.dfu_suffix(image))

    def test_dfu_suffix(self):
        d = M.dfu_suffix(b"abc")
        self.assertEqual(len(d), 3 + 16)
        self.assertEqual(d[3 + 8:3 + 11], b"UFD")
        self.assertEqual(d[3 + 11], 16)
        self.assertEqual(struct.unpack_from("<I", d, 3 + 12)[0], M.dfu_crc32(d[:-4]))

    def test_cli(self):
        with tempfile.TemporaryDirectory() as t:
            s1, k, o, r = (os.path.join(t, n) for n in ("s1", "k", "o", "r"))
            open(s1, "wb").write(b"\1" * 64)
            open(k, "wb").write(b"\2" * 33)
            self.assertEqual(M.main(["--stage1", s1, "--payload", k, "--raw", r, "-o", o]), 0)
            img, raw = open(o, "rb").read(), open(r, "rb").read()
            self.assertEqual(img[0x600:0x600 + len(raw)], raw)
            # stage1 64 + length word 4 + kernel padded to 36, then the body is 16-aligned (112)
            self.assertEqual(len(raw), 112)
            self.assertEqual(struct.unpack_from('<I', img, 0xc)[0], 112)
            open(k, "wb").write(b"\2" * 0x30000)
            self.assertEqual(M.main(["--stage1", s1, "--payload", k, "-o", o]), 1)


if __name__ == "__main__":
    unittest.main()
