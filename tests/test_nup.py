#!/usr/bin/env python3
# SwitchRecomp - Copyright 2026 MZ Labs
# Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
# Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen.
"""Direct-NUP parser and recompiler staging tests; creates no copyrighted game data."""
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
import nup
import nxloader
import switchrecomp


def minimal_nso(build_id=b'test-build'):
    data = bytearray(0x200)
    data[:4] = b'NSO0'
    struct.pack_into('<III', data, 0x10, 0x100, 0, 0x100)
    struct.pack_into('<III', data, 0x20, 0x200, 0x100, 0)
    struct.pack_into('<III', data, 0x30, 0x200, 0x100, 0)
    data[0x40:0x40 + len(build_id)] = build_id
    struct.pack_into('<III', data, 0x60, 0x100, 0, 0)
    return bytes(data)


def minimal_nro(build_id=b'deferred'):
    data = bytearray(0x100)
    data[0x10:0x14] = b'NRO0'
    struct.pack_into('<I', data, 0x18, len(data))
    struct.pack_into('<II', data, 0x20, 0, len(data))
    struct.pack_into('<II', data, 0x28, len(data), 0)
    struct.pack_into('<II', data, 0x30, len(data), 0)
    data[0x40:0x40 + len(build_id)] = build_id
    return bytes(data)


