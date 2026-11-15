# SwitchRecomp - Copyright 2026 MZ Labs
# Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
# Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen.
"""A shader helper's liveness check must never terminate its parent on Windows."""
import subprocess
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools' / 'nxshader'))
from nxs_live import ParentProcess


class ParentTest(unittest.TestCase):
    def test_probe_preserves_child_and_detects_exit(self):
        with subprocess.Popen([sys.executable, '-c', 'import sys; sys.stdin.read()'],
                              stdin=subprocess.PIPE) as child:
            parent = ParentProcess(child.pid)
            try:
                for _ in range(10):
                    self.assertTrue(parent.alive())
                    self.assertIsNone(child.poll())
                child.communicate(timeout=5)
                self.assertFalse(parent.alive())
            finally:
                parent.close()


if __name__ == '__main__': unittest.main()
