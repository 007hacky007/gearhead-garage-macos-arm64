#!/usr/bin/env python3
"""Non-interactive entry points for Gearhead Garage Builder (builder/).

  python3 tools/builder_cli.py setup --venv DIR
      DIR: a venv of the Python that runs this (the Builder's private one) with
      requirements.txt installed. Done again only when requirements.txt changes.
  VENV/bin/python tools/builder_cli.py build --source PATH --output APP [--version V]
      the same steps as ./build.sh: reads and verifies the game copy at PATH (a CD image,
      GEAR.MSI, a disc folder, an installed game folder or a Gearhead Garage.app), then
      translates, compiles and packages APP. PATH is only read.
  VENV/bin/python tools/builder_cli.py check --source PATH
      only reads and verifies the game copy.

Event lines are `GEARHEAD-EVENT {json}`: {"step": ID, "title": TEXT} before each step
(prepare, lift, compile, package), {"progress": 0..1} within a step, {"done": APP} at the
end and {"error": TEXT} on failure (exit 1). Every other line is plain log text.
"""
import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build_app  # noqa: E402
from game_source import SourceRejected  # noqa: E402

PREFIX = 'GEARHEAD-EVENT '
STEP_FAILED = 'This step stopped with an error. Click Copy log and include it in a bug report.'


def emit(event):
    print(PREFIX + json.dumps(event), flush=True)


class JsonEvents(build_app.Events):
    def step(self, ident, title):
        print(f'\n{title}', flush=True)
        emit({'step': ident, 'title': title})

    def progress(self, part):
        emit({'progress': part})


def setup(venv):
    if sys.platform != 'darwin' or os.uname().machine != 'arm64':
        raise ValueError('building needs an Apple Silicon Mac')
    if sys.version_info < (3, 11):
        raise ValueError('Python 3.11 or newer is needed')
    print(f'Setting up the build tools in {venv}', flush=True)
    build_app.install_requirements(venv)
    print('Build tools ready', flush=True)


def build(args):
    if args.version and not all(c.isalnum() or c in '.+-' for c in args.version):
        raise ValueError('the Builder version is not a version number')
    output = Path(args.output).expanduser().absolute()
    if output.suffix != '.app':
        raise ValueError('the output must be an .app')
    build_app.check_host()
    result = build_app.build_app(Path(args.source).expanduser().absolute(), output, sys.executable,
                                 JsonEvents(), builder_version=args.version, prepare_only=args.check_only)
    if result:
        emit({'done': str(result)})


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest='command', required=True)
    setup_parser = commands.add_parser('setup')
    setup_parser.add_argument('--venv', required=True)
    for name in ('build', 'check'):
        sub = commands.add_parser(name)
        sub.add_argument('--source', required=True)
        sub.add_argument('--output', default=str(build_app.ROOT / 'out/Gearhead Garage.app'))
        sub.add_argument('--version')
        sub.set_defaults(check_only=name == 'check')
    args = parser.parse_args()
    try:
        if args.command == 'setup':
            setup(args.venv)
        else:
            build(args)
    except SourceRejected as error:
        emit({'error': f'Your game copy was not accepted: {error}.'})
        return 1
    except subprocess.CalledProcessError as error:
        print(f'command failed with exit status {error.returncode}: {error.cmd}', flush=True)
        emit({'error': STEP_FAILED})
        return 1
    except (ValueError, OSError) as error:
        emit({'error': str(error)})
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
