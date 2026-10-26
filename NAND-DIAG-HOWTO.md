# nano 5G NAND diagnostics and writes with emCORE (this bundle)

How to check this bundle's emCORE image on an iPod nano 5G, read the NAND without writing, write logical sectors
through the Rockbox FTL, and read or set the private partition's FirmwareStatus. Commands run from the bundle root.
`.venv/bin/python` is the virtual environment `setup.sh` creates (PyUSB); `sudo` is needed only if your udev rules
do not grant access (`udev/50-ipod-nano5g.rules` does).

## Rules

* Nothing survives a reset. Read every status record BEFORE the next step; the host output is the only record.
* Read-only first. Sections 2 and 3 never write to the NAND. Sections 4 and 5 do. Saving the sectors you are about
  to change (`lread ... -o FILE`) is recommended, not enforced; `lwrite` puts them back.
* Backlight stays at or below `0x40`; use a unit with a healthy battery for long NAND runs (the FTL mount reads
  about 20,000 pages).
* Assume no emCORE primitive works on a new image until the assumption ladder (section 2) has passed on it.
* One step per command; stop at the first deviation and keep the output.
* Never re-send a write command after a USB error: it may already have run. Read `status` first (section 7).
* One module per boot. A loaded module (`*.n5m`, section 6) keeps the NAND lease; the harness refuses NAND jobs
  while it holds it. A NAND session and a module session are separate boots.

## 1. Image

| Image | Content |
|---|---|
| `recovery-image/emcore-nano5g.dfu` | this bundle's kernel: USB monitor, LCD console, I2C/PMU, backlight, hardware AES, click wheel, module ABI 2 (I2C / LCD / NAND / WHEEL leases), NAND harness on the Rockbox nano 5G driver (reads and, unless built with `N5G_NAND_WRITE=0`, writes) |

The image is unsigned. It is sent to the device in DFU mode (USB id `05ac:1231`) with the external tool wInd3x:

```sh
sudo wInd3x run recovery-image/emcore-nano5g.dfu
```

Expected: the LCD shows the console, second line `emCORE nano5g build v0.1`, last line
`No usable boot options, waiting for USB commands`; `lsusb` lists `ffff:e000`. Plain DFU senders cannot boot an
unsigned image.

## 2. Assumption ladder (any new image): `tools/n5g_ladder.py`

Checks, one test per command, the assumptions the port makes (the values marked PROVISIONAL in the sources) and the
emCORE primitives later work relies on. Run it on a freshly booted image before loading any module or starting the
NAND ladder; it never loads a module and never touches the NAND.

```sh
.venv/bin/python tools/n5g_ladder.py list         # every rung in order, its purpose, its last result here
.venv/bin/python tools/n5g_ladder.py identity     # run ONE rung; after a PASS it prints the next command
.venv/bin/python tools/n5g_ladder.py bootreport   # ... and so on, one rung per command, in list order
```

**Everything comes back over USB.** Each rung prints its checks (`[PASS]`/`[FAIL]`/`[info]`), the device's own
records (`n5g_init_report`, `lcd_panel_info`, `i2c_timeouts`, `accel_whoami`, click-wheel counters) and the device
console text written while it ran (`[console] ...`, including any `*PANIC*`). `bootreport` returns the whole boot
log, so the LCD does not have to be read. Two files collect it in the current directory:

* `n5g-ladder-report.txt`: every printed line, appended per run (send this one);
* `n5g-ladder.jsonl`: one start and one end record per rung (verdict, values, console), fsynced before the device
  acts; `list` reads it to show what already passed.

