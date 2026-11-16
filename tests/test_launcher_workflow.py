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
    def test_import_existing_build_without_compiling(self):
        binary = ROOT / 'bin/launcher/SwitchRecompLauncher.exe'
        self.assertTrue(binary.is_file(), 'Build the Qt launcher first')
        with tempfile.TemporaryDirectory(prefix='launcher import spaces ') as tmp:
            work = Path(tmp)
            exe = work / 'Mario Kart World 180.exe'; exe.write_bytes(b'fixture, never execute')
            exe.with_name(exe.stem + '_code.dll').write_bytes(b'fixture')
            data = exe.with_name(exe.stem + '_data'); data.mkdir()
            source = work / 'Source game 1.8.0.nsp'; source.write_bytes(b'PFS0')
            (data / 'romfs.nsp').write_text('SWRNSP1\n' + str(source) + '\n100\n200\n')
            exe.with_suffix('.ini').write_text('[Graphics]\nrenderer=vulkan\n')
            library = work / 'library.json'
            original = {'schema': 1, 'toolRoot': str(ROOT), 'jobs': 3, 'colorMode': 'light', 'games': [
                {'id': 'existing', 'title': 'Keep my game', 'source': str(work / 'Other.nup'), 'cover': 'custom.png'}]}
            library.write_text(json.dumps(original))
            command = [str(binary), '--root', str(ROOT), '--library', str(library), '--import-build', str(exe),
                       '--screenshot', str(work / 'preview.png')]
            for _ in range(2):
                result = subprocess.run(command, capture_output=True, text=True, timeout=30)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                saved = json.loads(library.read_text(encoding='utf-8'))
                self.assertEqual(len(saved['games']), 2)
                self.assertEqual(saved['jobs'], 3); self.assertEqual(saved['colorMode'], 'light')
                for key, value in original['games'][0].items(): self.assertEqual(saved['games'][0][key], value)
                imported = saved['games'][1]
                self.assertEqual(imported['title'], exe.stem)
                self.assertEqual(imported['renderer'], 'vulkan')
                self.assertEqual(imported['source'], str(source))
                self.assertEqual(imported['builtSource'], str(source))
                self.assertFalse(imported['buildIncomplete'])
                self.assertEqual(exe.read_bytes(), b'fixture, never execute')

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
