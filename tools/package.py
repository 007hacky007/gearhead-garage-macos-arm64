"""Internal app packaging step. The public entry point is ./build.sh."""
import plistlib
import shutil
import struct
import subprocess
import tempfile
from pathlib import Path

import pefile

ROOT = Path(__file__).resolve().parents[1]
BUNDLE_ID = 'org.classicgames.gearheadgarage'
VERSION = (ROOT / 'VERSION').read_text().strip()


def icon_file(exe, output):
    """The largest icon group of the game's Windows launcher, written as a .ico file."""
    pe = pefile.PE(str(exe))
    groups, images = [], {}
    if not hasattr(pe, 'DIRECTORY_ENTRY_RESOURCE'):
        raise ValueError('Gearhead.exe has no resources')
    for entry in pe.DIRECTORY_ENTRY_RESOURCE.entries:
        for sub in entry.directory.entries:
            for lang in sub.directory.entries:
                data = pe.get_data(lang.data.struct.OffsetToData, lang.data.struct.Size)
                if entry.id == 14:
                    groups.append(data)
                elif entry.id == 3:
                    images[sub.id] = data
    if not groups:
        raise ValueError('Gearhead.exe has no icon')
    group = groups[0]
    count = struct.unpack_from('<H', group, 4)[0]
    entries = [struct.unpack_from('<BBBBHHIH', group, 6 + 14 * i) for i in range(count)]
    header = struct.pack('<HHH', 0, 1, len(entries))
    offset = 6 + 16 * len(entries)
    directory, blobs = b'', b''
    for w, h, colors, reserved, planes, bits, size, ident in entries:
        blob = images[ident]
        directory += struct.pack('<BBBBHHII', w, h, colors, reserved, planes, bits, len(blob), offset + len(blobs))
        blobs += blob
    Path(output).write_bytes(header + directory + blobs)


