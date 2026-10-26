#!/usr/bin/env python3
"""Load a nano5g SDRAM module (*.n5m, e.g. the Rockbox boot probe) into a booted emCORE kernel.

Requires a freshly booted emcore-nano5g.dfu. The module remains until reset;
loading it performs no NAND I/O. Trusted native modules only.
"""
import argparse
import pathlib
import struct
import zlib

MAGIC, ABI, MAX_SIZE = 0x4d47354e, 1, 2 * 1024 * 1024
# ABI 2 appends peripheral leases (module5g.h); only a lease kernel activates it.
ABIS = (1, 2)
HEADER = struct.Struct('<10I')
INFO, RESERVE, ACTIVATE = 132, 133, 134


def parse_module(data):
    if len(data) < HEADER.size or len(data) > MAX_SIZE:
        raise ValueError('invalid module file size')
    h = HEADER.unpack_from(data)
    magic, abi, kind, text, bss, entry, command, heartbeat, count, reserved = h
    if (magic, kind, reserved) != (MAGIC, 1, 0) or abi not in ABIS:
        raise ValueError('unsupported module format, ABI or kind')
    if text < 4 or text % 4 or text + bss > MAX_SIZE or len(data) != HEADER.size + text + count * 4:
        raise ValueError('invalid module section sizes')
    if any(off % 4 or off >= text for off in (entry, command, heartbeat)):
        raise ValueError('invalid module entry points')
    previous = -1
    for (off,) in struct.iter_unpack('<I', data[HEADER.size + text:]):
        if off % 4 or off > text - 4 or off <= previous:
            raise ValueError('invalid or duplicate relocation')
        if struct.unpack_from('<I', data, HEADER.size + off)[0] > text + bss:
            raise ValueError('relocation points outside module')
        previous = off
    return h


def load(session, data, expected_caps=()):
    """Load once and (optionally) check the module's command-128 capability prefix.

    Callers with an extensible registry can check magic/version here, then
    validate the advertised registry count and layout when they read it.
    The default still checks all three capability words exactly.
    """
    h = parse_module(data)
    state, _, _ = session.cmd(INFO)
    if state:
        raise ValueError('a module is already staged/loaded; reset before loading another')
    status = session.status()
    if status.busy or status.jobs_started:
        raise ValueError('load the module before the NAND ladder; reset emCORE first')
    size = max(len(data), h[3] + h[4])
    rc, addr, cap = session.cmd(RESERVE, size)
    if rc or cap != size or addr % 32 or not 0x08000000 <= addr < 0x0c000000 - size:
        raise ValueError('module allocation rejected (rc=%d)' % rc)
    session.write_memory(addr, data)
    if session.read(addr, len(data)) != data:
        raise ValueError('module upload read-back mismatch; reset before retrying')
    rc, _, _ = session.cmd(ACTIVATE, len(data), zlib.crc32(data) & 0xffffffff)
    if rc:
        raise ValueError('module activation rejected (rc=%d); reset before retrying' % rc)
    if session.cmd(INFO)[0] != 2 or session.cmd(128)[:len(expected_caps)] != expected_caps:
        raise ValueError('module did not publish the expected capabilities')
    return addr


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('module', type=pathlib.Path)
    a = p.parse_args()
    session = None
    try:
        data = a.module.read_bytes()
        parse_module(data)  # Fail before connecting on malformed input.
        import os, sys
        here = os.path.dirname(os.path.abspath(__file__))
        if here not in sys.path:
            sys.path.insert(0, here)
        import n5g_nand as N
        session = N.make_write_session(ep0=True)   # nano 5G check, EP0 only, no USB retry/replay
        addr = load(session, data, expected_caps=())  # a module's command-128 words are its own business
        print('module loaded into SDRAM at 0x%08x and activated (it holds its leases until reset)' % addr)
    except Exception as exc:
        p.exit(1, '%s\n' % exc)
    finally:
        if session is not None: session.em.close()


if __name__ == '__main__':
    main()
