#!/usr/bin/env python3
# SwitchRecomp - Copyright 2026 MZ Labs
# Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
"""Synthetic, game-data-free tests for add-on content (DLC): the package readers and installer (dlc.py) and the
runtime's manifest / storage code (runtime/dlc.c, compiled here when a C compiler is around).

The NCAs are made up and so is their "encryption": a stand-in for hactool (written into the test folder) undoes
it, with the same options the real one takes. The header is XORed with 0x5a; an encrypted section is XORed with
one byte: the first byte of the ticket's title key block XOR the first byte of titlekek_<generation> from the
keys file (or just key_area_key_application_<generation>'s when the NCA has no rights id)."""
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / 'tests'))
import dlc
import nsp
import switchrecomp
from test_nsp import pfs, romfs

GAME = 0x0100152000022000
AOC = 0x0100152000023001
KEYS = 'header_key = %s\ntitlekek_0c = 21%s\nkey_area_key_application_0c = 33%s\n' % ('00' * 32, '00' * 15, '00' * 15)

FAKE_HACTOOL = r'''
import os, struct, sys
args = sys.argv[1:]
opt = dict(a.split('=', 1) for a in args if a.startswith('--') and '=' in a)
keys = dict((k.strip(), v.strip()) for k, v in (line.split('=', 1) for line in open(args[args.index('-k') + 1]) if '=' in line))
data = open(args[-1], 'rb').read()
if 'header_key' not in keys: sys.exit(1)
head = bytes(b ^ 0x5a for b in data[:0xc00])
if head[0x200:0x204] != b'NCA3': sys.exit(1)
if '--header' in opt: open(opt['--header'], 'wb').write(head)
def section(i):
    start, end = struct.unpack_from('<II', head, 0x240 + i * 16)
    return start * 0x200, head[0x400 + i * 0x200:0x600 + i * 0x200]
def body_key(crypto):
    if crypto != 3: return 0
    generation = '%02x' % (max(head[0x206], head[0x220]) - 1)
    if any(head[0x230:0x240]):
        if '--titlekey' not in opt: sys.exit(1)
        return int(opt['--titlekey'][:2], 16) ^ int(keys.get('titlekek_' + generation, '00')[:2], 16)
    return int(keys.get('key_area_key_application_' + generation, '00')[:2], 16)
if '--section0dir' in opt:
    at, fs = section(0); pfs_at, pfs_size = struct.unpack_from('<QQ', fs, 0x40)
    blob = data[at + pfs_at:at + pfs_at + pfs_size]
    magic, count, strings, _ = struct.unpack_from('<4sIII', blob, 0)
    names = blob[0x10 + count * 24:0x10 + count * 24 + strings]; base = 0x10 + count * 24 + strings
    os.makedirs(opt['--section0dir'], exist_ok=True)
    for i in range(count):
        off, size, name_at, _ = struct.unpack_from('<QQII', blob, 0x10 + i * 24)
        name = names[name_at:names.index(b'\0', name_at)].decode()
        open(os.path.join(opt['--section0dir'], name), 'wb').write(blob[base + off:base + off + size])
if '--romfs' in opt:
    for i in range(4):
        at, fs = section(i)
        if fs[3] == 3 and fs[8:12] == b'IVFC':
            data_at, size = struct.unpack_from('<QQ', fs, 0x18 + 5 * 0x18); key = body_key(fs[4])
            open(opt['--romfs'], 'wb').write(bytes(b ^ key for b in data[at + data_at:at + data_at + size]))
'''


