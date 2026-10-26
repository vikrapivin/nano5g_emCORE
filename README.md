# emCORE for the iPod nano 5G, standalone bundle

A self-contained emCORE for the iPod nano 5G (S5L8730) whose NAND stack is the **Rockbox nano 5G NAND driver**. It
builds alone: nothing in this directory refers to a path outside it. The Rockbox driver sources are *copies*
(`emCORE/target/ipodnano5g/nand/`), not links.

What the kernel contains: USB monitor and DFU-less boot (stage 1 + kernel in one IMG1), LCD console, I2C/PMU,
backlight, hardware AES, click wheel poller, the module ABI 2 with the I2C / LCD / NAND / WHEEL leases, and the NAND
harness on the Rockbox driver: physical layer, boot pages, FTL mount, logical reads and logical writes through the
Rockbox FTL. On top of it, a host tool reads and sets the private partition's FirmwareStatus. Nothing touches the
NAND at boot; nothing is written without a write command from the host. The startup banner reads
`emCORE nano5g build v0.1` (`NANO5G_BUILD_VERSION` in `emCORE/version.h`; the version reported over USB is the
upstream emCORE one).

## Build

Requirements: GNU make, `arm-none-eabi-gcc` with newlib headers, a host C compiler (for `tools/fmssasm.c`), Python 3.
`setup.sh` installs them on Debian/Ubuntu and creates a venv with PyUSB for the host tools.

```sh
make -j2            # = make n5g-image
```