def icon_pixels(ico):
    """RGBA rows of the largest, deepest image in a .ico (BMP images with palette and mask)."""
    data = Path(ico).read_bytes()
    count = struct.unpack_from('<H', data, 4)[0]
    best = max((struct.unpack_from('<BBBBHHII', data, 6 + 16 * i) for i in range(count)),
               key=lambda e: ((e[0] or 256), e[5]))
    blob = data[best[7]:best[7] + best[6]]
    if blob[:8] == b'\x89PNG\r\n\x1a\n':
        raise ValueError('PNG icon images are not expected in Gearhead.exe')
    size, w, h2, _, bits = struct.unpack_from('<IiiHH', blob, 0)
    h = h2 // 2
    colors = struct.unpack_from('<I', blob, 32)[0] or (1 << bits if bits <= 8 else 0)
    palette = [blob[size + 4 * i:size + 4 * i + 4] for i in range(colors)]
    pixels = size + 4 * colors
    stride = ((w * bits + 31) // 32) * 4
    mask_stride = ((w + 31) // 32) * 4
    mask = pixels + stride * h
    rows = []
    for y in range(h):
        src = h - 1 - y
        row = []
        for x in range(w):
            if bits <= 8:
                byte = blob[pixels + src * stride + x * bits // 8]
                index = (byte >> (8 - bits - (x * bits) % 8)) & ((1 << bits) - 1)
                b, g, r = palette[index][:3]
            else:
                off = pixels + src * stride + x * bits // 8
                b, g, r = blob[off:off + 3]
            transparent = (blob[mask + src * mask_stride + x // 8] >> (7 - x % 8)) & 1
            row.append((r, g, b, 0 if transparent else 255))
        rows.append(row)
    return rows


def write_png(rows, scale, path):
    """Nearest-neighbour enlargement keeps the original pixel art sharp."""
    import zlib
    h, w = len(rows) * scale, len(rows[0]) * scale
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        for x in range(w):
            raw.extend(rows[y // scale][x // scale])
    def chunk(t, b):
        return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b))
    Path(path).write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0))
                           + chunk(b'IDAT', zlib.compress(bytes(raw), 9)) + chunk(b'IEND', b''))


def make_icns(exe, resources):
    with tempfile.TemporaryDirectory() as temporary:
        ico = Path(temporary) / 'gearhead.ico'
        icon_file(exe, ico)
        rows = icon_pixels(ico)
        iconset = Path(temporary) / 'Gearhead.iconset'
        iconset.mkdir()
        base = len(rows)
        for size in (16, 32, 128, 256, 512):
            for scale in (1, 2):
                pixels = size * scale
                name = f'icon_{size}x{size}' + ('@2x' if scale == 2 else '') + '.png'
                if pixels >= base:
                    write_png(rows, pixels // base, iconset / name)
                else:
                    small = [row[::base // pixels] for row in rows[::base // pixels]]
                    write_png(small, 1, iconset / name)
        subprocess.run(['/usr/bin/iconutil', '-c', 'icns', str(iconset), '-o', str(resources / 'Gearhead.icns')], check=True)


def package_app(game_folder, output=None, builder_version=None):
    game_folder = Path(game_folder).resolve()
    output = Path(output or ROOT / 'out/Gearhead Garage.app').resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.gearhead-app-', dir=output.parent) as temporary:
        app = Path(temporary) / output.name
        contents = app / 'Contents'
        binaries, resources = contents / 'MacOS', contents / 'Resources'
        binaries.mkdir(parents=True)
        resources.mkdir(parents=True)
        make_icns(game_folder / 'Gearhead.exe', resources)
        subprocess.run(['clang', '-arch', 'arm64', '-mmacosx-version-min=11.0', '-O2', '-fobjc-arc',
                        '-I' + str(ROOT / 'gen/all'), str(ROOT / 'launcher/main.m'),
                        '-framework', 'Cocoa', '-o', str(binaries / 'GearheadLauncher')], check=True)
        shutil.copy2(ROOT / 'out/gearhead', binaries / 'GearheadEngine')
        shutil.copy2(ROOT / 'out/linked.exe', resources / 'linked.exe')
        # The verified game: data, help pages and the original Windows files (never run).
        shutil.copytree(game_folder, resources / 'Game')
        for path in (resources / 'Game').rglob('*'):
            if path.is_file():
                path.chmod(0o644)
        shutil.copytree(ROOT / 'third_party', resources / 'Notices')
        info = {'CFBundleExecutable': 'GearheadLauncher', 'CFBundleIdentifier': BUNDLE_ID,
                'CFBundleName': 'Gearhead Garage', 'CFBundleDisplayName': 'Gearhead Garage',
                'CFBundlePackageType': 'APPL', 'CFBundleIconFile': 'Gearhead.icns',
                'CFBundleShortVersionString': VERSION, 'CFBundleVersion': '1',
                'LSMinimumSystemVersion': '11.0', 'NSHighResolutionCapable': True,
                'LSApplicationCategoryType': 'public.app-category.simulation-games',
                'LSMultipleInstancesProhibited': True}
        if builder_version:
            info['GearheadBuilderVersion'] = builder_version
        (contents / 'Info.plist').write_bytes(plistlib.dumps(info))
        for binary in (binaries / 'GearheadEngine', binaries / 'GearheadLauncher'):
            subprocess.run(['codesign', '--force', '--sign', '-', '--timestamp=none', str(binary)], check=True)
        subprocess.run(['codesign', '--force', '--sign', '-', '--timestamp=none', str(app)], check=True)
        subprocess.run(['codesign', '--verify', '--deep', '--strict', str(app)], check=True)
        if output.exists():
            marker = output / 'Contents/Info.plist'
            if not marker.is_file() or plistlib.loads(marker.read_bytes()).get('CFBundleIdentifier') != BUNDLE_ID:
                raise ValueError('Refusing to replace an unrelated app at the output path')
            shutil.rmtree(output)
        app.rename(output)
    print(f'Built {output}')
    return output


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument('game_folder')
    parser.add_argument('output', nargs='?')
    parser.add_argument('--builder-version')
    args = parser.parse_args()
    package_app(args.game_folder, args.output, args.builder_version)
