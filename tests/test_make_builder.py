import unittest

import _paths
import make_builder


class BundleTests(unittest.TestCase):
    def test_bundle_has_the_build_and_no_game(self):
        files = {str(p) for p in make_builder.build_sources(_paths.ROOT)}
        for needed in ('build.sh', 'VERSION', 'requirements.txt', 'tools/builder_cli.py', 'tools/prepare.py',
                       'tools/game_manifest.json', 'runtime/d3d.c', 'launcher/main.m'):
            self.assertIn(needed, files)
        self.assertFalse(any(f.startswith(('gen/', 'out/', 'local/', 'tools/dev/')) for f in files))
        self.assertFalse(any(p.split('.')[-1].lower() in ('exe', 'vd', 'msi', 'bin', 'cue', 'iso') for p in files))


if __name__ == '__main__':
    unittest.main()