def nca(content_type, title_id, payload, pfs_section=False, crypto=1, rights=b'', key=0, generation=13, at=0xc00):
    """a made-up NCA3: one section at `at` holding payload (a PFS0, or an IVFC RomFS data level 0x200 into it)"""
    lead = 0 if pfs_section else 0x200
    size = (at + lead + len(payload) + 0x1ff) & ~0x1ff
    body = bytearray(size)
    body[at + lead:at + lead + len(payload)] = bytes(b ^ key for b in payload) if crypto == 3 else payload
    header = bytearray(0xc00); header[0x200:0x204] = b'NCA3'; header[0x205] = content_type
    header[0x206] = 2; header[0x220] = generation
    struct.pack_into('<QQ', header, 0x208, size, title_id)
    header[0x230:0x230 + len(rights)] = rights
    struct.pack_into('<II', header, 0x240, at // 0x200, size // 0x200)
    fs = memoryview(header)[0x400:0x600]
    if pfs_section:
        fs[0:5] = bytes((2, 0, 1, 2, crypto)); struct.pack_into('<QQ', fs, 0x40, 0, len(payload))
    else:
        fs[0:5] = bytes((2, 0, 0, 3, crypto)); fs[8:12] = b'IVFC'
        struct.pack_into('<QQI', fs, 0x18 + 5 * 0x18, lead, len(payload), 14)
    body[:0xc00] = bytes(b ^ 0x5a for b in header)
    return bytes(body)


def cnmt(title_id, version, kind, records):
    """content metadata: records are (nca id hex, size, type)"""
    extended = 0x10 if kind in (0x80, 0x81, 0x82) else 0
    data = struct.pack('<QIBBHHH', title_id, version, kind, 0, extended, len(records), 0) + bytes(12) + bytes(extended)
    for nca_id, size, record_type in records:
        data += bytes(0x20) + bytes.fromhex(nca_id) + size.to_bytes(6, 'little') + bytes((record_type, 0))
    return data


def meta_nca(title_id, version, kind, records):
    names = {0x80: 'Application', 0x81: 'Patch', 0x82: 'AddOnContent'}
    inner = pfs({'%s_%016x.cnmt' % (names[kind], title_id): cnmt(title_id, version, kind, records)})
    return nca(1, title_id, inner, pfs_section=True)


def ticket(rights, key_block_first, personalized=False):
    body = bytearray(0x180)
    body[0:26] = b'Root-CA00000003-XS00000020'
    body[0x40] = key_block_first
    if personalized: body[0x41:0x140] = bytes(range(1, 256)) + b'\1'      # an RSA block, not a 16-byte key
    body[0x140] = 2; body[0x141] = 1 if personalized else 0; body[0x145] = 13
    body[0x160:0x170] = rights
    return struct.pack('<I', 0x10004) + bytes(0x100) + bytes(0x3c) + bytes(body)


def rights_of(title_id, generation=13):
    return struct.pack('>QQ', title_id, generation)


def dlc_package(title_id=AOC, version=65536, mode='titlekey', data_id='a3' * 16, meta_id='1f' * 16, image=None,
                personalized=False, with_ticket=True, data_at=0xc00):
    """the members of a DLC NSP: ticket, certificate, meta NCA, data NCA"""
    image = image if image is not None else romfs()
    rights = rights_of(title_id)
    if mode == 'titlekey':      # the section key is 0x77 = ticket byte 0x56 ^ titlekek_0c byte 0x21
        data = nca(5, title_id, image, crypto=3, rights=rights, key=0x77, at=data_at)
    elif mode == 'keyarea':     # no rights id: key_area_key_application_0c byte 0x33
        data = nca(5, title_id, image, crypto=3, key=0x33, at=data_at)
    else:
        data = nca(5, title_id, image, crypto=1, at=data_at)
    files = {}
    if mode == 'titlekey' and with_ticket:
        files[rights.hex() + '.tik'] = ticket(rights, 0x56, personalized)
        files[rights.hex() + '.cert'] = bytes(0x700)
    files[meta_id + '.cnmt.nca'] = meta_nca(title_id, version, 0x82, [(data_id, len(data), 2)])
    files[data_id + '.nca'] = data
    return files


def zstd_compress():
    try:
        from backports import zstd
        return zstd.compress
    except ImportError:
        pass
    try:
        from compression import zstd
        return zstd.compress
    except ImportError:
        try:
            import zstandard
            return lambda data: zstandard.ZstdCompressor().compress(data)
        except ImportError:
            return None


def to_ncz(data, blocks=False, compress=None):
    """what nsz makes of an NCA: its first 0x4000 bytes, the section table, then the rest with its sections decrypted
    and zstd compressed (one stream, or NCZBLOCK blocks). The made-up NCA's only section starts at 0x4000."""
    plain = bytes(b ^ 0x77 for b in data[0x4000:])
    out = data[:0x4000] + b'NCZSECTN' + struct.pack('<Q', 1)
    out += struct.pack('<QQQQ', 0x4000, len(plain), 3, 0) + bytes(16) + bytes(16)
    if not blocks:
        return out + compress(plain[:len(plain) // 2]) + compress(plain[len(plain) // 2:])     # two frames
    size = 1 << 14; parts = [plain[i:i + size] for i in range(0, len(plain), size)]
    stored = [compress(part) for part in parts]
    stored = [packed if len(packed) < len(part) else part for packed, part in zip(stored, parts)]
    out += b'NCZBLOCK' + struct.pack('<BBBBIQ', 2, 1, 0, 14, len(parts), len(plain))
    return out + struct.pack('<%dI' % len(stored), *map(len, stored)) + b''.join(stored)


def hfs(files):
    names = bytearray(); rows = bytearray(); data = bytearray()
    for name, payload in files.items():
        name_at = len(names); names += name.encode() + b'\0'
        rows += struct.pack('<QQII8x32x', len(data), len(payload), name_at, 0); data += payload
    return struct.pack('<4sIII', b'HFS0', len(files), len(names), 0) + rows + names + data


def xci(files, key_area=False):
    root = hfs({'update': hfs({}), 'normal': hfs({}), 'secure': hfs(files)})
    header = bytearray(0x200); header[0x100:0x104] = b'HEAD'; struct.pack_into('<Q', header, 0x130, 0x200)
    return (bytes(0x1000) if key_area else b'') + bytes(header) + root


class DlcTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.root = Path(self.tmp.name)
        self.keys = self.root / 'prod.keys'; self.keys.write_text(KEYS)
        self.tool = self.root / 'fake_hactool.py'; self.tool.write_text(FAKE_HACTOOL)
        self.data = self.root / 'Game_data'; (self.data / 'exefs').mkdir(parents=True)
        self.npdm(GAME)
        self.said = []

    def tearDown(self): self.tmp.cleanup()

    def npdm(self, title_id):
        blob = bytearray(0x100); blob[0:4] = b'META'; struct.pack_into('<I', blob, 0x70, 0x80)
        blob[0x80:0x84] = b'ACI0'; struct.pack_into('<Q', blob, 0x90, title_id)
        (self.data / 'exefs' / 'main.npdm').write_bytes(blob)

    def package(self, name, files):
        path = self.root / name; path.write_bytes(pfs(files)); return path

    def install(self, *paths, **options):
        return dlc.install([str(path) for path in paths], str(self.data), self.said.append, str(self.keys),
                           str(self.tool), **options)

    def image(self, title_id=AOC):
        return (self.data / 'dlc' / ('%016x' % title_id) / 'romfs.bin').read_bytes()

    def test_pfs0_entries_start_right_after_the_16_byte_header(self):
        # docs/DLC_IN_PLACE.md 1.1: reading the header as 24 bytes turns every size into an offset
        blob = pfs({'first.bin': b'A' * 7, 'second.bin': b'B' * 5, 'third.bin': b'C' * 3})
        path = self.root / 'three.nsp'; path.write_bytes(blob)
        self.assertEqual(nsp.PFS_HEAD.size, 0x10)
        entries = dlc.members(str(path))
        base = 0x10 + 3 * 24 + len(b'first.bin\0second.bin\0third.bin\0')
        self.assertEqual([(e.name, e.offset, e.size) for e in entries],
                         [('first.bin', base, 7), ('second.bin', base + 7, 5), ('third.bin', base + 12, 3)])
        self.assertEqual(entries[-1].offset + entries[-1].size, len(blob))      # chains exactly to the end
        self.assertEqual(blob[entries[1].offset:entries[1].offset + 5], b'BBBBB')
        self.assertEqual(struct.unpack_from('<Q', blob, 0x10)[0], 0)             # the first entry's offset field

    def test_titlekey_package_is_decrypted_with_its_common_ticket(self):
        path = self.package('dlc.nsp', dlc_package())
        done, errors = self.install(path)
        self.assertEqual((done, errors), ([AOC], []))
        self.assertEqual(self.image(), romfs())
        self.assertEqual(dlc.read_manifest(str(self.data)), [(AOC, 65536, 'data', str(path))])
        text = (self.data / 'dlc.manifest').read_text()
        self.assertIn('0100152000023001 65536 data ' + str(path), text)
        self.assertFalse(list((self.data / 'dlc').rglob('*.tmp')))             # no NCA copy is left behind
        self.assertFalse(list(self.root.glob('*.nca')))
        # the same file again is not unpacked again
        self.said.clear(); self.install(path)
        self.assertTrue(any('already installed' in line for line in self.said))

    def test_key_area_package_needs_no_ticket(self):
        path = self.package('dlc.nsp', dlc_package(mode='keyarea'))
        self.assertEqual(self.install(path), ([AOC], []))
        self.assertEqual(self.image(), romfs())

    def test_plaintext_package_is_read_in_place(self):
        files = dlc_package(mode='plain'); path = self.package('dlc.nsp', files)
        self.assertEqual(self.install(path), ([AOC], []))
        folder = self.data / 'dlc' / ('%016x' % AOC)
        self.assertFalse((folder / 'romfs.bin').exists())
        magic, source, offset, size = (folder / 'romfs.nsp').read_text().split('\n')[:4]
        self.assertEqual((magic, source), ('SWRNSP1', str(path)))
        blob = path.read_bytes()
        self.assertEqual(blob[int(offset, 16):int(offset, 16) + int(size, 16)], romfs())

    def test_combined_package_installs_only_its_add_on_content(self):
        # base + update + DLC in one NSP (docs/DLC_IN_PLACE.md 1.1)
        program = nca(0, GAME, romfs())
        files = {'aa' * 16 + '.nca': program,
                 'bb' * 16 + '.cnmt.nca': meta_nca(GAME, 0, 0x80, [('aa' * 16, len(program), 1)]),
                 'cc' * 16 + '.cnmt.nca': meta_nca(GAME + 0x800, 1507328, 0x81, [('dd' * 16, 0x1000, 1)])}
        files.update(dlc_package())
        path = self.package('combined.nsp', files)
        titles, others = dlc.scan(str(path), keys=str(self.keys), hactool=str(self.tool))
        self.assertEqual([(t.title_id, t.version) for t in titles], [(AOC, 65536)])
        self.assertEqual(sorted(o.type for o in others), [0x80, 0x81])
        self.assertEqual(self.install(path), ([AOC], []))
        self.assertEqual(self.image(), romfs())

    def test_several_titles_and_the_other_games_are_sorted_out(self):
        files = dlc_package(AOC, data_id='a1' * 16, meta_id='b1' * 16)
        files.update(dlc_package(AOC + 1, 3, data_id='a2' * 16, meta_id='b2' * 16))
        files.update(dlc_package(0x0100AAAA00003001, 1, data_id='a3' * 16, meta_id='b3' * 16))
        path = self.package('many.nsp', files)
        done, errors = self.install(path)
        self.assertEqual((done, errors), ([AOC, AOC + 1], []))
        self.assertEqual([entry[:2] for entry in dlc.read_manifest(str(self.data))], [(AOC, 65536), (AOC + 1, 3)])
        self.assertFalse((self.data / 'dlc' / '0100aaaa00003001').exists())

    def test_card_image_secure_partition(self):
        for key_area in (False, True):
            path = self.root / ('card%d.xci' % key_area); path.write_bytes(xci(dlc_package(mode='keyarea'), key_area))
            self.assertEqual(self.install(path), ([AOC], []))
            self.assertEqual(self.image(), romfs())
            self.assertEqual(dlc.remove(str(self.data), str(path), self.said.append), 1)

    def test_bare_nca(self):
        path = self.root / 'data.nca'; path.write_bytes(dlc_package(mode='keyarea')['a3' * 16 + '.nca'])
        self.assertEqual(self.install(path), ([AOC], []))
        self.assertEqual(self.image(), romfs())
        self.assertEqual(dlc.read_manifest(str(self.data)), [(AOC, 0, 'data', str(path))])
        # with a rights id it needs its ticket: beside it, or the install says so
        files = dlc_package(AOC + 1); tik = [name for name in files if name.endswith('.tik')][0]
        locked = self.root / 'locked.nca'; locked.write_bytes(files['a3' * 16 + '.nca'])
        done, errors = self.install(locked)
        self.assertEqual(done, []); self.assertRegex(errors[0], 'needs a ticket')
        (self.root / tik).write_bytes(files[tik])
        self.assertEqual(self.install(locked), ([AOC + 1], []))
        # a program NCA is not add-on content
        program = self.root / 'program.nca'; program.write_bytes(nca(0, GAME, romfs()))
        self.assertRegex(self.install(program)[1][0], 'not add-on content data')

    def compressed(self, blocks, container):
        compress = zstd_compress()
        if not compress: self.skipTest('no zstd module in this Python')
        image = romfs() + bytes(range(256)) * 300      # past one 16 KiB block
        image = image[:0x50] + image[0x50:]
        files = dlc_package(image=image, data_at=0x4000)
        name = 'a3' * 16 + '.nca'
        files['a3' * 16 + '.ncz'] = to_ncz(files.pop(name), blocks, compress)
        if container == 'nsz': path = self.package('dlc.nsz', files)
        else:
            path = self.root / 'dlc.xcz'; path.write_bytes(xci(files))
        self.assertEqual(self.install(path), ([AOC], []))
        self.assertEqual(self.image(), image)

    def test_nsz_solid(self): self.compressed(False, 'nsz')

    def test_nsz_blocks(self): self.compressed(True, 'nsz')

    def test_xcz_blocks(self): self.compressed(True, 'xcz')

    def test_broken_ncz_is_refused(self):
        compress = zstd_compress()
        if not compress: self.skipTest('no zstd module in this Python')
        files = dlc_package(data_at=0x4000); name = 'a3' * 16 + '.nca'
        packed = bytearray(to_ncz(files.pop(name), False, compress)); packed[-20:] = b'\xff' * 20
        files['a3' * 16 + '.ncz'] = bytes(packed)
        done, errors = self.install(self.package('bad.nsz', files))
        self.assertEqual(done, []); self.assertTrue(errors)
        self.assertFalse((self.data / 'dlc.manifest').exists())

    def test_what_cannot_work_is_refused_with_the_reason(self):
        cases = [
            ('personal.nsp', dlc_package(personalized=True), 'bound to one console'),
            ('noticket.nsp', dlc_package(with_ticket=False), 'needs a ticket'),
            ('other.nsp', dlc_package(0x0100AAAA00003001), 'not for this game'),
            ('game.nsp', {'bb' * 16 + '.cnmt.nca': meta_nca(GAME, 0, 0x80, [('aa' * 16, 0x1000, 1)])}, 'no add-on content'),
            ('lost.nsp', {k: v for k, v in dlc_package(mode='plain').items() if k.endswith('.cnmt.nca')}, 'not in the package'),
        ]
        for name, files, reason in cases:
            done, errors = self.install(self.package(name, files))
            self.assertEqual(done, [], name); self.assertRegex(errors[0], reason)
        for name, blob in (('junk.nsp', b'not a package at all'), ('notes.txt', b'PFS0')):
            path = self.root / name; path.write_bytes(blob)
            self.assertTrue(self.install(path)[1], name)
        self.assertTrue(self.install(self.root / 'missing.nsp')[1])
        self.assertFalse((self.data / 'dlc.manifest').exists())
        self.assertFalse((self.data / 'dlc').exists())                          # nothing half-installed is left
        # in a package with several titles the good ones are installed, the refusal names the other
        files = dlc_package(AOC, data_id='a1' * 16, meta_id='b1' * 16)
        mixed = dlc_package(AOC + 1, 2, data_id='a2' * 16, meta_id='b2' * 16, with_ticket=False); files.update(mixed)
        done, errors = self.install(self.package('mixed.nsp', files))
        self.assertEqual(done, [AOC]); self.assertEqual(len(errors), 1); self.assertRegex(errors[0], '0100152000023002 needs a ticket')
        self.assertEqual([entry[0] for entry in dlc.read_manifest(str(self.data))], [AOC])

    def test_keys_without_the_content_generation_are_named(self):
        self.keys.write_text('header_key = ' + '00' * 32 + '\n')
        done, errors = self.install(self.package('dlc.nsp', dlc_package()))
        self.assertEqual(done, []); self.assertRegex(errors[0], 'titlekek_0c')
        self.keys.write_text('nothing = 00\n')
        self.assertRegex(self.install(self.package('dlc2.nsp', dlc_package()))[1][0], 'keys')

    def test_stub_is_only_installed_on_request(self):
        path = self.package('personal.nsp', dlc_package(personalized=True))
        self.assertEqual(self.install(path, allow_stub=True), ([AOC], []))
        self.assertEqual(dlc.read_manifest(str(self.data)), [(AOC, 65536, 'stub', str(path))])

    def test_not_a_built_game(self):
        shutil.rmtree(self.data)
        with self.assertRaisesRegex(dlc.Error, 'build the game first'):
            self.install(self.package('dlc.nsp', dlc_package()))

    def test_sync_and_remove(self):
        first = self.package('first.nsp', dlc_package(AOC, data_id='a1' * 16, meta_id='b1' * 16))
        second = self.package('second.nsp', dlc_package(AOC + 1, 2, data_id='a2' * 16, meta_id='b2' * 16))
        sync = lambda *paths: dlc.sync([str(p) for p in paths], str(self.data), self.said.append, str(self.keys), str(self.tool))
        self.assertEqual(sync(first, second)[1], [])
        self.assertEqual([e[0] for e in dlc.read_manifest(str(self.data))], [AOC, AOC + 1])
        self.said.clear(); self.assertEqual(sync(first, second), ([AOC, AOC + 1], []))
        self.assertEqual(self.said, [])                                          # nothing to do, nothing done
        self.assertEqual(sync(second)[1], [])                                    # first is no longer wanted
        self.assertEqual([e[0] for e in dlc.read_manifest(str(self.data))], [AOC + 1])
        self.assertFalse((self.data / 'dlc' / ('%016x' % AOC)).exists())
        second.unlink(); self.assertEqual(sync(second), ([AOC + 1], []))         # an installed image outlives its package
        self.assertEqual(dlc.remove(str(self.data), '%016x' % (AOC + 1), self.said.append), 1)
        self.assertFalse((self.data / 'dlc.manifest').exists()); self.assertFalse((self.data / 'dlc').exists())
        self.assertEqual(dlc.remove(str(self.data), '%016x' % AOC, self.said.append), 0)
        self.assertEqual(sync()[1], [])

    def test_two_packages_with_the_same_title(self):
        # the standalone DLC file and a combined package that holds the same title
        first = self.package('standalone.nsp', dlc_package())
        again = self.package('combined.nsp', dlc_package(meta_id='2f' * 16))
        done, errors = self.install(first, again)
        self.assertEqual(done, [AOC]); self.assertRegex(errors[0], 'already installed from standalone.nsp')
        self.assertEqual(dlc.read_manifest(str(self.data)), [(AOC, 65536, 'data', str(first))])
        # a newer version of the title replaces it
        newer = self.package('newer.nsp', dlc_package(version=131072, meta_id='3f' * 16))
        self.assertEqual(self.install(newer), ([AOC], []))
        self.assertEqual(dlc.read_manifest(str(self.data)), [(AOC, 131072, 'data', str(newer))])
        # and when the installed one lost its data, the other package takes over
        (self.data / 'dlc' / ('%016x' % AOC) / 'romfs.bin').unlink()
        self.assertEqual(self.install(first), ([AOC], []))
        self.assertEqual(dlc.read_manifest(str(self.data)), [(AOC, 65536, 'data', str(first))])

    def test_manifest_accepts_the_three_column_form(self):
        (self.data / 'dlc.manifest').write_text('# comment\n\n0100152000023001 65536 P:/a b/c.nsp\n'
                                                '0100152000023002 1 stub\nnot a line\n')
        self.assertEqual(dlc.read_manifest(str(self.data)),
                         [(AOC, 65536, 'data', 'P:/a b/c.nsp'), (AOC + 1, 1, 'stub', '')])

    def test_command_line(self):
        path = self.package('dlc.nsp', dlc_package())
        env = dict(os.environ, SWR_PROD_KEYS=str(self.keys), SWR_HACTOOL=str(self.tool), PYTHONUTF8='1')
        run = lambda *args: subprocess.run([sys.executable, str(ROOT / 'switchrecomp.py')] + list(args), env=env,
                                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        done = run('--install-dlc', str(path), '--datadir', str(self.data))
        self.assertEqual(done.returncode, 0, done.stdout); self.assertIn('DLC: installed 0100152000023001 v65536', done.stdout)
        listed = run('--list-dlc', '--outdir', str(self.root), '-o', 'Game')
        self.assertIn('0100152000023001 v65536 data', listed.stdout)
        bad = run('--install-dlc', str(self.root / 'nope.nsp'), '--datadir', str(self.data))
        self.assertNotEqual(bad.returncode, 0); self.assertIn('DLC error', bad.stdout)
        self.assertEqual(run('--sync-dlc', '--datadir', str(self.data)).returncode, 0)
        self.assertFalse((self.data / 'dlc.manifest').exists())
        self.assertNotEqual(run('--remove-dlc', '0100152000023001', '--datadir', str(self.data)).returncode, 0)


class DlcRuntimeTest(unittest.TestCase):
    """runtime/dlc.c + runtime/nsp.c compiled with tests/test_dlc_runtime.c against a data folder made here"""

    def test_manifest_list_and_storages(self):
        target_win = sys.platform.startswith('win')
        cc = os.environ.get('CC') or switchrecomp.find_cc(None, target_win)
        if not cc or switchrecomp.is_tcc(cc): self.skipTest('no GCC or Clang to compile the runtime test with')
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); data = root / 'Game_data'; base = data / 'dlc'
            (base / '0100152000023001' / 'romfs' / 'sub').mkdir(parents=True)
            (base / '0100152000023001' / 'romfs' / 'version.txt').write_bytes(b'10002')
            (base / '0100152000023001' / 'romfs' / 'empty.bin').write_bytes(b'')
            (base / '0100152000023001' / 'romfs' / 'sub' / 'deep.bin').write_bytes(b'deep data')
            (base / '0100152000023002').mkdir(); (base / '0100152000023002' / 'romfs.bin').write_bytes(romfs())
            (base / '0100152000023003' / 'romfs').mkdir(parents=True)
            (base / '0100152000023003' / 'romfs' / 'version.txt').write_bytes(b'1')
            package = root / 'package.nsp'; package.write_bytes(b'xxxx' + romfs() + b'yyyy')
            (base / '01001520000237ff').mkdir()
            (base / '01001520000237ff' / 'romfs.nsp').write_bytes(
                ('SWRNSP1\n%s\n4\n%x\n' % (package, len(romfs()))).encode())
            (data / 'dlc.manifest').write_bytes(
                b'# SwitchRecomp add-on content\r\n'
                b'0100152000023002 3 data C:\\some path\\b.nsp\r\n'
                b'0100152000023001 65536 P:/x y/a.nsp\n'            # the three-column form
                b'this is not a line\n'
                b'0100152000023003 7 stub c.nsp\n'
                b'01001520000237ff 1 data d.nsp\n'
                b'0100152000023001 1 data duplicate.nsp\n'
                b'0100aaaa00003005 2 data e.nsp\n')
            # a second data folder where every title's data is unusable: a marker to a missing package, a broken image
            lost = data / 'lost' / 'dlc'
            (lost / '0100152000023001').mkdir(parents=True); (lost / '0100152000023002').mkdir()
            (lost / '0100152000023001' / 'romfs.nsp').write_bytes(('SWRNSP1\n%s\n4\n10\n' % (root / 'gone.nsp')).encode())
            (lost / '0100152000023002' / 'romfs.bin').write_bytes(b'not a RomFS image' * 8)
            (data / 'lost' / 'dlc.manifest').write_bytes(b'0100152000023001 1 data gone.nsp\n0100152000023002 1 data b.nsp\n')
            exe = root / ('test_dlc' + ('.exe' if target_win else ''))
            command = [cc, '-O1', '-g', '-w', '-I', str(ROOT / 'runtime'), str(ROOT / 'tests' / 'test_dlc_runtime.c'),
                       str(ROOT / 'runtime' / 'dlc.c'), str(ROOT / 'runtime' / 'nsp.c'), '-o', str(exe)]
            if not target_win: command[1:1] = ['-fsanitize=address,undefined']
            built = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            if built.returncode and not target_win:       # a compiler without the sanitizer runtimes
                command = [part for part in command if not part.startswith('-fsanitize')]
                built = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            self.assertEqual(built.returncode, 0, built.stdout)
            env = dict(os.environ, PATH=os.path.dirname(cc) + os.pathsep + os.environ.get('PATH', ''))
            ran = subprocess.run([str(exe), str(data).replace('\\', '/') + '/'], stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT, text=True, env=env)
            self.assertEqual(ran.returncode, 0, ran.stdout)
            self.assertIn('PASS add-on content runtime', ran.stdout)


if __name__ == '__main__': unittest.main()
