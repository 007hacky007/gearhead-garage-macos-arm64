#!/usr/bin/env python3
"""Install the files of a Windows Installer package (.msi) into a folder, without running it.

Reads the OLE compound file, the MSI string pool and the File/Component/Directory
tables, and extracts the embedded MSZIP cabinet with zlib. Nothing from the package
is executed. Usage: python3 tools/msi.py PACKAGE.msi NEW_DIRECTORY
"""
import struct
import sys
import zlib
from pathlib import Path, PurePosixPath


class MsiError(ValueError):
    pass


# ---- OLE compound file ----------------------------------------------------
class CompoundFile:
    FREE, END = 0xFFFFFFFF, 0xFFFFFFFE

    def __init__(self, data):
        if data[:8] != b'\xd0\xcf\x11\xe0\xa1\xb1\x1a\xe1':
            raise MsiError('not an OLE compound file')
        self.data = data
        shift = struct.unpack_from('<H', data, 0x1e)[0]
        mini_shift = struct.unpack_from('<H', data, 0x20)[0]
        self.size, self.mini_size = 1 << shift, 1 << mini_shift
        if self.size not in (512, 4096) or self.mini_size != 64:
            raise MsiError('unsupported compound file sector size')
        (fat_count, dir_start, _, self.mini_cutoff, mini_fat_start, mini_fat_count,
         difat_start, difat_count) = struct.unpack_from('<IIIIIIII', data, 0x2c)
        difat = list(struct.unpack_from('<109I', data, 0x4c))
        sector = difat_start
        for _ in range(difat_count):
            if sector >= self.END - 1:
                break
            entries = struct.unpack_from(f'<{self.size // 4}I', self.sector(sector))
            difat.extend(entries[:-1])
            sector = entries[-1]
        self.fat = []
        for s in difat[:fat_count]:
            self.fat.extend(struct.unpack_from(f'<{self.size // 4}I', self.sector(s)))
        directory = self.chain(dir_start)
        self.entries = []
        for off in range(0, len(directory), 128):
            e = directory[off:off + 128]
            name_len = struct.unpack_from('<H', e, 64)[0]
            name = e[:max(0, name_len - 2)].decode('utf-16-le', 'surrogatepass')
            kind = e[66]
            start, size = struct.unpack_from('<IQ', e, 116)
            if self.size == 512:
                size &= 0xFFFFFFFF
            self.entries.append((name, kind, start, size))
        root = self.entries[0]
        self.mini_stream = self.chain(root[2])[:root[3]] if root[1] == 5 else b''
        self.mini_fat = []
        if mini_fat_count:
            raw = self.chain(mini_fat_start)
            self.mini_fat = list(struct.unpack_from(f'<{len(raw) // 4}I', raw))

    def sector(self, n):
        off = (n + 1) * self.size
        if off + self.size > len(self.data):
            raise MsiError('compound file sector outside the file')
        return self.data[off:off + self.size]

    def chain(self, start, mini=False):
        out, seen = bytearray(), set()
        table, size = (self.mini_fat, self.mini_size) if mini else (self.fat, self.size)
        n = start
        while n < 0xFFFFFFFA:
            if n in seen or n >= len(table):
                raise MsiError('corrupt compound file sector chain')
            seen.add(n)
            out += self.mini_stream[n * size:(n + 1) * size] if mini else self.sector(n)
            n = table[n]
        return bytes(out)

    def streams(self):
        for name, kind, start, size in self.entries:
            if kind == 2:
                mini = size < self.mini_cutoff
                yield name, self.chain(start, mini)[:size]


# ---- MSI stream names and tables ---------------------------------------------
_CHARS = '0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz._'


def decode_name(name):
    out = []
    for ch in name:
        v = ord(ch)
        if 0x3800 <= v < 0x4800:
            v -= 0x3800
            out.append(_CHARS[v & 0x3f])
            out.append(_CHARS[v >> 6])
        elif 0x4800 <= v < 0x4840:
            out.append(_CHARS[v - 0x4800])
        elif v == 0x4840:
            out.append('!')
        else:
            out.append(ch)
    return ''.join(out)


class Package:
    def __init__(self, path):
        cf = CompoundFile(Path(path).read_bytes())
        self.streams = {decode_name(n): d for n, d in cf.streams()}
        pool, data = self.streams.get('!_StringPool'), self.streams.get('!_StringData')
        if pool is None or data is None:
            raise MsiError('not a Windows Installer package')
        self.long_refs = struct.unpack_from('<I', pool, 0)[0] & 0x80000000
        self.strings = ['']
        off, i = 0, 4
        while i < len(pool):
            size, refs = struct.unpack_from('<HH', pool, i)
            i += 4
            if size == 0 and refs:
                size = struct.unpack_from('<I', pool, i)[0]
                i += 4
            self.strings.append(data[off:off + size].decode('cp1252'))
            off += size
        self.width = 3 if self.long_refs else 2
        self.columns = {}
        raw = self.streams['!_Columns']
        widths = [self.width, 2, self.width, 2]
        for row in self._rows(raw, widths):
            table, number, name, kind = row
            self.columns.setdefault(self.strings[table], []).append((number ^ 0x8000, self.strings[name], kind ^ 0x8000))

    @staticmethod
    def _rows(raw, widths):
        count = len(raw) // sum(widths)
        cols, off = [], 0
        for w in widths:
            cols.append([int.from_bytes(raw[off + w * r:off + w * r + w], 'little') for r in range(count)])
            off += w * count
        return list(zip(*cols))

    def table(self, name):
        spec = sorted(self.columns.get(name, []))
        if not spec:
            raise MsiError(f'package has no {name} table')
        widths = [self.width if kind & 0x800 else (4 if (kind & 0xff) == 4 else 2) for _, _, kind in spec]
        rows = []
        for row in self._rows(self.streams.get('!' + name, b''), widths):
            values = {}
            for (number, column, kind), w, v in zip(spec, widths, row):
                if kind & 0x800:
                    values[column] = self.strings[v] if v < len(self.strings) else ''
                else:
                    values[column] = (v ^ (0x8000 if w == 2 else 0x80000000)) if v else None
            rows.append(values)
        return rows