def write_nup(path, romfs, exefs, compressed=(), trailing_auxiliary=False):
    def compress(data):
        try:
            from backports import zstd
            return zstd.compress(data)
        except ImportError:
            try:
                from compression import zstd
                return zstd.compress(data)
            except ImportError:
                try:
                    import zstandard
                    return zstandard.compress(data)
                except ImportError:
                    return subprocess.run(['zstd', '-q', '--stdout'], input=data, check=True,
                                          stdout=subprocess.PIPE).stdout

    tree = {'': set()}
    for name in romfs:
        parts = name.split('/')
        for index in range(1, len(parts)):
            directory = '/'.join(parts[:index])
            parent = '/'.join(parts[:index - 1])
            tree.setdefault(directory, set())
            tree[parent].add(parts[index - 1])
        tree['/'.join(parts[:-1])].add(parts[-1])
    directories = sorted(tree)
    exefs_directory = len(directories)
    directory_count = len(directories) + 1
    romfs_names, exefs_names = sorted(romfs), sorted(exefs)
    names = romfs_names + exefs_names
    indexes = {name: directory_count + index for index, name in enumerate(names)}

    def children(directory):
        if directory == exefs_directory:
            return [indexes[name] for name in exefs_names]
        result = []
        for child in sorted(tree[directories[directory]]):
            full = (directories[directory] + '/' + child).lstrip('/')
            result.append(directories.index(full) if full in directories else indexes[full])
        return result

    pool = bytearray()

    def pooled(name):
        encoded = name.encode('utf-8')
        offset = len(pool)
        pool.extend(encoded)
        return offset, len(encoded)

    directory_names = [pooled('' if index == exefs_directory else directories[index].rpartition('/')[2])
                       for index in range(directory_count)]
    file_names = {name: pooled(name.rpartition('/')[2]) for name in names}
    title_count = 2 if trailing_auxiliary else 1
    files_at = 0x30 + title_count * 0x40 + directory_count * 24
    children_at = files_at + len(names) * 80
    pool_at = children_at + 4 * sum(len(children(index)) for index in range(directory_count))
    data_at = 0x40 + pool_at + len(pool)
    payload = bytearray()
    rows = {}
    for name in names:
        source = romfs[name] if name in romfs else exefs[name]
        codec = int(name in compressed)
        stored = compress(source) if codec else source
        rows[name] = len(source), data_at + len(payload), len(stored), codec
        payload.extend(stored)

    directory_rows = bytearray()
    child_rows = bytearray()
    for index in range(directory_count):
        items = children(index)
        no, nl = directory_names[index]
        directory_rows.extend(struct.pack('<IIII8x', no, nl, len(child_rows) // 4, len(items)))
        for item in items:
            child_rows.extend(struct.pack('<I', item))
    file_rows = bytearray()
    for name in names:
        no, nl = file_names[name]
        size, offset, stored, codec = rows[name]
        file_rows.extend(struct.pack('<IIQQQ32sB15x', no, nl, size, offset, stored, bytes(32), codec))

    header = struct.pack('<4s4xI', b'NUP\x01', 0x40) + bytes(0x34)
    toc = struct.pack('<4sIQQQQ', b'TOC\x01', title_count, directory_count, len(names), pool_at, len(pool)) + bytes(8)
    title = struct.pack('<QIIII', 0x0400C3F00006E000, 0, exefs_directory, 0, 1) + bytes(0x28)
    if trailing_auxiliary:
        title += struct.pack('<QIIII', 0x0400C3F00006E001, 0, 0xFFFFFFFF, 0, 0) + bytes(0x28)
    Path(path).write_bytes(header + toc + title + directory_rows + file_rows + child_rows + pool + payload)


class DirectNupTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.main = minimal_nso()
        self.deferred = minimal_nro()
        self.packed = bytes(range(256)) * 1024
        self.package = self.root / 'game.nup'
        write_nup(self.package,
                  {'Data/raw.bin': b'raw data', 'Data/packed.bin': self.packed,
                   'Modules/deferred.nro': self.deferred},
                  {'main': self.main, 'main.npdm': b'not-meta'},
                  compressed={'Data/packed.bin'})

    def test_parser_reads_raw_compressed_and_exefs_entries(self):
        package = nup.load(self.package)
        self.assertEqual(package.title[0], 0x0400C3F00006E000)
        self.assertEqual(package.read(package.find('Data/raw.bin')), b'raw data')
        self.assertEqual(package.read(package.find('Data/packed.bin')), self.packed)
        self.assertEqual({name for name, _, _ in package.exefs()}, {'main', 'main.npdm'})

    def test_loader_and_staging_keep_romfs_in_package(self):
        modules, npdm, romfs = nxloader.load_program(str(self.package))
        self.assertEqual(npdm, b'not-meta')
        self.assertEqual([module.name for module in modules], ['main', 'romfs/Modules/deferred.nro'])
        output = self.root / 'out' / 'Game_data'
        switchrecomp.copy_data(modules, npdm, romfs, str(self.package), str(output), lambda _: None)
        self.assertEqual((output / 'exefs' / 'main').read_bytes(), self.main)
        self.assertTrue(Path((output / 'romfs.nup').read_text().strip()).samefile(self.package))
        self.assertFalse((output / 'romfs').exists())

    def test_corrupt_package_is_rejected(self):
        bad = self.root / 'bad.nup'
        bad.write_bytes(b'BAD!' + self.package.read_bytes()[4:])
        self.assertFalse(nup.is_nup(bad))
        with self.assertRaises(nup.Error):
            nup.Nup(bad)

    def test_declared_size_bounds_zstd_output(self):
        path = self.root / 'oversize.nup'
        write_nup(path, {'packed': b'A' * (1 << 20)}, {'main': self.main}, compressed={'packed'})
        data = bytearray(path.read_bytes())
        nd, = struct.unpack_from('<Q', data, 0x40 + 8)
        files_at = 0x40 + 0x30 + 0x40 + nd * 24
        struct.pack_into('<Q', data, files_at + 8, 16)
        path.write_bytes(data)
        package = nup.Nup(path)
        with self.assertRaisesRegex(nup.Error, 'declared size'):
            package.read(package.find('packed'))

    def test_unsafe_exefs_name_is_not_materialized(self):
        path = self.root / 'unsafe.nup'
        write_nup(path, {'data': b'x'}, {'main': self.main, '..\\outside': b'bad'})
        modules, npdm, romfs = nxloader.load_program(str(path))
        output = self.root / 'unsafe-output' / 'Game_data'
        with self.assertRaisesRegex(ValueError, 'unsafe ExeFS name'):
            switchrecomp.copy_data(modules, npdm, romfs, str(path), str(output), lambda _: None)
        self.assertFalse((output.parent / 'outside').exists())

    def test_trailing_auxiliary_title_is_not_selected(self):
        path = self.root / 'auxiliary.nup'
        write_nup(path, {'data': b'x'}, {'main': self.main}, trailing_auxiliary=True)
        package = nup.Nup(path)
        self.assertEqual(package.title_index, 0)
        self.assertEqual(package.title[0], 0x0400C3F00006E000)
        self.assertEqual(package.read(package.find('data')), b'x')


if __name__ == '__main__':
    unittest.main()
