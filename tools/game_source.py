#!/usr/bin/env python3
"""Turn the user's own Gearhead Garage copy into a verified game folder.

Accepted inputs: a CD image (.cue with its .bin, .bin, .img or .iso), the GEAR.MSI
package, a folder with the disc contents, an installed Gearhead Garage folder, or a
Gearhead Garage.app made by this build (the game inside it is reused).
Nothing from the input is executed; inputs are only read.
"""
import hashlib
import json
import shutil
import tempfile
from pathlib import Path

try:
    from .disc import Disc, DiscError
    from .msi import MsiError, install
except ImportError:
    from disc import Disc, DiscError
    from msi import MsiError, install

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = json.loads((ROOT / 'tools/game_manifest.json').read_text())
# Rewritten by the game itself (palette cache); any version is fine.
VOLATILE = {'soft8bit.plt'}
IMAGE_SUFFIXES = {'.cue', '.bin', '.img', '.iso'}


class SourceRejected(ValueError):
    pass


def find_insensitive(base, relative):
    """The path of relative (a/b/c) under base, matching each component without case."""
    current = Path(base)
    for part in Path(relative).parts:
        if not current.is_dir():
            return None
        match = next((p for p in current.iterdir() if p.name.lower() == part.lower()), None)
        if match is None:
            return None
        current = match
    return current


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def verify(folder):
    """Every accepted file is present with its exact contents; returns a summary line."""
    problems = []
    for entry in MANIFEST['files']:
        path = find_insensitive(folder, entry['path'])
        if path is None or not path.is_file():
            problems.append(f'missing {entry["path"]}')
        elif entry['path'] not in VOLATILE and (path.stat().st_size != entry['size'] or sha256(path) != entry['sha256']):
            problems.append(f'different {entry["path"]}')
    if problems:
        shown = ', '.join(problems[:6]) + (f' and {len(problems) - 6} more' if len(problems) > 6 else '')
        raise SourceRejected('this is not the supported Gearhead Garage release (Build 131): ' + shown)
    total = sum(e['size'] for e in MANIFEST['files'])
    return f'{len(MANIFEST["files"])} files, {total:,} bytes, matching {MANIFEST["release"]}'


def copy_installed(source, dest):
    dest.mkdir(parents=True)
    for entry in MANIFEST['files']:
        path = find_insensitive(source, entry['path'])
        if path is None:
            raise SourceRejected(f'the installed folder is missing {entry["path"]}')
        target = dest / entry['path']
        target.parent.mkdir(parents=True, exist_ok=True)
        if entry['path'] in VOLATILE and sha256(path) != entry['sha256']:
            raise SourceRejected('soft8bit.plt was changed by the game; use the CD or GEAR.MSI instead')
        shutil.copyfile(path, target)
    for empty in ('Data/SnapShots', 'Data/Mechanics'):
        (dest / empty).mkdir(parents=True, exist_ok=True)


def install_package(msi, dest):
    if sha256(msi) != MANIFEST['msi_sha256']:
        raise SourceRejected('GEAR.MSI differs from the supported Gearhead Garage release')
    try:
        install(msi, dest)
    except MsiError as error:
        raise SourceRejected(f'cannot read GEAR.MSI: {error}') from error


def prepare_source(source, dest):
    """Lay out and verify the game from source into the new folder dest."""
    source, dest = Path(source).expanduser(), Path(dest)
    if dest.exists():
        raise ValueError(f'{dest} already exists')
    if not source.exists():
        raise SourceRejected(f'{source} does not exist')
    if source.suffix.lower() == '.app' and source.is_dir():
        inner = source / 'Contents/Resources/Game'
        if not inner.is_dir():
            raise SourceRejected(f'{source.name} has no game inside it')
        source = inner
    if source.is_dir():
        if find_insensitive(source, 'ghg.exe'):
            copy_installed(source, dest)
        else:
            msi = find_insensitive(source, 'GEARHEAD/GEAR.MSI') or find_insensitive(source, 'GEAR.MSI')
            if msi is None:
                raise SourceRejected('the folder has neither ghg.exe (installed game) nor GEARHEAD/GEAR.MSI (disc)')
            install_package(msi, dest)
    elif source.suffix.lower() == '.msi':
        install_package(source, dest)
    elif source.suffix.lower() in IMAGE_SUFFIXES:
        with tempfile.TemporaryDirectory() as temporary:
            msi = Path(temporary) / 'GEAR.MSI'
            try:
                disc = Disc(source)
                try:
                    disc.extract('GEARHEAD/GEAR.MSI', msi)
                finally:
                    disc.close()
            except DiscError as error:
                raise SourceRejected(f'cannot read the disc image: {error}') from error
            install_package(msi, dest)
    else:
        raise SourceRejected('unsupported input; use a .cue/.bin/.iso image, GEAR.MSI or a folder')
    return verify(dest)


if __name__ == '__main__':
    import sys
    if len(sys.argv) != 3:
        raise SystemExit('usage: game_source.py SOURCE NEW_GAME_FOLDER')
    print(prepare_source(sys.argv[1], sys.argv[2]))
