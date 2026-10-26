/*
 * iPod nano 5G (S5L8730) emCORE memory layout (docs/08-emcore-port-map.md section 3).
 *
 *   0x08000000  INIT   : the image as loaded by stage 1 (tools/n5g_mkimage.py appends emcore.bin behind stage1.bin;
 *                        stage1.S copies it to 0x08000000). .inithead/.init/.inittail run from here once; the load
 *                        images of .sram/.sdram sit here too and are copied out by crt0. After init the whole range
 *                        .._poolend is the malloc pool (_poolstart.._poolend).
 *   0x22000000  SRAM   : .sram (vectors at 0x22000000 = VA 0 through the stage-1 page table, ICODE/IDATA), .ibss
 *                        (IRQ/abort stacks). MUST stay below 0x2202c000: stage1.S builds the first-level page table
 *                        at 0x2202c000..0x2202ffff (16 KiB) and crt0 keeps it as TTBR0 for the whole run (as the 4G
 *                        bootstub did). The ROM's SRAM (DFU buffer 0x22000000..0x22025200, BSS/heap/stacks up to
 *                        0x2202ff.., docs/01 section 1 / docs/02 section 7) is dead once stage 1 runs; nothing is placed
 *                        at 0x22030000.. (448 KiB SRAM is inferred only, docs/02 section 4).
 *   0x0bf00000  SDRAM  : .sdram (.text/.rodata/.data) + .bss, top of the 64 MiB (docs/02 section 2.5: base 0x08000000,
 *                        size 0x04000000, bl_PreEfi:0x2200b4b4). Same scheme as the 4G (which used the top of 32 MiB).
 *                        832 KiB: the Rockbox FTL (target/ipodnano5g/nand/ftl-nano4g.c) keeps its tables in .bss.
 *   0x0bfd0000  LCD    : .lcdfb, the console framebuffer 240*376*2 = 0x2c100 B, up to the SDRAM end 0x0c000000.
 *
 * PROVISIONAL: the 64 MiB size comes from PreEfi's constants (observed) and the stage-1 pattern test of 0x0bf00000
 * (docs/08 ladder R2 reads the controller registers back). Everything above 0x0a000000 is unused until the first
 * boot proves it.
 */
ENTRY(__start)
OUTPUT_FORMAT(elf32-littlearm)
OUTPUT_ARCH(arm)
STARTUP(build/ipodnano5g/target/ipodnano5g/crt0.o)

MEMORY
{
    INIT : ORIGIN = 0x08000000, LENGTH = 0x03f00000
    SRAM : ORIGIN = 0x22000000, LENGTH = 0x0002c000   /* below the stage-1 page table at 0x2202c000 */
    SDRAM : ORIGIN = 0x0bf00000, LENGTH = 0x000d0000   /* .text/.rodata/.data/.bss, up to the LCD fb */
    LCD  : ORIGIN = 0x0bfd0000, LENGTH = 0x00030000
}

SECTIONS
{
    .inithead :
    {
        _initheadstart = .;
        _poolstart = .;
	*(.inithead*)
        . = ALIGN(0x4);
        _initheadend = .;
    } > INIT

    .sram :
    {
        _sramstart = .;
        KEEP(*(.intvect))
        *(.intvect)
        *(.icode*)
        *(.irodata*)
        *(.idata*)
        . = ALIGN(0x4);
        _sramend = .;
    } > SRAM AT> INIT
    _sramsource = LOADADDR(.sram);

    .sdram :
    {
        _sdramstart = .;
        _poolend = .;
        *(.text*)
        *(.glue_7)
        *(.glue_7t)
        . = ALIGN(0x4);
        *(.rodata*)
        . = ALIGN(0x4);
        *(.data*)
        . = ALIGN(0x4);
        _sdramend = .;
    } > SDRAM AT> INIT
    _sdramsource = LOADADDR(.sdram);

    .init :
    {
        _initstart = .;
        *(.initcode*)
        *(.initrodata*)
        *(.initdata*)
        . = ALIGN(0x4);
        _initend = .;
    } > INIT

    .inittail :
    {
        _inittailstart = .;
        *(.inittail*)
        . = ALIGN(0x4);
        _inittailend = .;
    } > INIT

    .ibss (NOLOAD) :
    {
        _ibssstart = .;
        *(.ibss*)
        . = ALIGN(0x4);
        _abortstackstart = .;
        . += 0x400;
        _abortstackend = .;
        _irqstackstart = .;
        . += 0x400;
        _irqstackend = .;
        *(.stack*)
        . = ALIGN(0x4);
        _ibssend = .;
    } > SRAM

    .bss (NOLOAD) :
    {
        _bssstart = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(0x4);
        _bssend = .;
    } > SDRAM

    .lcdfb (NOLOAD) :
    {
        _lcdfbstart = .;
        *(.lcdfb*)
        . = ALIGN(0x4);
        _lcdfbend = .;
    } > LCD

    /DISCARD/ :
    {
        *(.eh_frame)
    }

    /* stage1.S builds the first-level page table at 0x2202c000 and it stays the live TTBR0 (crt0 only enables the
       MMU). .ibss must end below it or clearibss zeroes live descriptors. */
    ASSERT(_ibssend <= 0x2202C000, ".ibss overflowed into the 0x2202C000 stage-1 MMU page table (SRAM budget)")

    /* framebuffer must fit its pinned region and stay clear of the 64 MiB SDRAM end */
    ASSERT(_lcdfbend <= 0x0C000000, ".lcdfb (LCD framebuffer) overflowed past the 0x0C000000 SDRAM end")

    /* the ROM's DFU buffer is 0x25200 B and its datalen check is < 0x24c01 (docs/01, docs/02 section 7): the load
       image (.inithead..inittail) plus stage 1 (~1.5 KiB) must fit. tools/n5g_mkimage.py enforces the exact limit. */
    ASSERT(_inittailend - 0x08000000 <= 0x24000, "emcore.bin exceeds the ROM DFU budget (0x24c00 minus stage 1 and the IMG1 footer)")
}
