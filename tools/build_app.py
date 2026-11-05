#!/usr/bin/env python3
"""Build Gearhead Garage.app for Apple Silicon from your own Gearhead Garage copy.

GAME_SOURCE is a CD image (.cue with its .bin, .bin, .img, .iso), GEAR.MSI, the disc's
folder, an installed Gearhead Garage folder, or a Gearhead Garage.app built before.
"""
import argparse
import hashlib
import platform
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from game_source import SourceRejected, prepare_source  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
# The steps every build announces, in order (the Builder shows them with these ids).
STEPS = (
    ('prepare', 'Reading and checking your game copy'),
    ('lift', 'Translating the game and its video drivers to C'),
    ('compile', 'Compiling native ARM64 code'),
    ('package', 'Packaging and signing the app'),
)
SHARDS = 64


class Events:
    """Plain progress text for the Terminal build; tools/builder_cli.py adds JSON events."""

    def step(self, ident, title):
        print(title, flush=True)

    def progress(self, part):
        pass


def check_host():
    if sys.version_info < (3, 11):
        raise ValueError('Python 3.11 or newer is required')
    if platform.system() != 'Darwin' or platform.machine() != 'arm64':
        raise ValueError('the build needs macOS on Apple Silicon with a native ARM64 Python')
    if not shutil.which('xcrun') or subprocess.run(['xcrun', '--find', 'clang'], capture_output=True).returncode:
        raise ValueError('install the Xcode Command Line Tools first: xcode-select --install')


def requirements_digest():
    return hashlib.sha256((ROOT / 'requirements.txt').read_bytes()).hexdigest()


def install_requirements(environment, base_python=sys.executable):
    """The pinned packages in a private venv; global Python is never touched."""
    environment = Path(environment)
    python = environment / 'bin/python'
    marker = environment / '.build-requirements-sha256'
    stamp = requirements_digest()
    if python.is_file() and marker.is_file() and marker.read_text().strip() == stamp:
        return python
    if not python.is_file():
        subprocess.run([base_python, '-m', 'venv', str(environment)], check=True)
    subprocess.run([str(python), '-m', 'pip', 'install', '--disable-pip-version-check', '--no-input',
                    '--only-binary=:all:', '-r', str(ROOT / 'requirements.txt')], check=True)
    marker.write_text(stamp)
    return python


def bootstrap(cache):
    stamp = requirements_digest()
    environment = Path(cache) / f'python-{sys.version_info.major}.{sys.version_info.minor}-{stamp[:12]}'
    return install_requirements(environment)


def count_new(folder, pattern, since):
    try:
        return sum(1 for p in Path(folder).glob(pattern) if p.stat().st_mtime >= since)
    except OSError:
        return 0


def run(command, events, progress=None):
    """One build script from the repository root; `progress` () -> 0..1 is polled meanwhile."""
    process = subprocess.Popen(command, cwd=ROOT)
    stop = threading.Event()

    def poll():
        last = -1
        while not stop.wait(1):
            value = round(min(0.99, progress()), 2)
            if value != last:
                events.progress(value)
                last = value
    watcher = threading.Thread(target=poll, daemon=True) if progress else None
    if watcher:
        watcher.start()
    try:
        status = process.wait()
    finally:
        stop.set()
        if process.poll() is None:
            process.terminate()
            process.wait()
    if status:
        raise subprocess.CalledProcessError(status, command)


def build_app(source, output, python, events=None, builder_version=None, prepare_only=False):
    """Every build step: the game copy is read and verified, translated, compiled and packaged
    into `output`. `python` runs the steps and has requirements.txt installed."""
    events = events or Events()
    titles = dict(STEPS)
    work_parent = ROOT / 'local'
    work_parent.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='build-', dir=work_parent) as temporary:
        game = Path(temporary) / 'Game'
        events.step('prepare', titles['prepare'])
        print(f'Reading {source}', flush=True)
        print(prepare_source(source, game), flush=True)
        if prepare_only:
            return None
        events.step('lift', titles['lift'])
        started = time.time()
        run([str(python), '-B', str(ROOT / 'tools/prepare.py'), str(game)], events,
            lambda: count_new(ROOT / 'gen/all', 'shard_*.c', started) / SHARDS)
        events.step('compile', titles['compile'])
        started = time.time()
        total = len(list((ROOT / 'gen/all').glob('*.c'))) + len(list((ROOT / 'runtime').glob('*.[cm]')))
        run([str(python), '-B', str(ROOT / 'tools/compile.py')], events,
            lambda: count_new(ROOT / 'out/obj', '*.o', started) / max(1, total))
        events.step('package', titles['package'])
        command = [str(python), '-B', str(ROOT / 'tools/package.py'), str(game), str(Path(output).absolute())]
        if builder_version:
            command += ['--builder-version', builder_version]
        run(command, events)
    return Path(output)


def main():
    parser = argparse.ArgumentParser(prog='./build.sh', description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('game_source', type=Path, metavar='GAME_SOURCE')
    parser.add_argument('--cache', type=Path, default=Path.home() / 'Library/Caches/GearheadGarage')
    parser.add_argument('--output', type=Path, default=ROOT / 'out/Gearhead Garage.app')
    parser.add_argument('--prepare-only', action='store_true', help='only read and verify the game copy')
    args = parser.parse_args()
    if args.output.suffix != '.app':
        parser.error('--output must end in .app')
    try:
        check_host()
        cache = args.cache.expanduser().absolute()
        cache.mkdir(parents=True, exist_ok=True)
        print('Setting up the private Python packages', flush=True)
        python = bootstrap(cache)
        result = build_app(args.game_source, args.output, python, prepare_only=args.prepare_only)
    except (SourceRejected, ValueError, OSError, subprocess.CalledProcessError) as error:
        print(f'Build failed: {error}', file=sys.stderr)
        return 1
    if result:
        print(f'Done: {result}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
