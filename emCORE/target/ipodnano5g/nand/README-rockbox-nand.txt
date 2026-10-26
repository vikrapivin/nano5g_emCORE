Shared nano4g / nano5g NAND engine
================================

Both targets include nand-controller.h, nand-read.h and nand-write.h from their
physical driver. Chip tables, clock/timing calculation, pin setup, chip reset
and geometry construction remain in the model directories. The FTL is shared
in ipodnano4g/ftl-*. The old nano4g sequencer sources remain as historical source;
its build now selects the same read/program/bootread sources as nano5g.

The new program.s is independently written GPL source, not an extracted Apple
program. It executes command/row descriptors produced by nand-plan.h. Sector
DMA, encoding and transmission are serialized, while NAND array operations can
remain busy on other CEs or use cache/multi-plane commands. Throughput is not
claimed equivalent to Apple's pipelined implementation.

Public interfaces are declared in ipodnano4g/nand-target.h (also used by nano5g).
All physical calls require external serialization, as before. A batch has at
most NAND_MAX_PAGES_PER_READ pages (16), with cache-line-aligned contiguous data.
Page size is the detected geometry's page_size, not the maximum buffer size.
Sequential buffers are plane-major within each stripe. row_base contains one
physical row per plane/bank pair. aligned and correct_bank are caller promises
about a request prepared according to the detected layout; setting them does
not rearrange physical rows into a valid multi-plane pair.

Mutation operations:
  nand_device_program       one erased page; bounces unaligned data
                            NULL metadata selects ten 0xa5 bytes
  nand_program_pages        scattered pages, normal 80/10 commands
  nand_program_sequential   layout-specific multi-plane/cache commands
  nand_device_erase         one block (dense block number, not a physical row)
  nand_erase_blocks         batch of dense block numbers/banks
  nand_program_boot_page    three BCH16 sectors, 1536 bytes, no metadata
  nand_read_boot_page        corresponding boot format read
  nand_transfer_raw         full physical data/spare stream, no ECC processing
  nand_get_last_failure_mask physical CE bits from last program/erase completion

The firmware contains these operations. BOOTLOADER builds omit mutation/raw/boot
page services and remain read-only. Normal reads and writes support 4/8-KiB
pages and BCH8/BCH16. A mutation is not retried after failure. Reset/re-identify
recovers the controller for subsequent requests. Raw write preserves OSOS's
ready-only completion semantics (it does not inspect NAND FAIL). Boot-page APIs
report ECC/program failures that Apple's CPU boot-page wrapper ignores.

Mutation parameter ABI (offsets within FMC/FMSS):
  d04 failing physical CE mask (output)
  d0c descriptor list: {command, row} pairs, zero command terminates
  d1c metadata pointer, 12 bytes/page; zero for boot/erase
  d20 DMA list: one bus address per 2048-byte chunk
  d2c ECC mode for sectors after sector zero
  d30 ECC mode for sector zero (normal pages include ten metadata bytes)
  d34 first sector parity/header length minus one
  d38 later sector parity length minus one
  d48 write timing, d4c read/status timing
  d50 used physical CE mask (also consumed by the CPU completion wait)
  d54 command, d58 row, d5c final CE, d60 status probe, d64 continuation
  d78 enhanced-status second-die row, zero for ordinary final status
  d7c number of sectors per programmed page (8/16 normal, 3 boot)

Keep all parameter/scratch state within the observed d00..d7c window. The
former d80..d94 layout depended on storage supplied only by the emulator:
with reads returning zero, a nonzero target row became row zero, and both
the sequencer status sweep and the CPU ready/FAIL check were skipped. The
nano5g standalone erase path also publishes its used CE mask at d50.

Descriptor bytes from most to least significant:
  status before reuse (0/70/71/78), setup (80/81 or erase 60),
  confirmation (10/11/15 or erase d0), physical CE bit mask.
Result r6: bit28 NAND failure, bit27 timeout/fault. Final status drains all used
CEs. Enhanced status probes both plane rows in both die address ranges.

Verification and known limits were recorded in the source project's note 25 (OSOS NAND reimplementation),
which is not part of this bundle.

Model exclusions
----------------
SOURCES builds exactly one model's NAND platform driver and chip table. The
IPOD_NANO5G branches in the shared target header and nand-write.h hold the
8 KiB buffer capacity, explicit row-hole/stride geometry and dense-block mapper;
nano4g omits them. Nano4g multiple-die addressing remains supported with its
dense row map. BCH16, boot/raw access, cache, multiplane and enhanced status are
shared chip capabilities and must not be removed just because a build is nano4g.
Both bootloader builds already exclude mutation/raw/boot-page services.

The separately staged nand-next candidate additionally supports per-model
population/clock-table preprocessing. See nand-next/README.txt; it has not
replaced this active runtime.
