#!/usr/bin/env python3
"""Build and assemble Gearhead Garage Builder.app (ad hoc signed).

  tools/make_builder.py --version 1.0.0 --output dist   -> dist/Gearhead Garage Builder.app

The app bundles exactly what a build needs from this checkout (Contents/Resources/
BuildSources): build.sh, VERSION, requirements.txt, tools/, runtime/, launcher/ and
third_party/. Never gen/, out/, local/ or any game file.
"""
import argparse
import hashlib
import plistlib
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ENTRIES = ('build.sh', 'VERSION', 'requirements.txt', 'tools', 'runtime', 'launcher', 'third_party')
# Development helpers a build never runs.
EXCLUDE = ('tools/dev/', 'tools/disasm.py', 'tools/make_builder.py', 'tools/release_notes.py')
GAME_SUFFIXES = {'.exe', '.dll', '.vd', '.msi', '.cab', '.bin', '.cue', '.iso', '.img', '.dat', '.plt'}
MIN_MACOS = '11.0'
SWIFT_TARGET = f'arm64-apple-macos{MIN_MACOS}'
APP_NAME = 'Gearhead Garage Builder'


def build_sources(root):
    """The files the Builder bundles, relative to the repository root: what git tracks (or
    would track: ignored files never count), or the same entries walked outside a checkout."""
    try:
        out = subprocess.run(['git', 'ls-files', '-z', '--cached', '--others', '--exclude-standard', '--', *ENTRIES],
                             cwd=root, check=True, capture_output=True).stdout.decode()
        names = [n for n in out.split('\0') if n]
    except (OSError, subprocess.CalledProcessError):
        names = []
        for entry in ENTRIES:
            path = root / entry
            if path.is_file():
                names.append(entry)
            else:
                names += [str(p.relative_to(root)) for p in path.rglob('*') if p.is_file()]
    files = sorted(Path(n) for n in set(names)
                   if '__pycache__' not in n and not n.endswith(('.pyc', '.DS_Store')) and not n.startswith(EXCLUDE)
                   and (root / n).is_file())
    game = [str(f) for f in files if f.suffix.lower() in GAME_SUFFIXES]
    if game:
        raise SystemExit(f'refusing to bundle what looks like game files: {", ".join(game)}')
    return files


def parse_minos(otool_output):
    block = otool_output.split('LC_BUILD_VERSION', 1)[1]
    return re.search(r'minos (\S+)', block).group(1)


def build_binary(root):
    """swiftc directly: SwiftPM raises the deployment target, and the Builder must open on
    macOS 11 like the game (`swift test` still uses the package)."""
    package = root / 'builder'
    out = package / '.build/swiftc-release'
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True)
    common = ['swiftc', '-swift-version', '5', '-O', '-target', SWIFT_TARGET, '-parse-as-library']
    core = sorted(str(p) for p in (package / 'Sources/BuilderCore').glob('*.swift'))
    app = sorted(str(p) for p in (package / 'Sources/GearheadBuilder').glob('*.swift'))
    subprocess.run(common + ['-module-name', 'BuilderCore', '-emit-module', '-emit-module-path',
                             str(out / 'BuilderCore.swiftmodule'), '-emit-library', '-static',
                             '-o', str(out / 'libBuilderCore.a'), *core], check=True)
    binary = out / 'GearheadBuilder'
    subprocess.run(common + ['-module-name', 'GearheadBuilder', '-I', str(out), '-L', str(out), '-lBuilderCore',
                             '-o', str(binary), *app], check=True)
    minos = parse_minos(subprocess.run(['otool', '-l', str(binary)], check=True, capture_output=True,
                                       text=True).stdout)
    if minos != MIN_MACOS:
        raise SystemExit(f'{binary} targets macOS {minos}, expected {MIN_MACOS}')
    return binary


def make_icns(png, destination):
    with tempfile.TemporaryDirectory() as tmp:
        iconset = Path(tmp) / 'AppIcon.iconset'
        iconset.mkdir()
        for size in (16, 32, 128, 256, 512):
            for scale in (1, 2):
                name = f'icon_{size}x{size}{"@2x" if scale == 2 else ""}.png'
                subprocess.run(['sips', '-z', str(size * scale), str(size * scale), str(png), '--out',
                                str(iconset / name)], check=True, capture_output=True)
        subprocess.run(['iconutil', '-c', 'icns', str(iconset), '-o', str(destination)], check=True)


def assemble(binary, version, output, root):
    app = Path(output) / f'{APP_NAME}.app'
    if app.exists():
        shutil.rmtree(app)
    macos, resources = app / 'Contents/MacOS', app / 'Contents/Resources'
    macos.mkdir(parents=True)
    resources.mkdir(parents=True)
    shutil.copy2(binary, macos / 'GearheadBuilder')
    sources = build_sources(root)
    digest = hashlib.sha256()
    for rel in sources:
        target = resources / 'BuildSources' / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(root / rel, target)
        digest.update(str(rel).encode() + b'\0' + (root / rel).read_bytes() + b'\0')
    # Identifies these sources: the Builder copies them to its workspace again when it changes.
    (resources / 'BuildSources/.sources-id').write_text(digest.hexdigest()[:16] + '\n')
    make_icns(root / 'builder/Resources/AppIcon-1024.png', resources / 'AppIcon.icns')
    with open(app / 'Contents/Info.plist', 'wb') as f:
        plistlib.dump({
            'CFBundleIdentifier': 'org.classicgames.gearheadgarage.builder', 'CFBundleName': APP_NAME,
            'CFBundleDisplayName': APP_NAME, 'CFBundleExecutable': 'GearheadBuilder',
            'CFBundlePackageType': 'APPL', 'CFBundleShortVersionString': version, 'CFBundleVersion': version,
            'LSMinimumSystemVersion': MIN_MACOS, 'NSHighResolutionCapable': True, 'CFBundleIconFile': 'AppIcon',
            'LSArchitecturePriority': ['arm64'],
            # The game copy may be in these places; macOS asks once and shows this text.
            **{f'NS{place}UsageDescription': 'To read your game copy. Gearhead Garage Builder never changes it.'
               for place in ('DesktopFolder', 'DocumentsFolder', 'DownloadsFolder', 'RemovableVolumes',
                             'NetworkVolumes')},
            'NSHumanReadableCopyright':
                'Builds Gearhead Garage.app from your own game copy. No game files are included.',
        }, f)
    subprocess.run(['codesign', '--force', '--deep', '--sign', '-', str(app)], check=True)
    subprocess.run(['codesign', '--verify', '--deep', '--strict', str(app)], check=True)
    print(f'{len(sources)} source files bundled')
    return app


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--version', required=True)
    parser.add_argument('--output', default='dist')
    args = parser.parse_args()
    if not re.fullmatch(r'[0-9A-Za-z.+-]+', args.version):
        raise SystemExit('--version must be a version number')
    print(assemble(build_binary(ROOT), args.version, Path(args.output), ROOT))


if __name__ == '__main__':
    main()