Outputs in `recovery-image/`: **`emcore-nano5g.dfu`** (IMG1 "8730", format 4, unsigned; the body must stay under the
ROM's 0x24c00 data limit, the packer prints it), `emcore-nano5g.bin` (raw stage 1 + kernel), `emcore-nano5g.nm`
(symbol map for the host tools). `make n5g-check` runs the packer's self-test. `make clean` removes `emCORE/build`.

Switches: `N5G_NAND_DISABLE=1` (no NAND harness), `N5G_NAND_WRITE=0` (read-only harness; by default
`ftl_write`/`ftl_flush` are reachable through USB commands 129..131), `N5G_NOCACHE=1`, `N5G_BACKLIGHT_MAX=0xNN`,
`N5G_BACKLIGHT_BOOT=0xNN`, `IMG1_FORMAT=3|4`, `IMG1_CERT_LEN=0x...`. `CROSS=`, `HOSTCC=`, `PYTHON=` override the
tools.

## Layout

| Path | Content |
|---|---|
| `emCORE/` | kernel: shared sources, `target/ipodnano5g/` (stage 1, crt0, drivers, `module5g.c` leases, `nand5g.c` harness), `target/ipodnano5g/nand/` (Rockbox driver copy, `rbcompat/` header subset, `rbcompat.c` shims, `emcshim/` stdint shim for the emCORE side), `ls.x` memory map |
| `tools/` | host side: `libemcore.py` (+ `libemcoredata.py`, `misc.py`, `usb_cmds.py`), `n5g_usbsession.py` (USB transport, MBR/firmware-directory parsers, READ ID decoder), `n5g_ladder.py` (assumption ladder), `n5g_nand.py` (NAND harness: reads, writes), `n5g_private_status.py` (private header check, FirmwareStatus 0/2), `n5g_module.py` (module loader), `n5g_img1_decrypt.py` (hardware AES), `n5g_mkimage.py` / `n5g_mkmodule.py` (packers) + `test_n5g_mkimage.py`, `fmssasm.c` (FMSS microcode assembler, built by make) |
| `recovery-image/` | build outputs |
| `udev/`, `requirements.txt`, `setup.sh` | host setup |
| `NAND-DIAG-HOWTO.md` | device procedure: assumption ladder, NAND ladder, writes, FirmwareStatus, module loading, troubleshooting |
| `COPYING` | GNU GPL version 2 |

## How the Rockbox driver is built here

`target/ipodnano5g/target.mk` compiles `nand/nand-nano5g.c` and `nand/ftl-nano4g.c` with the include path
`nand/rbcompat` first, so the driver sees a Rockbox-shaped `config.h`, `system.h`, `kernel.h`, `storage.h`, `s5l87xx.h`
subset instead of emCORE's headers (`nand/rbcompat/README.txt`). The five FMSS programs are assembled into
`fmss-nano5g.h` by `tools/fmssasm`. `nand/rbcompat.c` maps the driver's primitives onto emCORE: mutexes (`thread.h`),
`udelay`/`USEC_TIMER` (`timer.h`), `commit_dcache_range`/`discard_dcache_range` (`mmu.h`), the 100 Hz tick, and
`clockgate_enable` (same gate numbering). The driver is built in its main-firmware shape (no `BOOTLOADER`), so its
boot-page and write entry points exist; `N5G_NAND_WRITE=0` leaves the writer out of the harness.

The FTL keeps its tables in `.bss` (about 230 KiB); `ls.x` therefore places the kernel at `0x0bf00000..0x0bfd0000`
(832 KiB) instead of the former 320 KiB window. Stage 1 still pattern-tests `0x0bf00000` before the copy.

## NAND harness (USB commands 120..131, `tools/n5g_nand.py`)

| Step / job | What runs |
|---|---|
| step 1 | FMC + ECC clock gates (PWRCON0 bits 5 and 9) through `clockgate_enable`, before/after words recorded |
| step 2 | snapshot of the NAND pad words `0x3cf000c0/e0/100` (the driver programs the pins itself in step 3) |
| step 3 | `nand_phys_init()`: pins, chip reset, READ ID per CE, chip table, timing → `chip_ids[]`, `geom[]` |
| step 4 | `ftl_init()`: VFL + FTL mount → `ftl_pages` 4 KiB sectors (`ftl_num_sectors()`), `ftl_bpp` 4096 |
| romread | `nand_read_boot_page(0, row)`: the three BCH16 boot sectors, 1536 bytes |
| rawread | `nand_read_page(bank, row)`: data + 12 meta bytes; the verdict is per page (ok / erased / uncorrectable) |
| lread, dump | `ftl_read()` in 4 KiB sectors; `--ftl` is the same view on this harness |
| mbr | partition table, firmware-partition directory, private header (format, FirmwareStatus, stored tag, unverified) |
| get-struct | refused (`BAD_ARG`, detail `0x80000007`): a service of the earlier Apple-library harness |
| write-caps (128) | `{protocol 1, writer built in?, max sectors per write}`; answered by read-only kernels too |
| write-open, lwrite, flush (129..131) | write open (`nand_write_supported`), block write (`ftl_write` of staged 4 KiB sectors, CRC-32 checked on the device first), flush (`ftl_flush`). Any logical sector, MBR and firmware partition included. A failed mutating job poisons the writer until the next reset; the host never retries or replays a write command |

The status record (`struct n5g_nand_status`, version 1) keeps the layout of the earlier harness; `geom[6]` holds the
driver's layout word (`NAND_LAYOUT_*`), `geom[15]` is 0, `fil_pages` counts pages the driver handed to the sequencer.

```sh
sudo wInd3x run recovery-image/emcore-nano5g.dfu                              # the image is unsigned: wInd3x (external tool), not plain DFU
.venv/bin/python tools/n5g_ladder.py list                                     # assumption ladder (24 rungs, one per command)
.venv/bin/python tools/n5g_nand.py init --ftl                                 # steps 1..4, then `id`
.venv/bin/python tools/n5g_nand.py mbr
.venv/bin/python tools/n5g_nand.py lread 0 8 -o build/first.bin
.venv/bin/python tools/n5g_nand.py write-caps
.venv/bin/python tools/n5g_nand.py lwrite 2048 build/sectors.bin              # whole 4 KiB sectors; flush + read-back
.venv/bin/python tools/n5g_private_status.py show                            # PRVD header, tag verified on the device
.venv/bin/python tools/n5g_private_status.py set 2                           # or set 0; no-op when already equal
```

## Private partition status (`tools/n5g_private_status.py`)

The MBR partition of type 0x3e (LBA 1..4 on the device) starts with the header the iPod OS writes after a successful
resource mount: `PRVD`, 2, 0, 4, the u32 FirmwareStatus at +0x40 (2 = the bootloader's boot policy skips the
resource-validation request) and a 16-byte tag at +0x80 = AES-128-CBC with hardware key selector 2 (zero IV) over
the first 16 bytes of SHA-1(header bytes 0..0x7f). `show` reads the header and verifies the tag through the kernel's
AES command (the key never leaves the device); `set 0|2` rebuilds the whole sector as the OS does and writes it
through the FTL writer, then flushes and reads it back. Only a header whose present tag verifies is rewritten, an
equal status is a read-only no-op, and a changed MBR or header between inspection and write-open cancels the write.
Run the read-only ladder (`init --ftl`) first. Procedure with checks: `NAND-DIAG-HOWTO.md` section 5.

## Verification status

This kernel and these tools were run against a software model of the SoC and NAND (kept outside the bundle): the
read ladder (steps 1..4, boot and raw pages, logical reads, MBR, a file's CRC through the FTL), the write commands
(`ftl_write`/`ftl_flush`, 65-sector chunking, `lwrite`/`flush`, the CRC guard) and the full `n5g_private_status.py`
round trip (status 1 -> 2 -> 0, no-op cases, bad-tag refusal, with the model's stand-in hardware key), after which
the file still read back with the same CRC; on an unmountable fixture the FTL mount and write-open are refused
cleanly. The Rockbox FTL writer this kernel calls has written on a device from Rockbox itself. Nothing in this
bundle has been run on a device. Device rules: USB is the only channel,
nothing survives a reset, read-only ladder first, backlight at or below 0x40 until the battery state is known.

