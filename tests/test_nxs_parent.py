# SwitchRecomp - Copyright 2026 MZ Labs
# Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
# Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen.
"""A shader helper's liveness check must never terminate its parent on Windows."""
import subprocess
import sys
import unittest
import tempfile
import struct
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools' / 'nxshader'))
from nxs_live import ParentProcess, is_current
from nxs_cache import Cache
from nxs_front import job_for, job_digest


class ParentTest(unittest.TestCase):
    def test_live_compute_launch_geometry(self):
        ctl = bytearray(0xe0)
        struct.pack_into('<2I', ctl, 0, 0x98761234, 0xc9)
        struct.pack_into('<I', ctl, 0x38, 16)
        struct.pack_into('<2I', ctl, 0x40, 16, 16)
        ctl[0x56] = 5
        struct.pack_into('<4I', ctl, 0x58, 8, 8, 1, 4096)
        with tempfile.TemporaryDirectory() as work:
            rec = bytes(0x38) + bytes(16) + ctl
            job, _ = job_for('test', 'cs_live.bin', rec, work, {})
            self.assertEqual(job['local'], [8, 8, 1])
            self.assertEqual(job['shared'], 4096)
            self.assertTrue(job['no_facts'])
            wrong = dict(job, local=[1, 1, 1], shared=0)
            self.assertNotEqual(job_digest(job), job_digest(wrong))

    def test_stale_and_partial_publications_are_rebuilt(self):
        with tempfile.TemporaryDirectory() as root:
            cache = Cache(root)
            keys = [f'{i:016x}' for i in range(5)]
            spv = bytes.fromhex('03022307') + bytes(16)
            cache.publish(keys[0], spv, {'cache': {'chain': 'current'}})
            cache.publish(keys[1], spv, {'cache': {'chain': 'old'}})
            cache.publish(keys[2], b'', {'cache': {'chain': 'current'}})
            cache.publish(keys[3], spv, {'cache': {'chain': 'current'}})
            Path(root, 'vk', keys[3]+'.spv').unlink()
            Path(root, 'vk', keys[4]+'.json').write_text('{')
            self.assertEqual({k for k in keys if is_current(cache, "current", k)}, {keys[0]})
            self.assertTrue(Path(root, 'vk', keys[1]+'.spv').exists())

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
