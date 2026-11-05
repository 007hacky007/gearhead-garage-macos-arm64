#!/usr/bin/env python3
"""Read single files from an ISO9660 CD image: a 2048-byte ISO, or a raw 2352-byte
MODE1 or MODE2 (XA form 1) BIN/IMG, optionally through its CUE sheet.

Usage: python3 tools/disc.py IMAGE PATH_ON_DISC OUTPUT_FILE
The image is only read.
"""
import re
import struct
import sys
from pathlib import Path


class DiscError(ValueError):
    pass


def cue_image(path):
    try:
        text = Path(path).read_text(encoding='utf-8-sig', errors='strict')
    except UnicodeError as error:
        raise DiscError(f'invalid CUE sheet: {error}') from error
    files = re.findall(r'^\s*FILE\s+"([^"\x00]+)"\s+BINARY\s*$', text, re.I | re.M)
    if len(files) != 1:
        raise DiscError('the CUE sheet must name exactly one BIN file')
    tracks = re.findall(r'^\s*TRACK\s+(\d+)\s+(\S+)', text, re.I | re.M)
    if not tracks or tracks[0][1].upper() not in ('MODE1/2352', 'MODE2/2352', 'MODE1/2048'):
        raise DiscError('the first CUE track must be a data track')
    name = files[0]
    if name in ('.', '..') or any(c in name for c in '/\\:'):
        raise DiscError('the CUE BIN must be a file beside the CUE sheet')
    image = Path(path).parent / name
    if not image.is_file():
        raise DiscError(f'{name} (named by the CUE sheet) is missing beside it')
    return image


class Disc:
    SYNC = b'\x00' + b'\xff' * 10 + b'\x00'

    def __init__(self, path):
        path = Path(path)
        if path.suffix.lower() == '.cue':
            path = cue_image(path)
        self.file = path.open('rb')
        self.file.seek(16 * 2352)
        raw = self.file.read(2352)
        if raw[:12] == self.SYNC and raw[15] in (1, 2):
            self.stride = 2352
            self.offset = 16 if raw[15] == 1 else 24
        else:
            self.stride, self.offset = 2048, 0
        pvd = self.sector(16)
        if pvd[1:6] != b'CD001' or pvd[0] != 1:
            raise DiscError('no ISO9660 file system found in the image')
        self.root = (struct.unpack_from('<I', pvd, 158)[0], struct.unpack_from('<I', pvd, 166)[0])

    def close(self):
        self.file.close()

    def sector(self, n):
        self.file.seek(n * self.stride + self.offset)
        data = self.file.read(2048)
        if len(data) != 2048:
            raise DiscError('the image ends inside the file system')
        return data

    def read(self, extent, size):
        out = bytearray()
        n = extent
        while len(out) < size:
            out += self.sector(n)
            n += 1
        return bytes(out[:size])

    def listing(self, extent, size):
        data = self.read(extent, size)
        i = 0
        while i < len(data):
            length = data[i]
            if not length:
                i = (i // 2048 + 1) * 2048
                continue
            record = data[i:i + length]
            i += length
            name = record[33:33 + record[32]]
            if name in (b'\0', b'\1'):
                continue
            ext, sz = struct.unpack_from('<I', record, 2)[0], struct.unpack_from('<I', record, 10)[0]
            yield name.decode('latin-1').split(';')[0].rstrip('.'), bool(record[25] & 2), ext, sz

    def find(self, disc_path):
        extent, size = self.root
        parts = [p for p in re.split(r'[\\/]', disc_path) if p]
        for k, part in enumerate(parts):
            for name, is_dir, ext, sz in self.listing(extent, size):
                if name.lower() == part.lower() and is_dir == (k < len(parts) - 1):
                    extent, size = ext, sz
                    break
            else:
                return None
        return extent, size

    def extract(self, disc_path, output):
        found = self.find(disc_path)
        if not found:
            raise DiscError(f'{disc_path} is not on the disc')
        Path(output).write_bytes(self.read(*found))


if __name__ == '__main__':
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    d = Disc(sys.argv[1])
    d.extract(sys.argv[2], sys.argv[3])
