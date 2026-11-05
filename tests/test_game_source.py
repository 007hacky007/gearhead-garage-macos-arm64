import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import _paths
from game_source import MANIFEST, SourceRejected, prepare_source
from isoimage import write_image

CLI = _paths.ROOT / 'tools/builder_cli.py'


class GameSourceTests(unittest.TestCase):
    """Only copies that must be refused: no game file is available to the tests."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.dir = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def refused(self, source, text):
        with self.assertRaises(SourceRejected) as caught:
            prepare_source(source, self.dir / 'Game')
        self.assertIn(text, str(caught.exception))
        self.assertFalse((self.dir / 'Game').exists() and any((self.dir / 'Game').rglob('*.exe')))

    def test_manifest_lists_the_release(self):
        paths = {e['path'].lower() for e in MANIFEST['files']}
        self.assertEqual(len(MANIFEST['files']), 122)
        self.assertTrue({'ghg.exe', 'data/d3d.vd', 'data/soft8bit.vd'} <= paths)
        self.assertEqual(len(MANIFEST['msi_sha256']), 64)

    def test_installed_folder_with_other_files(self):
        game = self.dir / 'Installed'
        game.mkdir()
        (game / 'GHG.EXE').write_bytes(b'MZ not the game')
        self.refused(game, 'missing')

    def test_wrong_msi(self):
        (self.dir / 'GEAR.MSI').write_bytes(b'not the installer')
        self.refused(self.dir / 'GEAR.MSI', 'differs')

    def test_disc_image_with_other_msi(self):
        write_image(self.dir / 'disc.iso', {'GEARHEAD/GEAR.MSI': b'other installer'})
        self.refused(self.dir / 'disc.iso', 'differs')

    def test_disc_image_without_msi(self):
        write_image(self.dir / 'disc.iso', {'OTHER/FILE.TXT': b'x'})
        self.refused(self.dir / 'disc.iso', 'not on the disc')

    def test_app_without_game(self):
        (self.dir / 'Gearhead Garage.app/Contents').mkdir(parents=True)
        self.refused(self.dir / 'Gearhead Garage.app', 'no game inside')

    def test_folder_without_game(self):
        self.refused(self.dir, 'neither ghg.exe')

    def test_unsupported_file(self):
        (self.dir / 'game.zip').write_bytes(b'PK')
        self.refused(self.dir / 'game.zip', 'unsupported input')

    def test_cli_reports_refusal_as_event(self):
        (self.dir / 'GEAR.MSI').write_bytes(b'not the installer')
        result = subprocess.run([sys.executable, '-B', str(CLI), 'check', '--source', str(self.dir / 'GEAR.MSI')],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 1)
        events = [json.loads(line[len('GEARHEAD-EVENT '):]) for line in result.stdout.splitlines()
                  if line.startswith('GEARHEAD-EVENT ')]
        self.assertEqual(events[0], {'step': 'prepare', 'title': 'Reading and checking your game copy'})
        self.assertIn('not accepted', events[-1]['error'])


if __name__ == '__main__':
    unittest.main()
