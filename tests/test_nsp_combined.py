#!/usr/bin/env python3
# Copyright 2026 ChrisColeTech
# SPDX-License-Identifier: MIT
"""Synthetic, game-data-free tests for encrypted and combined (game + update + DLC) NSPs: nca.py, the version 2
romfs.nsp marker and the runtime that reads it (runtime/nsp.c, compiled here when a C compiler is around).

The packages are made here with random test keys: an AES-CTR encrypted base Program NCA with a common ticket, an
update whose RomFS section is a BKTR patch over the base's (key area key, two counter generations), Control and
Meta NCAs, and an add-on content entry."""
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
import nca
import nsp
import switchrecomp
from test_nsp import pfs
from test_nup import minimal_nso

GAME = 0x0100AAAA0000C000
HEADER_KEY = bytes(range(32))
TITLEKEK = bytes(range(0x40, 0x50))
AREA_KEK = bytes(range(0x60, 0x70))
SHARED = bytes((index * 7 + index // 251) & 0xff for index in range(0x9000))      # the data both versions share


def romfs_image(files):
    """a RomFS image with the files in its root, in the given order"""
    dirs = struct.pack('<6I', 0, 0xffffffff, 0xffffffff, 0, 0xffffffff, 0)
    table = bytearray(); data = bytearray(); names = list(files)
    offsets = []; at = 0
    for name in names:
        offsets.append(at); at += 0x20 + (len(name.encode()) + 3 & ~3)
    for index, name in enumerate(names):
        raw = name.encode(); payload = files[name]
        sibling = offsets[index + 1] if index + 1 < len(names) else 0xffffffff
        while len(data) % 16: data += b'\0'
        table += struct.pack('<IIQQII', 0, sibling, len(data), len(payload), 0xffffffff, len(raw))
        table += raw + bytes(-len(raw) % 4); data += payload
    data_at = (0x50 + len(dirs) + len(table) + 15) & ~15
    header = struct.pack('<10Q', 0x50, 0, 0, 0x50, len(dirs), 0, 0, 0x50 + len(dirs), len(table), data_at)
    return (header + dirs + table).ljust(data_at, b'\0') + bytes(data)


def fs_header(kind, crypto, counter, data_at=0, data_size=0):
    """kind: 'pfs' or 'romfs'. counter: (generation, secure value)"""
    fs = bytearray(0x200); fs[0:2] = b'\x02\x00'
    if kind == 'pfs':
        fs[2:5] = bytes((1, 2, crypto)); struct.pack_into('<QQ', fs, 0x40, data_at, data_size)
    else:
        fs[2:5] = bytes((0, 3, crypto)); fs[8:12] = b'IVFC'
        struct.pack_into('<QQI', fs, 0x18 + 5 * 0x18, data_at, data_size, 14)
    struct.pack_into('<II', fs, 0x140, counter[0], counter[1])
    return fs


def upper(counter, generation=None):
    return struct.pack('>II', counter[1], counter[0] if generation is None else generation)


def build_nca(content_type, title_id, sections, key, rights=None, generation=3):
    """sections: [(fs header, plaintext bytes, encrypt(offset in NCA, bytes) or None)]. Returns the encrypted NCA."""
    header = bytearray(0xc00); header[0x200:0x204] = b'NCA3'; header[0x205] = content_type
    header[0x206] = 2; header[0x220] = generation
    struct.pack_into('<Q', header, 0x210, title_id)
    if rights: header[0x230:0x240] = rights
    else: header[0x320:0x330] = nca.ecb(AREA_KEK, key, True)
    body = bytearray(); at = 0xc00
    for index, (fs, plain, seal) in enumerate(sections):
        plain = bytes(plain) + bytes(-len(plain) % 0x200)
        struct.pack_into('<II', header, 0x240 + index * 0x10, at // 0x200, (at + len(plain)) // 0x200)
        header[0x400 + index * 0x200:0x600 + index * 0x200] = fs
        body += seal(at, plain) if seal else plain; at += len(plain)
    struct.pack_into('<Q', header, 0x208, at)
    return nca.xts(HEADER_KEY, bytes(header), encrypt=True) + bytes(body)


def plain_ctr(key, counter):
    return lambda at, data: nca.ctr(key, upper(counter), at, data)


def ticket(rights, key, personalized=False):
    data = bytearray(0x2c0); struct.pack_into('<I', data, 0, 0x10004)
    data[0x180:0x190] = nca.ecb(TITLEKEK, key, True); data[0x281] = 1 if personalized else 0
    data[0x2a0:0x2b0] = rights
    return bytes(data)


def cnmt(title_id, version, kind, records):
    data = struct.pack('<QIBBHHH', title_id, version, kind, 0, 0x10, len(records), 0).ljust(0x20, b'\0') + bytes(0x10)
    for nca_id, record_type in records:
        data += bytes(0x20) + bytes.fromhex(nca_id) + (0x1000).to_bytes(6, 'little') + bytes((record_type, 0))
    return data


def meta_nca(title_id, version, kind, records, key):
    image = pfs({'meta.cnmt': cnmt(title_id, version, kind, records)})
    return build_nca(1, title_id, [(fs_header('pfs', 3, (0, 0), 0, len(image)), image, plain_ctr(key, (0, 0)))], key)


def control_nca(title_id, display, key):
    nacp = bytearray(0x4000); nacp[0x3060:0x3060 + len(display)] = display.encode()
    image = romfs_image({'control.nacp': bytes(nacp), 'icon_AmericanEnglish.dat': b'\xff\xd8icon' + display.encode()})
    return build_nca(2, title_id, [(fs_header('romfs', 3, (0, 5), 0x200, len(image)), bytes(0x200) + image,
                                    plain_ctr(key, (0, 5)))], key)


class Fixture:
    """the package's pieces, and what the composed RomFS has to be"""
    def __init__(self):
        self.base_key = bytes(range(0x10, 0x20)); self.update_key = bytes(range(0x80, 0x90))
        self.control_key = bytes(range(0xa0, 0xb0))
        self.base_rights = GAME.to_bytes(8, 'big') + bytes(7) + b'\x03'
        self.base_main = minimal_nso(); self.update_main = minimal_nso() + b'update'
        self.base_image = romfs_image({'keep.bin': SHARED, 'file.bin': b'test', 'gone.bin': b'removed by the update'})
        self.new_image = romfs_image({'keep.bin': SHARED, 'file.bin': b'patched!', 'new.bin': bytes(range(256)) * 9})

    def base(self, personalized=False):
        exefs = pfs({'main': self.base_main, 'main.npdm': b'npdm'})
        program = build_nca(0, GAME, [
            (fs_header('pfs', 3, (0, 1), 0, len(exefs)), exefs, plain_ctr(self.base_key, (0, 1))),
            (fs_header('romfs', 3, (0, 2), 0x400, len(self.base_image)), bytes(0x400) + self.base_image,
             plain_ctr(self.base_key, (0, 2)))], self.base_key, self.base_rights)
        files = {'aa' * 16 + '.nca': program,
                 'ab' * 16 + '.nca': control_nca(GAME, '1.0.0', self.control_key),
                 'ac' * 16 + '.cnmt.nca': meta_nca(GAME, 0, 0x80, [('aa' * 16, 1), ('ab' * 16, 3)], self.control_key)}
        files[self.base_rights.hex() + '.tik'] = ticket(self.base_rights, self.base_key, personalized)
        return files

    def update(self):
        """the patch section: [hash area + the new image's head][the new image's tail][indirect][AesCtrEx]"""
        counter = (3, 2)                                  # the section's own generation is 3
        base_at = 0x400 + self.base_image.index(SHARED)   # in the base's RomFS section
        shared_at = self.new_image.index(SHARED)
        head = bytes(0x800) + self.new_image[:shared_at]; tail = self.new_image[shared_at + len(SHARED):]
        physical = head + tail; physical += bytes(-len(physical) % 16)
        relocations = [(0, 0, 1), (len(head), base_at, 0), (len(head) + len(SHARED), len(head), 1)]
        virtual_size = 0x800 + len(self.new_image)
        indirect_at = len(physical); table_size = 0x8000; ex_at = indirect_at + table_size
        # generations: the data changes its generation in the middle of an AES block, the tables keep the section's
        generations = [(0, 5), (0x813, 7), (len(head) + 5, 9), (indirect_at, counter[0])]

        def table(entries, total, layout):
            head_ = struct.pack('<IIQ', 0, 1, total).ljust(0x4000, b'\0')
            bucket = struct.pack('<IIQ', 0, len(entries), total) + b''.join(struct.pack(layout, *entry) for entry in entries)
            return head_ + bucket.ljust(0x4000, b'\0')
        section = bytearray(physical + table(relocations, virtual_size, '<QQI')
                            + table([(at, 0, generation) for at, generation in generations], indirect_at, '<QII'))

        def seal(at, data):
            out = bytearray()
            for index, (start, generation) in enumerate(generations):
                end = generations[index + 1][0] if index + 1 < len(generations) else len(data)
                out += nca.ctr(self.update_key, upper(counter, generation), at + start, data[start:end])
            return bytes(out)
        fs = fs_header('romfs', 4, counter, 0x800, len(self.new_image))
        struct.pack_into('<QQ4s', fs, 0x100, indirect_at, table_size, b'BKTR')
        struct.pack_into('<QQ4s', fs, 0x120, ex_at, table_size, b'BKTR')
        exefs = pfs({'main': self.update_main, 'main.npdm': b'npdm2'})
        program = build_nca(0, GAME, [(fs_header('pfs', 3, (3, 1), 0, len(exefs)), exefs, plain_ctr(self.update_key, (3, 1))),
                                      (fs, section, seal)], self.update_key)
        return {'ba' * 16 + '.nca': program,
                'bb' * 16 + '.nca': control_nca(GAME, '1.2.3', self.control_key),
                'bc' * 16 + '.cnmt.nca': meta_nca(GAME + 0x800, 262144, 0x81, [('ba' * 16, 1), ('bb' * 16, 3)], self.control_key)}

    def addon(self):
        return {'cc' * 16 + '.cnmt.nca': meta_nca(GAME + 0x1001, 65536, 0x82, [('cd' * 16, 2)], self.control_key)}


class CombinedTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.root = Path(self.tmp.name)
        self.keys = self.root / 'prod.keys'
        self.keys.write_text('header_key = %s\ntitlekek_02 = %s\nkey_area_key_application_02 = %s\n' % (
            HEADER_KEY.hex(), TITLEKEK.hex(), AREA_KEK.hex()))
        self.fixture = Fixture()
        self.package = self.root / 'Gam\u00e9 [v262144] (1G+1U+1D).nsp'

    def tearDown(self): self.tmp.cleanup()

    def write(self, *parts):
        files = {}
        for part in parts: files.update(part)
        self.package.write_bytes(pfs(files))

    def files(self, package):
        wrapper = nsp.romfs_of(package)
        return {name: wrapper.read(node) for name, node in wrapper.walk_files()}

    def test_aes_engines_agree(self):
        key = bytes(range(16)); plain = bytes.fromhex('00112233445566778899aabbccddeeff')
        cipher = bytes.fromhex('69c4e0d86a7b0430d8cdb78070b4c55a')        # FIPS-197 C.1
        self.assertEqual(nca.ecb(key, plain, True), cipher)
        self.assertEqual(nca.ecb(key, cipher), plain)
        pure = nca._pure()
        self.assertEqual(pure(key, plain * 3, True), cipher * 3)
        self.assertEqual(pure(key, cipher, False), plain)
        data = bytes(range(256)) * 5
        sealed = nca.xts(HEADER_KEY, data[:0x400], encrypt=True)
        self.assertNotEqual(sealed, data[:0x400]); self.assertEqual(nca.xts(HEADER_KEY, sealed), data[:0x400])
        self.assertEqual(nca.ctr(key, bytes(8), 0x1235, nca.ctr(key, bytes(8), 0x1235, data)), data)

    def test_game_with_update_and_dlc(self):
        self.write(self.fixture.base(), self.fixture.update(), self.fixture.addon())
        package = nsp.load(self.package, self.keys)
        self.assertIsInstance(package, nca.Package)
        entries = {name: reader() for name, _, reader in package.exefs()}
        self.assertEqual(entries['main'], self.fixture.update_main)       # the code is the update's
        self.assertEqual(entries['main.npdm'], b'npdm2')
        self.assertEqual(self.files(package), {'keep.bin': SHARED, 'file.bin': b'patched!', 'new.bin': bytes(range(256)) * 9})
        self.assertEqual((package.title_id, package.version, package.display_version), (GAME, 262144, '1.2.3'))
        self.assertEqual(package.addons, [GAME + 0x1001])
        self.assertTrue(package.control['icon_AmericanEnglish.dat'].endswith(b'1.2.3'))
        marker = package.marker().splitlines()
        self.assertEqual(marker[:3], ['SWRNSP2', str(self.package), 'keys ' + str(self.keys)])
        self.assertEqual([row.split()[0] for row in marker[3:]], ['romfs', 'base', 'patch'])
        # the build folder gets key names and wrapped keys only
        for secret in (self.fixture.base_key, self.fixture.update_key, TITLEKEK, AREA_KEK, HEADER_KEY[:16]):
            self.assertNotIn(secret.hex(), package.marker())
        self.assertIn('titlekek_02:', marker[4]); self.assertIn('key_area_key_application_02:', marker[5])

    def test_encrypted_game_alone(self):
        self.write(self.fixture.base())
        package = nsp.load(self.package, self.keys)
        self.assertIsInstance(package, nca.Package)
        self.assertEqual({name: reader() for name, _, reader in package.exefs()}['main'], self.fixture.base_main)
        self.assertEqual(self.files(package)['gone.bin'], b'removed by the update')
        self.assertEqual((package.version, package.display_version, package.addons), (0, '1.0.0', []))
        self.assertEqual([row.split()[0] for row in package.marker().splitlines()[3:]], ['romfs', 'base'])

    def test_refusals(self):
        self.write(self.fixture.update())
        with self.assertRaisesRegex(nsp.Error, 'update without its game'): nsp.load(self.package, self.keys)
        self.write(self.fixture.base(personalized=True), self.fixture.update())
        with self.assertRaisesRegex(nsp.Error, 'bound to one console'): nsp.load(self.package, self.keys)
        files = self.fixture.base(); files = {name: data for name, data in files.items() if not name.endswith('.tik')}
        self.write(files)
        with self.assertRaisesRegex(nsp.Error, 'needs the ticket'): nsp.load(self.package, self.keys)
        self.write(self.fixture.base(), self.fixture.update())
        self.keys.write_text('header_key = %s\ntitlekek_02 = %s\n' % (HEADER_KEY.hex(), TITLEKEK.hex()))
        with self.assertRaisesRegex(nsp.Error, 'key_area_key_application_02'): nsp.load(self.package, self.keys)
        self.keys.write_text('header_key = %s\ntitlekek_02 = %s\nkey_area_key_application_02 = %s\n' % (
            HEADER_KEY.hex(), bytes(16).hex(), AREA_KEK.hex()))
        with self.assertRaisesRegex(nsp.Error, 'did not decrypt'): nsp.load(self.package, self.keys)

    def test_build_stages_the_update(self):
        """copy_data: the update's ExeFS, the marker, the control data and the DLC hint"""
        self.write(self.fixture.base(), self.fixture.update(), self.fixture.addon())
        os.environ['SWR_PROD_KEYS'] = str(self.keys)
        try:
            package = nsp.load(self.package)
            said = []
            data = self.root / 'Game_data'
            switchrecomp.copy_data([], b'', nsp.romfs_of(package), str(self.package), str(data), said.append)
        finally:
            del os.environ['SWR_PROD_KEYS']
        self.assertEqual((data / 'exefs' / 'main.npdm').read_bytes(), b'npdm2')
        self.assertTrue((data / 'romfs.nsp').read_text().startswith('SWRNSP2\n'))
        self.assertEqual((data / 'control.nacp').read_bytes()[0x3060:0x3065], b'1.2.3')
        self.assertTrue((data / 'icon_AmericanEnglish.dat').is_file())
        self.assertTrue(any('1.2.3 (v262144)' in line for line in said), said)
        self.assertTrue(any(line.startswith('DLC:') and '%016x' % (GAME + 0x1001) in line for line in said), said)

    def test_output_name_is_ascii(self):
        self.assertEqual(switchrecomp.output_name('Pokémon Legends - Arceus (1G+1U)'), 'Pokemon Legends - Arceus 1G1U')
        self.assertEqual(switchrecomp.output_name('Game v1.0 [0100]'), 'Game v10 0100')
        self.assertEqual(switchrecomp.output_name('ゼルダ'), 'Game')

    def runtime(self, parts, expected, update=None):
        target_win = sys.platform == 'win32'
        cc = os.environ.get('CC') or switchrecomp.find_cc(None, target_win)
        if not cc or switchrecomp.is_tcc(cc): self.skipTest('no GCC or Clang to compile the runtime test with')
        self.write(*parts)
        package = nsp.load(self.package, self.keys, update=update)
        marker = self.root / 'romfs.nsp'; marker.write_text(package.marker(), encoding='utf-8', newline='\n')
        image = self.root / 'expected.bin'; image.write_bytes(expected)
        exe = self.root / ('nsp_runtime' + ('.exe' if target_win else ''))
        command = [cc, '-O1', '-g', '-w', '-I', str(ROOT / 'runtime'), str(ROOT / 'tests' / 'test_nsp_runtime.c'),
                   str(ROOT / 'runtime' / 'nsp.c'), '-o', str(exe)]
        if not target_win: command[1:1] = ['-fsanitize=address,undefined']
        env = dict(os.environ, PATH=os.path.dirname(cc) + os.pathsep + os.environ.get('PATH', ''))
        built = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env)
        if built.returncode and not target_win:       # a compiler without the sanitizer runtimes
            command = [part for part in command if not part.startswith('-fsanitize')]
            built = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env)
        self.assertEqual(built.returncode, 0, built.stdout)
        env.pop('SWR_PROD_KEYS', None)
        return lambda mode, **more: subprocess.run([str(exe), str(marker), str(image), mode], stdout=subprocess.PIPE,
                                                   stderr=subprocess.STDOUT, text=True, env=dict(env, **more))

    def test_runtime_reads_the_patched_romfs(self):
        run = self.runtime([self.fixture.base(), self.fixture.update()], self.fixture.new_image)
        for mode in ('auto', 'portable'):       # AES-NI when the CPU has it, then the plain C cipher
            ran = run(mode)
            self.assertEqual(ran.returncode, 0, ran.stdout); self.assertIn('PASS', ran.stdout)
        # without the keys the game is told what is missing, and gets no garbage
        moved = self.root / 'moved.keys'; os.replace(self.keys, moved)
        ran = run('auto')
        self.assertNotEqual(ran.returncode, 0); self.assertIn('from your prod.keys', ran.stdout)
        ran = run('auto', SWR_PROD_KEYS=str(moved))
        self.assertEqual(ran.returncode, 0, ran.stdout)

    def test_runtime_reads_an_encrypted_game(self):
        ran = self.runtime([self.fixture.base()], self.fixture.base_image)('auto')
        self.assertEqual(ran.returncode, 0, ran.stdout); self.assertIn('PASS', ran.stdout)


class SeparateUpdateTest(CombinedTest):
    """the update in a package of its own (docs/UPDATES.md): Package(game, update=...)"""
    test_aes_engines_agree = test_output_name_is_ascii = None      # the parent's cases are not run twice
    test_game_with_update_and_dlc = test_encrypted_game_alone = test_refusals = test_build_stages_the_update = None
    test_runtime_reads_the_patched_romfs = test_runtime_reads_an_encrypted_game = None

    def separate(self, update_name='Game [UPD] [v262144].nsp', container=pfs):
        self.write(self.fixture.base(), self.fixture.addon())
        update = self.root / update_name; update.write_bytes(container(self.fixture.update()))
        return update

    def test_update_of_its_own_package(self):
        update = self.separate()
        alone = nsp.load(self.package, self.keys)
        self.assertEqual((alone.version, alone.display_version, alone.update), (0, '1.0.0', None))
        package = nsp.load(self.package, self.keys, update=update)
        entries = {name: reader() for name, _, reader in package.exefs()}
        self.assertEqual((entries['main'], entries['main.npdm']), (self.fixture.update_main, b'npdm2'))
        self.assertEqual(self.files(package), {'keep.bin': SHARED, 'file.bin': b'patched!', 'new.bin': bytes(range(256)) * 9})
        self.assertEqual((package.title_id, package.version, package.display_version), (GAME, 262144, '1.2.3'))
        self.assertEqual(package.addons, [GAME + 0x1001])                 # the DLC hint still comes from the game's package
        marker = package.marker().splitlines()
        self.assertEqual(marker[:3], ['SWRNSP2', str(self.package), 'keys ' + str(self.keys)])
        self.assertEqual([row.split()[0] for row in marker[3:]], ['romfs', 'base', 'update', 'patch'])
        self.assertEqual(marker[5], 'update ' + str(update))
        for secret in (self.fixture.base_key, self.fixture.update_key, TITLEKEK, AREA_KEK):
            self.assertNotIn(secret.hex(), package.marker())

    def test_combined_package_without_its_update_and_with_another(self):
        self.write(self.fixture.base(), self.fixture.update())
        base = nsp.load(self.package, self.keys, embedded=False)
        self.assertIsNone(base.update); self.assertIsNotNone(base.embedded_update)
        self.assertEqual((base.version, base.display_version), (0, '1.0.0'))
        self.assertEqual({name: reader() for name, _, reader in base.exefs()}['main'], self.fixture.base_main)
        self.assertEqual(self.files(base)['gone.bin'], b'removed by the update')
        self.assertEqual([row.split()[0] for row in base.marker().splitlines()[3:]], ['romfs', 'base'])
        # an update of its own package wins over the one inside the game's package
        update = self.root / 'other update.nsp'; update.write_bytes(pfs(self.fixture.update()))
        package = nsp.load(self.package, self.keys, update=update)
        self.assertEqual(package.update_path, str(update)); self.assertEqual(package.update.path, str(update))
        self.assertEqual(self.files(package)['file.bin'], b'patched!')

    def test_card_image_and_bare_nca(self):
        from test_nsp_dlc import xci
        update = self.separate('update.xci', xci)
        self.assertEqual(self.files(nsp.load(self.package, self.keys, update=update))['file.bin'], b'patched!')
        bare = self.root / 'bare' / 'patch.nca'; bare.parent.mkdir()
        bare.write_bytes(self.fixture.update()['ba' * 16 + '.nca'])
        package = nsp.load(self.package, self.keys, update=bare)
        self.assertEqual(self.files(package)['file.bin'], b'patched!')
        self.assertEqual((package.version, package.display_version), (0, '1.0.0'))      # a bare NCA brings no metadata: the game's shows

    def test_refusals_of_updates(self):
        update = self.separate()
        other = Fixture(); other_files = other.update()
        # an update of another game
        saved = GAME; globals()['GAME'] = saved + 0x10000
        try: (self.root / 'other.nsp').write_bytes(pfs(Fixture().update()))
        finally: globals()['GAME'] = saved
        with self.assertRaisesRegex(nsp.Error, 'not for this game'): nsp.load(self.package, self.keys, update=self.root / 'other.nsp')
        (self.root / 'game.nsp').write_bytes(pfs(self.fixture.base()))
        with self.assertRaisesRegex(nsp.Error, 'holds no game update'): nsp.load(self.package, self.keys, update=self.root / 'game.nsp')
        with self.assertRaisesRegex(nsp.Error, 'missing'): nsp.load(self.package, self.keys, update=self.root / 'nowhere.nsp')
        files = dict(other_files); files['ba' * 16 + '.ncz'] = files.pop('ba' * 16 + '.nca')
        (self.root / 'packed.nsz').write_bytes(pfs(files))
        with self.assertRaisesRegex(nsp.Error, 'compressed package'): nsp.load(self.package, self.keys, update=self.root / 'packed.nsz')
        # the update as the source, with or without another update
        with self.assertRaisesRegex(nsp.Error, 'update without its game|update on its own'): nsp.load(update, self.keys)
        with self.assertRaisesRegex(nsp.Error, 'update on its own'): nsp.load(update, self.keys, update=update)
        import nxloader
        folder = self.root / 'dump' / 'exefs'; folder.mkdir(parents=True); (folder / 'main').write_bytes(minimal_nso())
        with self.assertRaisesRegex(ValueError, 'needs the game as an NSP'): nxloader.load_program(str(folder.parent), update=str(update))

    def test_update_info(self):
        update = self.separate()
        self.assertEqual(nca.update_info(update, self.keys), {'title_id': GAME, 'version': 262144, 'display': '1.2.3'})
        self.assertEqual(nca.update_info(update, self.keys, game=self.package)['version'], 262144)
        with self.assertRaisesRegex(nsp.Error, 'holds no game update'): nca.update_info(self.package, self.keys)
        with self.assertRaisesRegex(nsp.Error, 'needs the game as an NSP'): nca.update_info(update, self.keys, game=self.root)
        env = dict(os.environ, SWR_PROD_KEYS=str(self.keys), PYTHONUTF8='1')
        run = lambda *args: subprocess.run([sys.executable, str(ROOT / 'switchrecomp.py')] + [str(arg) for arg in args], env=env,
                                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding='utf-8')
        good = run(self.package, '--update-info', update)
        self.assertEqual(good.returncode, 0, good.stdout)
        self.assertEqual(good.stdout.strip().split('\t'), ['UPDATE', str(update), '%016x' % GAME, '262144', '1.2.3'])
        bad = run(self.package, '--update-info', update, self.package)
        self.assertNotEqual(bad.returncode, 0)
        self.assertIn('UPDATE\t', bad.stdout); self.assertIn('UPDATE error: ', bad.stdout); self.assertIn('holds no game update', bad.stdout)

    def test_build_stages_the_update_and_takes_it_back(self):
        """copy_data with the update, then without: code, marker, version and the modules only one version has"""
        update = self.separate()
        data = self.root / 'Game_data'; said = []
        package = nsp.load(self.package, self.keys, update=update)
        switchrecomp.copy_data([], b'', nsp.romfs_of(package), str(self.package), str(data), said.append)
        self.assertEqual((data / 'exefs' / 'main.npdm').read_bytes(), b'npdm2')
        self.assertIn('update ' + str(update), (data / 'romfs.nsp').read_text(encoding='utf-8'))
        self.assertEqual((data / 'control.nacp').read_bytes()[0x3060:0x3065], b'1.2.3')
        self.assertTrue(any('1.2.3 (v262144)' in line and str(update) in line for line in said), said)
        (data / 'exefs' / 'subsdk9').write_bytes(b'a module of the other version')
        package = nsp.load(self.package, self.keys)
        switchrecomp.copy_data([], b'', nsp.romfs_of(package), str(self.package), str(data), said.append)
        self.assertEqual((data / 'exefs' / 'main.npdm').read_bytes(), b'npdm')
        self.assertFalse((data / 'exefs' / 'subsdk9').exists())
        self.assertNotIn('update ', (data / 'romfs.nsp').read_text(encoding='utf-8'))
        self.assertEqual((data / 'control.nacp').read_bytes()[0x3060:0x3065], b'1.0.0')

    def test_hints_stay_with_their_program(self):
        from types import SimpleNamespace
        one = [SimpleNamespace(build_id=b'A' * 32)]; two = [SimpleNamespace(build_id=b'B' * 32)]
        hints = self.root / 'Game.hints'; said = []
        switchrecomp.program_hints(str(hints), one, said.append)            # first build: nothing to keep
        self.assertFalse(hints.exists()); self.assertEqual(said, [])
        hints.write_text('8001820\n')                                       # the game ran
        switchrecomp.program_hints(str(hints), one, said.append); self.assertEqual(hints.read_text(), '8001820\n')
        switchrecomp.program_hints(str(hints), two, said.append)            # the update is applied: another program
        self.assertFalse(hints.exists()); self.assertEqual(len(said), 1)
        hints.write_text('8046820\n')
        switchrecomp.program_hints(str(hints), one, said.append)            # and taken back
        self.assertEqual(hints.read_text(), '8001820\n')
        switchrecomp.program_hints(str(hints), two, said.append); self.assertEqual(hints.read_text(), '8046820\n')
        # a hints file from before the stamp existed is taken as the current program's
        (self.root / 'Game.hints.for').unlink()
        switchrecomp.program_hints(str(hints), two, said.append); self.assertEqual(hints.read_text(), '8046820\n')

    def test_runtime_reads_the_update_from_its_own_package(self):
        update = self.root / 'Upd\u00e4te.nsp'; update.write_bytes(pfs(self.fixture.update()))
        run = self.runtime([self.fixture.base()], self.fixture.new_image, update=update)
        for mode in ('auto', 'portable'):
            ran = run(mode)
            self.assertEqual(ran.returncode, 0, ran.stdout); self.assertIn('PASS', ran.stdout)
        # the update package moved away: the game is told which file is missing
        os.replace(update, self.root / 'elsewhere.nsp')
        ran = run('auto')
        self.assertNotEqual(ran.returncode, 0); self.assertIn('the update package', ran.stdout); self.assertIn('is missing', ran.stdout)


if __name__ == '__main__': unittest.main()
