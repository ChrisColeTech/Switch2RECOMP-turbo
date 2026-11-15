#!/usr/bin/env python3
# SwitchRecomp - Copyright 2026 MZ Labs
# Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
"""Exercise the deployed Qt launcher's real build path with a synthetic, game-data-free NUP."""
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_nup import minimal_nso, write_nup

ROOT = Path(__file__).resolve().parents[1]


class LauncherWorkflow(unittest.TestCase):
    def test_build_saves_ready_library_entry(self):
        binary = ROOT / 'bin/launcher/SwitchRecompLauncher.exe'
        self.assertTrue(binary.is_file(), 'Build the Qt launcher first: scripts/build-launcher.bat')
        with tempfile.TemporaryDirectory(prefix='launcher-', dir=ROOT / 'build') as tmp:
            work = Path(tmp)
            source = work / 'Test game [v0] & input.nup'
            output = work / 'Build output [test]'
            library = work / 'library.json'
            write_nup(source, {'Data/raw.bin': b'raw fixture'}, {'main': minimal_nso()})
            result = subprocess.run([str(binary), '--root', str(ROOT), '--library', str(library),
                                     '--smoke-build', str(source), '--build-output', str(output)],
                                    capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            records = json.loads(library.read_text(encoding='utf-8'))['games']
            self.assertEqual(len(records), 1)
            game = records[0]
            self.assertFalse(game['buildIncomplete'])
            self.assertEqual(game['builtSource'], game['source'])
            base = output / game['outputName']
            self.assertTrue(Path(str(base) + '.exe').is_file())
            self.assertTrue(Path(str(base) + '_code.dll').is_file())
            self.assertIn('renderer=vulkan', Path(str(base) + '.ini').read_text())
            self.assertEqual(Path(str(base) + '_data/romfs.nup').read_text().strip(), str(source))
            self.assertFalse(Path(str(base) + '_data/romfs').exists())
            self.assertTrue((output / 'tools/nxshader/nxs_live.py').is_file())


if __name__ == '__main__': unittest.main()
