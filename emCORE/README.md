# emCORE kernel
emCORE is a lightweight alternative operating system for iPods (and possibly other devices one day).

The default `make` target in this checkout builds the minimal nano5g DFU image
at `build/ipodnano5g/emcore.dfu`. See [the build instructions](../README.md).
The feature list below describes the broader kernel; not every feature is
enabled in the nano5g configuration.

## Features
- Preemptive multitasking
- Can run multiple independent apps at the same time
- Shared library support
- USB debugging API
- FAT32 file system access
- LCD text console and graphics API
- Can run other kernels (such as [Rockbox](https://www.rockbox.org/)) through a kexec-like interface
- ~75KB executable size, ~110KB RAM usage (plus LCD frame buffer)

## Installation instructions
There's an installation wizard available on [this page](https://freemyipod.org/wiki/EmCORE_Installation).

## Uninstallation instructions
There's an uninstallation wizard available on [this page](https://freemyipod.org/wiki/EmCORE_Uninstallation).
