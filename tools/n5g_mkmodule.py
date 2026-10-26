#!/usr/bin/env python3
"""Pack a zero-linked ARM ELF (--emit-relocs) as a nano5g SDRAM module.

Only ABS32 addresses need a runtime fixup. Relative branches remain unchanged;
unknown relocations, undefined symbols and absolute external references fail.
"""
import argparse
import pathlib
import struct
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / 'emCORE/tools'))
from n5g_module import HEADER, MAGIC, ABI, parse_module


def pack_elf(data, abi=ABI):
    if data[:7] != b'\x7fELF\x01\x01\x01':
        raise ValueError('expected little-endian ELF32')
    h = struct.unpack_from('<HHIIIIIHHHHHH', data, 16)
    if h[0:2] != (2, 40):
        raise ValueError('expected linked ARM executable')
    shoff, shsize, shcount, strindex = h[5], h[10], h[11], h[12]
    if shsize != 40:
        raise ValueError('invalid section table')
    sections = [struct.unpack_from('<10I', data, shoff + i * shsize) for i in range(shcount)]
    def contents(s):
        out = data[s[4]:s[4] + s[5]]
        if len(out) != s[5]: raise ValueError('truncated ELF section')
        return out
    strings = contents(sections[strindex])
    def string(table, off):
        return table[off:table.index(0, off)].decode('ascii')
    names = {string(strings, s[0]): i for i, s in enumerate(sections)}
    ti, bi = names['.text'], names['.bss']
    text, bss = sections[ti], sections[bi]
    if text[3] or bss[3] != text[5]:
        raise ValueError('module must have contiguous text/BSS starting at zero')
    if any(s[2] & 2 and i not in (ti, bi) and s[5] for i, s in enumerate(sections)):
        raise ValueError('unexpected allocated ELF section')
    payload = contents(text)
    total = text[5] + bss[5]
    symbols, symbol_tables = {}, {}
    for i, s in enumerate(sections):
        if s[1] != 2: continue
        if s[9] != 16: raise ValueError('invalid symbol table')
        table = contents(sections[s[6]])
        entries = list(struct.iter_unpack('<IIIBBH', contents(s)))
        symbol_tables[i] = entries
        for name, value, size, info, other, index in entries[1:]:
            if index == 0: raise ValueError('undefined symbol: ' + string(table, name))
            if name: symbols[string(table, name)] = value
    relocs = []
    for s in sections:
        if s[1] != 9: continue
        if not sections[s[7]][2] & 2: continue  # Non-allocated debug metadata.
        if s[7] != ti: raise ValueError('relocations outside text')
        if s[9] != 8: raise ValueError('invalid relocation table')
        for off, info in struct.iter_unpack('<II', contents(s)):
            kind, sym = info & 255, info >> 8
            symbol = symbol_tables[s[6]][sym]
            if kind == 2:  # R_ARM_ABS32
                if symbol[5] not in (ti, bi) or off % 4 or off > len(payload) - 4:
                    raise ValueError('non-local or unaligned ABS32 relocation')
                if struct.unpack_from('<I', payload, off)[0] > total:
                    raise ValueError('relocated address outside module')
                relocs.append(off)
            elif kind in (1, 3, 28, 29):  # PC24, REL32, CALL, JUMP24
                if symbol[5] not in (ti, bi):
                    raise ValueError('relative reference outside module')
            elif kind not in (0, 40):  # NONE, V4BX (linker already fixed instruction)
                raise ValueError('unsupported ARM relocation %d' % kind)
    relocs.sort()
    if not relocs or len(relocs) != len(set(relocs)):
        raise ValueError('missing or duplicate relocations; link with --emit-relocs')
    header = HEADER.pack(MAGIC, abi, 1, len(payload), bss[5], symbols['n5g_module_start'],
                         symbols['n5g_nand_usb_command'], symbols['n5g_nand_heartbeat'], len(relocs), 0)
    out = header + payload + struct.pack('<%dI' % len(relocs), *relocs)
    parse_module(out)
    return out


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('elf', type=pathlib.Path)
    p.add_argument('output', type=pathlib.Path)
    p.add_argument('--abi', type=int, choices=(1, 2), default=ABI,
                   help='module ABI; 2 requires a kernel with peripheral leases')
    a = p.parse_args()
    blob = pack_elf(a.elf.read_bytes(), a.abi)
    a.output.parent.mkdir(parents=True, exist_ok=True)
    a.output.write_bytes(blob)
    h = parse_module(blob)
    print('%d-byte module, %d bytes code/data + %d BSS, %d fixups -> %s' %
          (len(blob), h[3], h[4], h[8], a.output))
