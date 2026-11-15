#!/usr/bin/env python3
# SwitchRecomp - Copyright 2026 MZ Labs
# Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
"""Synthetic, game-data-free tests for standard application NSP in-place reads."""
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
import nsp
from test_nup import minimal_nso


def pfs(files):
    names = bytearray(); rows = bytearray(); data = bytearray()
    for name, payload in files.items():
        name_at = len(names); names += name.encode() + b'\0'
        rows += struct.pack('<QQII', len(data), len(payload), name_at, 0); data += payload
    return struct.pack('<4sIII', b'PFS0', len(files), len(names), 0) + rows + names + data


def romfs():
    filename = b'file.bin'
    dirs = struct.pack('<6I', 0, 0xffffffff, 0xffffffff, 0, 0xffffffff, 0)
    files = struct.pack('<IIQQII', 0, 0xffffffff, 0, 4, 0xffffffff, len(filename)) + filename
    data_at = 0x50 + len(dirs) + len(files)
    header = struct.pack('<10Q', 0x50, 0, 0, 0x50, len(dirs), 0, 0,
                         0x50 + len(dirs), len(files), data_at)
    return header + dirs + files + b'test'


def fixture(path, header_path, bktr=False):
    exefs = pfs({'main': minimal_nso(), 'main.npdm': b'npdm'})
    image = romfs(); exefs_at = 0xc00; romfs_at = (exefs_at + len(exefs) + 0x1ff) & ~0x1ff
    nca_size = (romfs_at + 0x200 + len(image) + 0x1ff) & ~0x1ff
    nca = bytearray(nca_size)
    nca[exefs_at:exefs_at + len(exefs)] = exefs
    nca[romfs_at + 0x200:romfs_at + 0x200 + len(image)] = image
    header = bytearray(0xc00); header[0x200:0x204] = b'NCA3'; header[0x205] = 0
    struct.pack_into('<Q', header, 0x210, 0x400000000000001)
    struct.pack_into('<II', header, 0x240, exefs_at // 0x200, romfs_at // 0x200)
    struct.pack_into('<II', header, 0x250, romfs_at // 0x200, len(nca) // 0x200)
    fs = memoryview(header)[0x400:0x600]; fs[0:5] = bytes((2, 0, 1, 2, 1)); struct.pack_into('<QQ', fs, 0x40, 0, len(exefs))
    fs = memoryview(header)[0x600:0x800]; fs[0:5] = bytes((2, 0, 0, 3, 4 if bktr else 1)); fs[8:12] = b'IVFC'
    struct.pack_into('<QQI', fs, 0x18 + 5 * 0x18, 0x200, len(image), 12)
    header_path.write_bytes(header)
    path.write_bytes(pfs({'program.nca': bytes(nca)}))


class NspTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.root = Path(self.tmp.name)
        self.keys = self.root / 'prod.keys'; self.keys.write_text('header_key = ' + '00' * 32)
        self.header = self.root / 'header.bin'; self.package = self.root / 'game.nsp'
        self.tool = self.root / 'fake_hactool.py'
        self.tool.write_text("import shutil,sys\nout=[x[9:] for x in sys.argv if x.startswith('--header=')][0]\nshutil.copyfile(%r,out)\n" % str(self.header))

    def tearDown(self): self.tmp.cleanup()

    def test_standard_application_reads_exefs_and_romfs(self):
        fixture(self.package, self.header)
        package = nsp.load(self.package, self.keys, self.tool)
        entries = {name: reader() for name, _, reader in package.exefs()}
        self.assertEqual(entries['main'][:4], b'NSO0')
        self.assertEqual(entries['main.npdm'], b'npdm')
        wrapper = nsp.NspRomfs(package); files = list(wrapper.walk_files())
        self.assertEqual([name for name, _ in files], ['file.bin'])
        self.assertEqual(wrapper.read(files[0][1]), b'test')
        self.assertIn('SWRNSP1\n', package.marker())

    def test_bktr_is_rejected(self):
        fixture(self.package, self.header, True)
        with self.assertRaisesRegex(nsp.Error, 'BKTR'):
            nsp.Nsp(self.package, self.keys, self.tool)

    def test_invalid_pfs_is_rejected(self):
        self.package.write_bytes(b'not an nsp')
        with self.assertRaises(nsp.Error): nsp.Nsp(self.package, self.keys, self.tool)


if __name__ == '__main__': unittest.main()
