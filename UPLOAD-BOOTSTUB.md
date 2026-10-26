# Uploading the bootstub with this emCORE

This bundle is the NAND **write primitive** for the iPod nano 5G: a
USB-connected recovery kernel (sent to the device in DFU mode) whose NAND
harness reads and writes the disk through the Rockbox FTL. This document is
the end-to-end procedure for installing the untethered Rockbox bootstub
(`../bootstub/`) with it. The generic device procedures (assumption
ladder, full NAND ladder, writes, module loading, troubleshooting) are in
[NAND-DIAG-HOWTO.md](NAND-DIAG-HOWTO.md) — this file only adds the
bootstub-specific steps.

## What you need

  * this emCORE bundle, built (`make`; output
    `recovery-image/emcore-nano5g.dfu`)
  * **wInd3x** — the one external tool, NOT included here: it sends the
    unsigned image to the device in DFU mode
  * the built bootstub artifacts from `../bootstub/`:
    `font/exploit_font.otf` — the exploit font produced by
    `make_exploit_font.py` against your own Firmware.MSE (see the
    bootstub README for the build pipeline)

## 1. Put the device in DFU mode and start emCORE

With the iPod in DFU mode (USB id `05ac:1231`):

    sudo wInd3x run recovery-image/emcore-nano5g.dfu

Expected: the LCD shows the emCORE console (second line
`emCORE nano5g build v0.1`, last line `No usable boot options, waiting
for USB commands`) and `lsusb` lists `ffff:e000`. Nothing touches the
NAND until a host command asks for it.

(On a freshly built image, run the assumption ladder first —
`tools/n5g_ladder.py` — per the HOWTO.)

## 2. Bring up the NAND harness

    .venv/bin/python tools/n5g_nand.py init --ftl

This runs the physical-layer steps and the FTL mount (step 4 is long —
it is the FTL scan; do not unplug while it runs). `mbr` afterwards must
show the three partitions: type `0x3e` (private, LBA 1..4), type `0x3f`
(firmware, LBA 64) and `0x0c` (FAT32 data).

## 3. Back up the target sectors

The exploit font lives at MSE offset **0x93FA00** (see the bootstub's
font/README.md). Device raw LBA = 64 + (mse_off >> 12), so the font
region is raw LBA **2431** with intra-sector byte offset **0xA00**, and
its 165,416 bytes span LBA **2431..2472** (42 sectors):

    .venv/bin/python tools/n5g_nand.py lread 2431 42 -o build/bootstub-orig-2431-2472.bin

Keep this file. Writing it back is the complete rollback for the
install (plus re-checking FirmwareStatus below).

## 4. Splice the font into the sectors

The font starts 0xA00 bytes into LBA 2431; the rest of each sector keeps
its original bytes. In Python:

```python
SECTORS = 42 * 4096          # device LBAs 2431..2472
FONT_OFF = 0xA00             # font start within LBA 2431
REGION_LEN = 165416           # the font slot: built font + zero padding;
                             # ends at LBA 2472 + 0xFF8
orig = bytearray(open('build/bootstub-orig-2431-2472.bin','rb').read())
font = open('../bootstub/font/exploit_font.otf','rb').read()
assert len(orig) == SECTORS and FONT_OFF + REGION_LEN <= SECTORS
patched = (orig[:FONT_OFF] + font
           + b'\0' * (REGION_LEN - len(font))      # pad to the slot end
           + orig[FONT_OFF + REGION_LEN:])
open('build/bootstub-sectors.bin','wb').write(patched)
```

(The padding zeroes the rest of the font's region slot, exactly the
`--patch-mse` region of the bootstub's font builder.)

## 5. Write and verify

    .venv/bin/python tools/n5g_nand.py write-open
    .venv/bin/python tools/n5g_nand.py lwrite 2431 build/bootstub-sectors.bin
    .venv/bin/python tools/n5g_nand.py lread 2431 42 -o build/bootstub-after.bin
    cmp build/bootstub-sectors.bin build/bootstub-after.bin

`lwrite` writes whole 4 KiB sectors through the Rockbox FTL and flushes;
`write-caps` shows what the harness allows (a build with
`N5G_NAND_WRITE=0` cannot write).

## 6. Check the boot gate (FirmwareStatus bit 1)

The untethered boot needs the private partition's FirmwareStatus bit 1
(the device's own state — Apple's OS sets it after a successful resource
mount; full background in `../bootstub/PRIV-PRVD.md`):

    .venv/bin/python tools/n5g_private_status.py show

Expected on a unit that last booted the iPod OS normally:
`PRVD v2 header, tag verified with AES selector 2` and
`FirmwareStatus 0x00000002: bit0 0, bit1 1`. If bit 1 is not set, the
tool can set it — the integrity tag is computed by the device's own
hardware AES, so this must happen on the unit:

    .venv/bin/python tools/n5g_private_status.py set 2

## 7. Reset and boot

Power-cycle the device. Cold boot, no USB, no buttons: the Apple boot
loads the patched resource font unvalidated (bit 1), the star
charstring delivers the stub and the bootstub payload, and the payload
mounts the disk, loads `/.rockbox/rockbox.ipod` and jumps into Rockbox.
The first storage_init runs the FTL scan — a few seconds; let it finish.

## Rollback

  1. `lwrite 2431 build/bootstub-orig-2431-2472.bin` (the section 3
     backup) restores the original resource font bytes.
  2. `n5g_private_status.py set 0` clears the gate (the Apple OS will
     set 2 again by itself after its next successful resource mount).
