import tempfile
import unittest
from pathlib import Path

import _paths  # noqa: F401
from disc import Disc, DiscError, cue_image
from isoimage import write_image

PAYLOAD = bytes(range(256)) * 20 + b'end'


class DiscTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.dir = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def check(self, image):
        disc = Disc(image)
        try:
            self.assertIsNotNone(disc.find('gearhead/gear.msi'))
            self.assertIsNone(disc.find('GEARHEAD/OTHER.MSI'))
            disc.extract('GEARHEAD/GEAR.MSI', self.dir / 'out')
        finally:
            disc.close()
        self.assertEqual((self.dir / 'out').read_bytes(), PAYLOAD)

    def test_iso_and_raw_modes(self):
        for mode, name in ((None, 'a.iso'), (1, 'b.bin'), (2, 'c.bin')):
            with self.subTest(mode=mode):
                write_image(self.dir / name, {'GEARHEAD/GEAR.MSI': PAYLOAD}, mode)
                self.check(self.dir / name)

    def test_cue_sheet(self):
        write_image(self.dir / 'disc.bin', {'GEARHEAD/GEAR.MSI': PAYLOAD}, 2)
        (self.dir / 'disc.cue').write_text('FILE "disc.bin" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n')
        self.assertEqual(cue_image(self.dir / 'disc.cue'), self.dir / 'disc.bin')
        self.check(self.dir / 'disc.cue')

    def test_cue_rejects_paths_and_audio(self):
        (self.dir / 'a.cue').write_text('FILE "../x.bin" BINARY\n  TRACK 01 MODE1/2352\n')
        with self.assertRaises(DiscError):
            cue_image(self.dir / 'a.cue')
        (self.dir / 'b.cue').write_text('FILE "x.bin" BINARY\n  TRACK 01 AUDIO\n')
        with self.assertRaises(DiscError):
            cue_image(self.dir / 'b.cue')

    def test_not_a_cd(self):
        (self.dir / 'junk.iso').write_bytes(bytes(40000))
        with self.assertRaises(DiscError):
            Disc(self.dir / 'junk.iso')


if __name__ == '__main__':
    unittest.main()
