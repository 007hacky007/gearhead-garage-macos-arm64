#!/usr/bin/env python3
"""Internal ARM64 compiler step. Use ./build.sh to build the app."""
import concurrent.futures
import hashlib
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
os.chdir(ROOT)
sys.path.insert(0, str(ROOT / 'tools'))
OUT = ROOT / 'out/obj'
OUT.mkdir(parents=True, exist_ok=True)
from build_shaders import build_shaders
build_shaders()
FLAGS = ['-O2', '-arch', 'arm64', '-mcpu=apple-m1', '-mmacosx-version-min=11.0',
         '-ffp-contract=off', '-fno-strict-aliasing', '-Iruntime', '-Igen/all',
         '-include', 'gen/all/image.h', '-Wno-unused-label', '-Wno-parentheses-equality']
sources = sorted(Path('gen/all').glob('*.c')) + sorted(Path('runtime').glob('*.c'))
sources += sorted(Path('runtime').glob('*.m'))
headers = b''.join(p.read_bytes() for p in sorted(Path('runtime').glob('*.h')))
headers += Path('gen/all/decls.h').read_bytes() + Path('gen/all/image.h').read_bytes()
headers += Path('gen/all/shader.h').read_bytes()
game_headers = Path('runtime/cpu.h').read_bytes() + Path('gen/all/decls.h').read_bytes() + Path('gen/all/image.h').read_bytes()


def compile_one(source):
    obj = OUT / (str(source).replace('/', '_') + '.o')
    flags = FLAGS + (['-fobjc-arc'] if source.suffix == '.m' else [])
    dependencies = game_headers if source.parts[0] == 'gen' else headers
    digest = hashlib.sha256(source.read_bytes() + dependencies + repr(flags).encode()).hexdigest()
    stamp = obj.with_suffix('.sha256')
    if obj.exists() and stamp.exists() and stamp.read_text() == digest:
        return obj
    result = subprocess.run(['clang', *flags, '-c', str(source), '-o', str(obj)],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    obj.with_suffix('.log').write_text(result.stdout)
    if result.returncode:
        raise RuntimeError(f'{source}:\n' + result.stdout[-5000:])
    stamp.write_text(digest)
    return obj


with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 1) as pool:
    objects = list(pool.map(compile_one, sources))
subprocess.run(['clang', '-arch', 'arm64', '-mmacosx-version-min=11.0', '-Wl,-dead_strip',
                '-o', 'out/gearhead', *map(str, objects), '-lm', '-lz',
                '-framework', 'Cocoa', '-framework', 'Metal', '-framework', 'QuartzCore',
                '-framework', 'AudioToolbox', '-framework', 'AudioUnit', '-framework', 'CoreAudio'], check=True)
print('Built out/gearhead (ARM64)')
