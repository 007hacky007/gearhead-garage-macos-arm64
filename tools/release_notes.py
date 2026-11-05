#!/usr/bin/env python3
"""Print the CHANGELOG.md section of a release tag (v1.2.3 or 1.2.3); fail without one."""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def notes(tag, text):
    version = tag[1:] if tag.startswith('v') else tag
    match = re.search(rf'^## {re.escape(version)}\s*\n(.*?)(?=^## |\Z)', text, re.M | re.S)
    if not match or not match.group(1).strip():
        raise SystemExit(f'CHANGELOG.md has no section for {version}')
    return match.group(1).strip() + '\n'


if __name__ == '__main__':
    if len(sys.argv) != 2:
        raise SystemExit('usage: release_notes.py TAG')
    print(notes(sys.argv[1], (ROOT / 'CHANGELOG.md').read_text()), end='')
