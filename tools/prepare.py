#!/usr/bin/env python3
"""Link the verified Gearhead Garage game and video drivers into one guest image and lift it."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys

import pefile

ROOT = Path(__file__).resolve().parents[1]
# (file inside the game folder, load address or None for the preferred base, SHA-256, export prefix)
MODULES = [
    ('ghg.exe', None, 'f9d797d580d5a42f2f46b912cd6c107bad34c27a32d050cbee13379a65cc498c'),
    ('Data/soft8bit.vd', 0x01000000, '3462e455687546681f510b2d2b84ff93f5475aee0b7503f7f27b4701499517c8'),
    ('Data/d3d.vd', 0x01100000, 'c9394cd571654de11ccadcf823683021b7bd49895487350178a0dc560452491f'),
]
EXTRA_ARGS = {
    'SetConsoleTitleA': 1, 'GetConsoleScreenBufferInfo': 2, 'FormatMessageA': 7, 'ToAscii': 5,
    'ClipCursor': 1, 'FillRect': 3, 'MoveWindow': 6, 'mmioOpenA': 3, 'mmioClose': 2,
    'mmioAscend': 3, 'mmioRead': 3, 'mmioDescend': 4, 'mmioSeek': 3, 'mmioSetInfo': 3,
    'mmioAdvance': 3, 'mmioGetInfo': 3, 'SetErrorMode': 1, 'DirectSoundCreate': 3,
    'FindWindowA': 2, 'CallNextHookEx': 4, 'SetWindowsHookExA': 4, 'UnhookWindowsHookEx': 1,
    'RegisterClassA': 1, 'SystemParametersInfoA': 4, 'GetKeyState': 1,
}


def section_extent(section):
    return section.Misc_VirtualSize or section.SizeOfRawData


def align(value, boundary):
    return (value + boundary - 1) & -boundary


def checked(path, expected):
    data = Path(path).read_bytes()
    if hashlib.sha256(data).hexdigest() != expected:
        raise ValueError(f'unsupported {Path(path).name}: checksum differs from the accepted release')
    return data


def link_modules(folder):
    """Combine the privately supplied modules; output stays in ignored build storage."""
    loaded = []
    for name, address, sha in MODULES:
        data = checked(Path(folder) / name, sha)
        pe = pefile.PE(data=data)
        if pe.FILE_HEADER.Machine != 0x14c:
            raise ValueError(f'{name} is not an x86 module')
        if address is not None:
            if not hasattr(pe, 'DIRECTORY_ENTRY_BASERELOC'):
                raise ValueError(f'{name} cannot be relocated')
            pe.relocate_image(address)
        loaded.append((name, pe, data))
    game = loaded[0][1]
    base = game.OPTIONAL_HEADER.ImageBase
    imports, relocs, sections, exports = [], [], [], []
    seeds = {base + game.OPTIONAL_HEADER.AddressOfEntryPoint}
    modules = []
    groups = []
    for name, pe, _ in loaded:
        mbase = pe.OPTIONAL_HEADER.ImageBase
        delta = mbase - base
        modules.append((Path(name).name, mbase, pe.OPTIONAL_HEADER.SizeOfImage, mbase + pe.OPTIONAL_HEADER.AddressOfEntryPoint))
        for module in pe.DIRECTORY_ENTRY_IMPORT:
            symbols = []
            for symbol in module.imports:
                label = symbol.name.decode() if symbol.name else f'#{symbol.ordinal}'
                imports.append((symbol.address, module.dll.decode(), label))
                symbols.append((symbol.address - base, symbol.name.decode() if symbol.name else symbol.ordinal))
            groups.append((module.dll.decode(), symbols))
        if hasattr(pe, 'DIRECTORY_ENTRY_BASERELOC'):
            relocs.extend(mbase + e.rva for b in pe.DIRECTORY_ENTRY_BASERELOC for e in b.entries if e.type == 3)
        for e in getattr(getattr(pe, 'DIRECTORY_ENTRY_EXPORT', None), 'symbols', []):
            if e.forwarder:
                raise ValueError('forwarded exports are unsupported')
            address = mbase + e.address
            seeds.add(address)
            exports.append((mbase, e.name.decode() if e.name else f'#{e.ordinal}', address))
        if delta:
            seeds.add(mbase + pe.OPTIONAL_HEADER.AddressOfEntryPoint)
        for s in pe.sections:
            extent = section_extent(s)
            if not extent:
                continue
            data = s.get_data() if s.PointerToRawData else b''
            data = data[:min(len(data), extent)]
            sections.append((s.Name, s.VirtualAddress + delta, extent, data, s.Characteristics))

    # Keep imports at their original guest IAT slots; concatenate descriptors only.
    imports_rva = align(max(rva + extent for _, rva, extent, _, _ in sections), 4096)
    payload = bytearray((len(groups) + 1) * 20)
    for index, (dll, symbols) in enumerate(groups):
        name_rva = imports_rva + len(payload)
        payload.extend(dll.encode() + b'\0')
        names = []
        for iat, label in symbols:
            if isinstance(label, int):
                names.append(0x80000000 | label)
            else:
                payload.extend(bytes(-len(payload) % 2))
                names.append(imports_rva + len(payload))
                payload.extend(b'\0\0' + label.encode() + b'\0')
        payload.extend(bytes(-len(payload) % 4))
        int_rva = imports_rva + len(payload)
        payload.extend(struct.pack('<' + 'I' * (len(names) + 1), *names, 0))
        struct.pack_into('<IIIII', payload, index * 20, int_rva, 0, 0, name_rva, symbols[0][0])
    sections.append((b'.hostimp', imports_rva, len(payload), bytes(payload), 0xc0000040))
    exe_data = loaded[0][2]
    headers = bytearray(exe_data[:4096])
    nt = struct.unpack_from('<I', headers, 0x3c)[0]
    optional = nt + 24
    table = optional + game.FILE_HEADER.SizeOfOptionalHeader
    if table + len(sections) * 40 > 4096:
        raise ValueError('too many linked sections')
    struct.pack_into('<H', headers, nt + 6, len(sections))
    struct.pack_into('<I', headers, optional + 56, align(imports_rva + len(payload), 4096))
    struct.pack_into('<I', headers, optional + 60, 4096)
    struct.pack_into('<II', headers, optional + 96, 0, 0)
    struct.pack_into('<II', headers, optional + 96 + 8, imports_rva, (len(groups) + 1) * 20)
    struct.pack_into('<II', headers, optional + 96 + 5 * 8, 0, 0)
    output = headers
    for index, (name, rva, extent, data, flags) in enumerate(sections):
        offset = len(output) if data else 0
        raw = align(len(data), 512)
        output.extend(data + bytes(raw - len(data)))
        struct.pack_into('<8sIIIIIIHHI', output, table + 40 * index,
                         name, extent, rva, raw, offset, 0, 0, 0, 0, flags)
    return bytes(output), imports, relocs, seeds, exports, modules


def prepare(folder):
    folder = Path(folder)
    linked, imports, relocs, seeds, exports, modules = link_modules(folder)
    out, gen = ROOT / 'out', ROOT / 'gen/all'
    out.mkdir(exist_ok=True)
    gen.mkdir(parents=True, exist_ok=True)
    image = out / 'linked.exe'
    image.write_bytes(linked)
    pe = pefile.PE(data=linked)
    base = pe.OPTIONAL_HEADER.ImageBase
    ranges = [(base + s.VirtualAddress, base + s.VirtualAddress + section_extent(s))
              for s in pe.sections if s.Characteristics & 0x20000000]
    (out / 'relocs.txt').write_text(''.join(f'{a:08x}\n' for a in sorted(relocs)))
    (out / 'seeds.txt').write_text(''.join(f'{a:08x}\n' for a in sorted(seeds)))
    counts = json.loads((ROOT / 'tools/win32_args.json').read_text())
    counts.update(EXTRA_ARGS)
    missing = sorted({n for _, _, n in imports if n not in counts})
    if missing:
        raise ValueError('unknown import calling conventions: ' + ', '.join(missing))
    header = '''#ifndef GEARHEAD_IMAGE_H
#define GEARHEAD_IMAGE_H
#include <stdint.h>
typedef struct { uint32_t address, offset, size; } ImageSection;
typedef struct { uint32_t iat; const char *dll, *name; int nargs; } Import;
typedef struct { uint32_t module; const char *name; uint32_t address; } GuestExport;
typedef struct { const char *name; uint32_t base, size, entry; } GuestModule;
extern const ImageSection image_sections[];
extern const uint32_t image_section_count;
extern const Import image_imports[];
extern const uint32_t image_import_count;
extern const GuestExport image_exports[];
extern const uint32_t image_export_count;
extern const GuestModule image_modules[];
extern const uint32_t image_module_count;
'''
    values = {'IMAGE_BASE': base, 'IMAGE_SIZE': pe.OPTIONAL_HEADER.SizeOfImage,
              'IMAGE_ENTRY': base + pe.OPTIONAL_HEADER.AddressOfEntryPoint,
              'IMAGE_FILE_SIZE': len(linked), 'DISPATCH_TEXT_BASE': min(lo for lo, hi in ranges),
              'DISPATCH_TEXT_SIZE': max(hi for lo, hi in ranges) - min(lo for lo, hi in ranges)}
    header += ''.join(f'#define {name} 0x{value:x}u\n' for name, value in values.items())
    header += '#define IMAGE_SHA256 "' + hashlib.sha256(linked).hexdigest() + '"\n#endif\n'
    (gen / 'image.h').write_text(header)
    source = '#include "image.h"\n'
    source += f'const uint32_t image_section_count = {len(pe.sections)};\nconst ImageSection image_sections[] = {{\n'
    for s in pe.sections:
        size = min(s.SizeOfRawData, section_extent(s)) if s.PointerToRawData else 0
        source += f'{{0x{base+s.VirtualAddress:x}u,{s.PointerToRawData}u,{size}u}},\n'
    source += '};\n'
    source += f'const uint32_t image_import_count = {len(imports)};\nconst Import image_imports[] = {{\n'
    for address, dll, name in imports:
        source += f'{{0x{address:x}u,{json.dumps(dll)},{json.dumps(name)},{counts[name]}}},\n'
    source += '};\n'
    source += f'const uint32_t image_export_count = {len(exports)};\nconst GuestExport image_exports[] = {{\n'
    source += ''.join(f'{{0x{m:x}u,{json.dumps(n)},0x{a:x}u}},\n' for m, n, a in exports) + '};\n'
    source += f'const uint32_t image_module_count = {len(modules)};\nconst GuestModule image_modules[] = {{\n'
    source += ''.join(f'{{{json.dumps(n)},0x{b:x}u,0x{s:x}u,0x{e:x}u}},\n' for n, b, s, e in modules) + '};\n'
    (gen / 'image.c').write_text(source)
    (out / 'image.json').write_text(json.dumps({'sha256': hashlib.sha256(linked).hexdigest(),
                                              'imports': len(imports), 'exports': exports,
                                              'modules': modules, 'ranges': ranges}, indent=2) + '\n')
    return image


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('game_folder', type=Path)
    parser.add_argument('--metadata-only', action='store_true')
    args = parser.parse_args()
    os.chdir(ROOT)
    image = prepare(args.game_folder.resolve())
    if not args.metadata_only:
        subprocess.run([sys.executable, 'tools/genall.py', str(image), '64'], check=True,
                       env=dict(os.environ, SEEDS='out/seeds.txt', X87PC='1', WATCH='1'))


if __name__ == '__main__':
    main()
