"""Embed this project's Metal shader source, never game data, in a private build header."""
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def build_shaders():
    target = ROOT / 'gen/all'
    target.mkdir(parents=True, exist_ok=True)
    metal = 'static const char metal_shader[] = ' + json.dumps((ROOT / 'runtime/scene.metal').read_text()) + ';\n'
    header = target / 'shader.h'
    if not header.exists() or header.read_text() != metal:
        header.write_text(metal)


if __name__ == '__main__':
    build_shaders()