**If the device stops answering**, the rung ends with `NO ANSWER`. The last `[plan]` line names the step that was
running. Only then read the LCD (its last lines are the device's own report) and note whether the iPod came back
by itself (reset) or stayed silent (hang); `lsusb` tells. Re-boot the image and continue with the next rung: the
rungs are independent, each one rechecks what it needs.

| # | Rung | Checks | Writes |
|---|---|---|---|
| 0 | identity | GET VERSION (nano5g), timer runs, live kernel code = the ELF, vectors, no module loaded | no |
| 1 | bootreport | `n5g_init_report` (boot clocks, CP15 SCTLR MMU/caches, boot PMU) + the full console log | no |
| 2 | clocks | CLKCON0/1, PLL0PMS + lock, CLKCON2 (LCD); PWRCON gates vs stage 1 (open item, reported) | no |
| 3 | usec | timer 4 really counts 1 MHz (PROVISIONAL), against the host clock, +-2 % | no |
| 4 | usbcaps | GHWCFG1..4 vs the 4G-derived endpoint set; the FIFOs emCORE programs (TX0/1/3, RX) hold the `target.h` sizes and fit the core (PROVISIONAL); TX2 is unused (endpoint 2 is OUT) and only reported | no |
| 5 | chipid | chip-id words (device-only fact); PHY+4 = 0x10 \| CHIPID+8 bit 0 (PROVISIONAL) | no |
| 6 | sdram | controller registers as stage 1 left them; 64 MiB: kernel code not mirrored 16/32/48 MiB lower | no |
| 7 | pagetable | all 4096 L1 entries exactly as stage 1 wrote them, SDRAM cacheability | no |
| 8 | threads | scheduler ABI 3, kernel threads present, none killed, stack guards intact | no |
| 9 | lcd | panel detected and initialised, no LCD timeouts or DMA aborts, controller mode | no |
| 10 | pmu | PMU on bus 0 / 0xe6: backlight <= 0x40, warm flag cleared, USB limit 500 mA, no I2C timeouts | no (I2C reads) |
| 11 | accel | accelerometer on bus 0 / 0x32 answers WHO_AM_I 0x12 (PROVISIONAL bus) | no (I2C read) |
| 12 | wheel | click-wheel init result and poller replies (PROVISIONAL protocol); turn the wheel meanwhile | no |
| 13 | tail | free SDRAM above `.bss` (below the LCD framebuffer): unchanged over time, WRITE_MEM + read-back | yes, free RAM |
| 14 | sram | free SRAM above `.ibss` (below the page table): same | yes, free RAM |
| 15 | flush | FLUSH CACHE command | no |
| 16 | sramcode | a thread runs code from free SRAM and exits | yes |
| 17 | tailcode | a thread runs code from cached SDRAM after FLUSH CACHE | yes |
| 18 | sleep | the kernel's own `sleep()` (20 and 100 ms) against the microsecond timer: scheduler timer B rate (timer.c PROVISIONAL; the device reads its PRE back as 0) | yes |
| 19 | icache | overwritten code + FLUSH CACHE runs the new code (ARM1176 erratum 411920 concern) | yes |
| 20 | dcache | entire-D-cache clean repeated until the dirty-status bit clears (the N33/Rockbox loop) | yes |
| 21 | aes | hardware AES, user key, FIPS-197 encrypt and decrypt (key byte order) | yes |
| 22 | memtest | device-side write/verify of the whole free SDRAM tail (`--size` limits it) | yes |
| 23 | sramsize | **explicit, run last:** SRAM above 0x22030000 (448 KiB inferred). If it is absent the access faults and the device stops answering: that silence is the answer (192 KiB) | yes, probe |
| 24 | udf | **explicit, run last:** an undefined instruction kills only the ladder thread; device keeps answering | yes |

Safety built in: the writing rungs only use memory the kernel layout (from `--elf`) leaves unused, and recheck that
the booted kernel matches that ELF first (`--skip-identity-check` bypasses this). Every READ/WRITE goes only to
known-safe windows (RAM and the clocked register blocks the ladder needs): an access that faults inside the USB
monitor would wedge the device, so anything else is refused on the host. Writes are single 64-byte USB packets.
`--elf` defaults to `emCORE/build/ipodnano5g/emcore.elf`, the build of `recovery-image/emcore-nano5g.dfu`; pass the
ELF of whatever image you booted. AES self-test of the IMG1 tooling: `.venv/bin/python tools/n5g_img1_decrypt.py --self-test`.

## 3. NAND ladder (read-only): `tools/n5g_nand.py`

Each step is one command; the device runs it as a bounded job on a worker thread and leaves a status record
(`status` decodes it). Nothing here writes.

| Step | Command | Expect |
|---|---|---|
| N1 | `status` | record `N5GN` v1, every step `-`, `jobs_started 0` |
| N2 | `step 1`, then `step 2` | FMC + ECC clock gates (PWRCON0 bits 5 and 9) opened, before/after words shown; snapshot of the NAND pad words |
| N3 | `step 3`, then `id` | the driver's `nand_phys_init`: pins, chip reset, READ ID per CE, chip-table lookup. Two populated CEs with the same ID are expected on the nano 5G (the rest zero); geometry and layout word printed |
| N4 | `romread 0`, then `rawread BANK PAGE` | boot-area page 0 of bank 0 (the IMG1 header `49 4d 47 31` or the driver's verdict BLANK/OK/UNCORRECTABLE); one raw page with its 12 meta bytes and verdict |
| N5 | `ftl-open` | step 4, the driver's `ftl_init` (VFL + FTL mount), long; `status` shows progress. Afterwards `ftl_pages` = number of 4 KiB sectors |
| N6 | `lread 0`, `mbr`, `dump -o FILE --start S --count N` | MBR with `55 aa`; three partitions: type `0x3e` (private, LBA 1, 4 sectors), `0x3f` (firmware, LBA 64; its "STOP sign" directory is listed) and `0x0c` (FAT32 data). `mbr` also prints the private header (format, FirmwareStatus, stored tag, unverified) |

`init` runs steps 1..3 (`init --ftl` also step 4). A failing step ends as a record with a result code
(`LIB_ERROR` with the driver's own code, `PRECOND`, `BAD_ARG`) and a register snapshot, not as a dead device; read
it with `status` before anything else. `get-struct` is refused on this kernel (it was a service of the earlier
Apple-library harness).

## 4. Writing logical sectors: `write-caps`, `lwrite`, `flush`

Writes go through the Rockbox FTL (`ftl_write`, `ftl_flush`) inside the kernel: any logical 4 KiB sector, the MBR
and the firmware partition included. The host stages whole sectors in the harness buffer, the device checks their
CRC-32 before the first NAND operation, and the FTL does the rest (allocation, metadata, erases of free blocks on
flush). A failed mutating job poisons the writer until the next reset (`status` shows which job failed and why).

1. Capability check (works on read-only kernels too):
   ```sh
   .venv/bin/python tools/n5g_nand.py write-caps
   ```
   Check: `writer protocol 1, writer built in, up to 64 sectors (256 KiB) per write command`. "NOT built in" means
   the kernel was built with `N5G_NAND_WRITE=0`.
2. Read-only ladder up to the FTL mount, if not done in this boot:
   ```sh
   .venv/bin/python tools/n5g_nand.py init --ftl
   ```
   Check: steps 1..4 `ok`, `id` prints two CEs and the geometry.
3. Save what you are about to overwrite (recommended):
   ```sh
   .venv/bin/python tools/n5g_nand.py lread LBA COUNT -o build/orig-LBA.bin
   ```
   Check: the file is `COUNT x 4096` bytes.
4. Write. `FILE` must be a whole number of 4 KiB sectors; the tool opens the FTL for writing, writes, flushes and
   reads the range back:
   ```sh
   .venv/bin/python tools/n5g_nand.py lwrite LBA FILE
   ```
   Check: `writing N sector(s) at LBA ...`, `flushed`, `read back: identical`. `--no-flush` leaves the FTL metadata
   uncommitted (follow with `flush`); `--no-verify` skips the read-back.
5. Independent check:
   ```sh
   .venv/bin/python tools/n5g_nand.py lread LBA COUNT -o build/after.bin && cmp FILE build/after.bin
   ```
6. To undo: `lwrite LBA build/orig-LBA.bin`.

Result codes of a write job in `status`: `PRECOND` = FTL not mounted, writer not opened, or writer poisoned by an
earlier failure; `BAD_ARG` = range outside the FTL or CRC mismatch of the staged data (nothing written);
`LIB_ERROR` = the driver refused or failed (`job_lib_rc` is its code; `0xfffffff9` = WRITE, `0xfffffffa` = FTL).

## 5. Private partition FirmwareStatus: `tools/n5g_private_status.py`

The MBR partition of type `0x3e` (LBA 1..4 on the device) begins with the header the iPod OS writes after it has
mounted its resources: `PRVD`, 2, 0, 4 (16 bytes), the u32 FirmwareStatus at +0x40, and at +0x80 a 16-byte tag =
AES-128-CBC with the device's hardware key selector 2 and a zero IV over the first 16 bytes of SHA-1(header bytes
0..0x7f). Status 2 (bit 1) tells the Apple bootloader's boot policy to skip the resource-validation request; status 0
clears that, and the OS sets 2 again after its next successful resource mount. The tag is computed on the device
through emCORE's AES command; the key never leaves it.

1. Read-only ladder up to the FTL mount (section 4 step 2) in this boot.
2. Show and verify:
   ```sh
   .venv/bin/python tools/n5g_private_status.py show
   ```
   Check: `private partition at LBA 1 (4 sectors); PRVD v2 header, tag verified with AES selector 2`, then
   `FirmwareStatus 0x00000002: bit0 0, bit1 1 (Bds skips the rsrc validation request)` on a unit that last booted
   the iPod OS normally, and the stored tag. An `ERROR: private-header integrity tag mismatch` means the header was
   not written by this device's OS (or is damaged): the tool will not rewrite it. `legacy/malformed headers are not
   writable here` means a version-1 or unknown header.
3. Set (only 0 and 2 are accepted; an equal value is a read-only no-op):
   ```sh
   .venv/bin/python tools/n5g_private_status.py set 0
   ```
   Check: `FirmwareStatus at LBA 1: 0x00000002 -> 0x00000000 (written, flushed, read back)`. The whole sector is
   rebuilt as the OS does (zeroes outside the header, bit 0 included). The write is cancelled if the MBR or the
   header changed between the inspection and the FTL write-open.
4. Confirm:
   ```sh
   .venv/bin/python tools/n5g_private_status.py show
   ```
   Check: `FirmwareStatus 0x00000000: bit0 0, bit1 0 (no Bds shortcut)`. `set 2` is the reverse.

The tool runs the AES self-test (FIPS-197 and SP 800-38A vectors with a user key) before touching the header, so a
wrong AES setup fails before any NAND access.

## 6. Loading a module: `tools/n5g_module.py`

A module (`*.n5m`, packed by `tools/n5g_mkmodule.py`) is loaded into free SDRAM of a freshly booted kernel and runs
until reset; loading performs no NAND I/O. Modules of ABI 1 and 2 are accepted; an ABI 2 module takes the I2C, LCD,
NAND and WHEEL leases it asks for, and the harness refuses NAND jobs while the NAND lease is held. Run the ladder
(section 2) first, load nothing else and start no NAND job in that boot:

```sh
.venv/bin/python tools/n5g_module.py FILE.n5m
```

The tool checks the module header (magic, ABI 1 or 2, size), asks the kernel for the slot (USB command 132), uploads
the image (133) and activates it (134). Check: it prints the kernel's answers without an error; the LCD console
shows the module's own output; a kernel that cannot grant a requested lease rejects the module (rc 4) and stays
usable. Only load native modules you trust: they run with the kernel's privileges.

## 7. When something goes wrong

* A NAND job never reports or a command fails: while USB answers,
  `.venv/bin/python tools/n5g_nand.py status` prints the record (state, job, result, the driver's code, the
  register snapshot taken on failure, pages read, panics); `.venv/bin/python tools/n5g_ladder.py bootreport`
  prints the device console, including any `*PANIC*` text.
* A USB error during a write session: do not re-send. `status` tells whether the job ran. `lread` the target range
  to see what landed. `flush` can be repeated safely; `lwrite` only after `lread` showed what is there.
* The device disappears from USB: write down the last command, the last LCD text, and whether it came back in DFU
  by itself (reset) or stayed silent (hang); `lsusb` tells.
* After a reset the kernel starts clean: nothing loaded, the FTL not mounted, the writer closed. Start again at the
  ladder; the NAND content is whatever the last flush committed.
