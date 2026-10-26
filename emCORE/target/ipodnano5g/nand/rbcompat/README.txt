Rockbox build environment for the copied NAND driver (../*.c, ../*.h).
These headers replace the Rockbox firmware headers the driver includes
(config.h, system.h, kernel.h, cpu.h, s5l87xx.h, storage.h, logf.h, mv.h,
clocking-s5l8702.h, cpucache-arm.h, nand.h) with the subset it uses, mapped
onto emCORE primitives by rbcompat.c (mutexes, cache maintenance, the tick,
clock gates). Nothing here changes the driver sources themselves.