# ---- cabinet (MSZIP) ---------------------------------------------------------------
def cabinet_files(cab):
    if cab[:4] != b'MSCF':
        raise MsiError('embedded cabinet is not a Microsoft cabinet')
    _, _, size, _, files_off, _, _, _, folders, files, flags = struct.unpack_from('<4sIIIIIBBHHH', cab, 0)
    off = 36
    folder_reserve = data_reserve = 0
    if flags & 4:
        header_reserve, folder_reserve, data_reserve = struct.unpack_from('<HBB', cab, off)
        off += 4 + header_reserve
    if flags & 3:
        raise MsiError('multi-part cabinets are not supported')
    folder_data = []
    for _ in range(folders):
        start, blocks, compress = struct.unpack_from('<IHH', cab, off)
        off += 8 + folder_reserve
        if compress & 0xf not in (0, 1):
            raise MsiError('only stored and MSZIP cabinets are supported')
        out, window, pos = bytearray(), b'', start
        for _ in range(blocks):
            _, packed, unpacked = struct.unpack_from('<IHH', cab, pos)
            pos += 8 + data_reserve
            block = cab[pos:pos + packed]
            pos += packed
            if compress & 0xf == 0:
                chunk = block
            else:
                if block[:2] != b'CK':
                    raise MsiError('corrupt MSZIP block')
                d = zlib.decompressobj(-15, zdict=window) if window else zlib.decompressobj(-15)
                chunk = d.decompress(block[2:]) + d.flush()
            if len(chunk) != unpacked:
                raise MsiError('MSZIP block has the wrong size')
            out += chunk
            window = bytes(out[-32768:])
        folder_data.append(bytes(out))
    off = files_off
    result = {}
    for _ in range(files):
        length, start, folder = struct.unpack_from('<IIH', cab, off)
        end = cab.index(b'\0', off + 16)
        name = cab[off + 16:end].decode('cp1252')
        off = end + 1
        if folder >= len(folder_data) or start + length > len(folder_data[folder]):
            raise MsiError('cabinet file outside its folder')
        result[name] = folder_data[folder][start:start + length]
    return result


def long_name(value):
    return value.split('|')[-1]


def install(package_path, target):
    """Lay the package's files out as the installer would under the product folder."""
    pkg = Package(package_path)
    dirs = {r['Directory']: (r['Directory_Parent'], long_name(r['DefaultDir'])) for r in pkg.table('Directory')}
    components = {r['Component']: r['Directory_'] for r in pkg.table('Component')}
    files = pkg.table('File')
    cab = next((d for d in pkg.streams.values() if d[:4] == b'MSCF'), None)
    if cab is None:
        raise MsiError('package has no embedded cabinet')
    contents = cabinet_files(cab)
    keys = {r['File'] for r in files}
    # The product folder: the directory that holds the most files.
    counts = {}
    for r in files:
        counts[components[r['Component_']]] = counts.get(components[r['Component_']], 0) + 1

    def chain(d):
        parts = []
        while d in dirs and dirs[d][0]:
            parent, name = dirs[d]
            if not name.startswith('.'):
                parts.append(name)
            d = parent
        return parts[::-1]

    def relative(directory, root):
        full, base = chain(directory), chain(root)
        if full[:len(base)] != base:
            raise MsiError(f'{directory} is outside the product folder')
        return full[len(base):]

    product = [d for d in components.values() if any(r['FileName'].lower().endswith('ghg.exe') for r in files
                                                       if components[r['Component_']] == d)]
    if not product:
        raise MsiError('package does not install ghg.exe')
    root = product[0]
    target = Path(target)
    target.mkdir(parents=True)
    written = []
    for r in files:
        key = r['File']
        if key not in contents:
            raise MsiError(f'cabinet is missing {key}')
        parts = relative(components[r['Component_']], root) + [long_name(r['FileName'])]
        rel = PurePosixPath(*parts)
        if any(p in ('', '.', '..') or '/' in p or '\\' in p for p in parts):
            raise MsiError(f'unsafe installed path {rel}')
        dest = target / rel
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(contents[key])
        written.append(str(rel))
    for r in pkg.table('DuplicateFile') if 'DuplicateFile' in pkg.columns else []:
        if r['File_'] in keys and r.get('DestFolder'):
            src = next(w for w in files if w['File'] == r['File_'])
            parts = relative(r['DestFolder'], root) + [long_name(r['DestName'] or src['FileName'])]
            dest = target / PurePosixPath(*parts)
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(contents[r['File_']])
            written.append(str(PurePosixPath(*parts)))
    for r in pkg.table('CreateFolder') if 'CreateFolder' in pkg.columns else []:
        try:
            (target / PurePosixPath(*relative(r['Directory_'], root))).mkdir(parents=True, exist_ok=True)
        except MsiError:
            pass
    return written


if __name__ == '__main__':
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    for name in install(sys.argv[1], sys.argv[2]):
        print(name)
