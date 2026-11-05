"""Tiny ISO9660 images for the tests: never a real game file."""
import struct


def _record(name, extent, size, directory):
    raw = name if isinstance(name, bytes) else name.encode()
    length = 33 + len(raw) + (1 - len(raw) % 2)
    rec = bytearray(length)
    rec[0] = length
    rec[2:10] = struct.pack('<I', extent) + struct.pack('>I', extent)
    rec[10:18] = struct.pack('<I', size) + struct.pack('>I', size)
    rec[25] = 2 if directory else 0
    rec[28:32] = struct.pack('<H', 1) + struct.pack('>H', 1)
    rec[32] = len(raw)
    rec[33:33 + len(raw)] = raw
    return bytes(rec)


def iso_sectors(files):
    """2048-byte sectors of an image with `files`: {'DIR/NAME.EXT': bytes}, one folder deep."""
    folders = {}
    for path, data in files.items():
        folder, name = path.split('/')
        folders.setdefault(folder, {})[name] = data
    sectors = {}
    root_extent, next_sector = 18, 19 + len(folders)
    folder_extents = {f: 19 + i for i, f in enumerate(folders)}
    root = _record(b'\0', root_extent, 2048, True) + _record(b'\1', root_extent, 2048, True)
    for folder, extent in folder_extents.items():
        root += _record(folder, extent, 2048, True)
        listing = _record(b'\0', extent, 2048, True) + _record(b'\1', root_extent, 2048, True)
        for name, data in folders[folder].items():
            listing += _record(name + ';1', next_sector, len(data), False)
            for i in range(0, max(1, len(data)), 2048):
                sectors[next_sector] = data[i:i + 2048].ljust(2048, b'\0')
                next_sector += 1
        sectors[extent] = listing.ljust(2048, b'\0')
    sectors[root_extent] = root.ljust(2048, b'\0')
    pvd = bytearray(2048)
    pvd[0], pvd[1:6], pvd[6] = 1, b'CD001', 1
    pvd[156:156 + 34] = _record(b'\0', root_extent, 2048, True)
    sectors[16] = bytes(pvd)
    sectors[17] = bytes([255]) + b'CD001' + bytes([1]) + bytes(2041)
    count = max(sectors) + 1
    return [sectors.get(i, bytes(2048)) for i in range(count)]


def write_image(path, files, mode=None):
    """mode None: 2048-byte .iso; 1 or 2: raw 2352-byte MODE1 or MODE2 form 1 sectors."""
    sync = b'\0' + b'\xff' * 10 + b'\0'
    with open(path, 'wb') as out:
        for sector in iso_sectors(files):
            if mode is None:
                out.write(sector)
            elif mode == 1:
                out.write(sync + bytes(3) + b'\1' + sector + bytes(288))
            else:
                out.write(sync + bytes(3) + b'\2' + bytes(8) + sector + bytes(280))
