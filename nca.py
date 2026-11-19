#!/usr/bin/env python3
# SwitchRecomp - Copyright 2026 MZ Labs
# Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
# Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen.
"""Encrypted and combined application NSPs, read in place with the user's keys.

A Switch 1 package keeps its ExeFS and RomFS AES-CTR encrypted, and a package that holds the game and its update
(1G+1U, 1G+1U+1D) has two Program NCAs: the base and a patch whose RomFS section is a BKTR patch over the base's.
This module opens such a package without hactool and without copying it:

    NCA header        AES-XTS with header_key (Nintendo's big-endian sector tweak)
    content key       the ticket's title key (titlekek_XX) or the header's key area (key_area_key_application_XX)
    section data      AES-CTR; the counter is the section's 8 counter bytes reversed, then the NCA offset / 16
    patch RomFS       the indirect (relocation) table maps the patched section onto the base section and the patch
                      section; the AesCtrEx (subsection) table gives the counter generation of each patch range

The update can also come in a package of its own (Package(game, update=...)): an NSP, a card image (XCI) or a bare
NCA with its ticket beside it. Its code replaces the game's and its patch RomFS is composed over the game's base
section, each read from its own file. docs/UPDATES.md.

docs/COMBINED_NSP.md describes the formats and the runtime marker written by Package.marker().
"""
import ctypes
import functools
import os
import struct
import sys
from types import SimpleNamespace

import nsp

Error = nsp.Error


class Locked(Error):
    """the installed keys open no NCA of the package"""

MEDIA = 0x200
BLOCK = 16
MAX_BKTR = 256 << 20


# ------------------------------------------------------------------ AES-128-ECB, the only primitive needed
def _bcrypt():
    lib = ctypes.WinDLL('bcrypt')
    algorithm = ctypes.c_void_p()
    if lib.BCryptOpenAlgorithmProvider(ctypes.byref(algorithm), ctypes.c_wchar_p('AES'), None, 0): raise OSError
    mode = ctypes.create_unicode_buffer('ChainingModeECB')
    if lib.BCryptSetProperty(algorithm, ctypes.c_wchar_p('ChainingMode'), mode, ctypes.sizeof(mode), 0): raise OSError

    def ecb(key, data, encrypt):
        handle = ctypes.c_void_p(); done = ctypes.c_ulong()
        if lib.BCryptGenerateSymmetricKey(algorithm, ctypes.byref(handle), None, 0, key, len(key), 0): raise OSError
        try:
            out = ctypes.create_string_buffer(len(data))
            call = lib.BCryptEncrypt if encrypt else lib.BCryptDecrypt
            if call(handle, data, len(data), None, None, 0, out, len(data), ctypes.byref(done), 0): raise OSError
            return out.raw
        finally:
            lib.BCryptDestroyKey(handle)
    return ecb


def _cryptography():
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

    def ecb(key, data, encrypt):
        cipher = Cipher(algorithms.AES(key), modes.ECB())
        worker = cipher.encryptor() if encrypt else cipher.decryptor()
        return worker.update(data) + worker.finalize()
    return ecb


def _openssl():
    import ctypes.util
    try: import ssl      # a Python with ssl has libcrypto loaded already: a bundled one finds its own copy by name
    except ImportError: pass
    lib = None
    for name in ('libcrypto.so.3', 'libcrypto.so.1.1', ctypes.util.find_library('crypto'),
                 os.path.join(sys.prefix, 'lib', 'libcrypto.so.3'), 'libcrypto.dylib'):
        try:
            if name: lib = ctypes.CDLL(name); break
        except OSError:
            continue
    if lib is None: raise OSError
    lib.EVP_CIPHER_CTX_new.restype = ctypes.c_void_p
    lib.EVP_aes_128_ecb.restype = ctypes.c_void_p
    lib.EVP_CipherInit_ex.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_char_p,
                                      ctypes.c_char_p, ctypes.c_int]
    lib.EVP_CIPHER_CTX_set_padding.argtypes = [ctypes.c_void_p, ctypes.c_int]
    lib.EVP_CipherUpdate.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.POINTER(ctypes.c_int),
                                     ctypes.c_char_p, ctypes.c_int]
    lib.EVP_CIPHER_CTX_free.argtypes = [ctypes.c_void_p]

    def ecb(key, data, encrypt):
        context = lib.EVP_CIPHER_CTX_new()
        try:
            if not lib.EVP_CipherInit_ex(context, lib.EVP_aes_128_ecb(), None, key, None, 1 if encrypt else 0):
                raise OSError
            lib.EVP_CIPHER_CTX_set_padding(context, 0)
            out = bytearray()
            for at in range(0, len(data), 1 << 30):
                part = bytes(data[at:at + (1 << 30)])
                buffer = ctypes.create_string_buffer(len(part) + BLOCK); done = ctypes.c_int()
                if not lib.EVP_CipherUpdate(context, buffer, ctypes.byref(done), part, len(part)): raise OSError
                out += buffer.raw[:done.value]
            return bytes(out)
        finally:
            lib.EVP_CIPHER_CTX_free(context)
    ecb(bytes(16), bytes(16), True)
    return ecb


def _pure():
    """plain Python AES-128, for a Python with no crypto library at all: correct, and slow (about 50 KB/s)"""
    sbox = [0] * 256; p = q = 1
    while True:
        p = p ^ ((p << 1) & 0xff) ^ (0x1b if p & 0x80 else 0)
        q ^= q << 1; q ^= q << 2; q ^= q << 4; q &= 0xff
        if q & 0x80: q ^= 0x09
        x = q ^ (q << 1 | q >> 7) ^ (q << 2 | q >> 6) ^ (q << 3 | q >> 5) ^ (q << 4 | q >> 4)
        sbox[p] = (x ^ 0x63) & 0xff
        if p == 1: break
    sbox[0] = 0x63
    inverse = [0] * 256
    for index, value in enumerate(sbox): inverse[value] = index

    def mul(a, b):
        r = 0
        while b:
            if b & 1: r ^= a
            a = ((a << 1) ^ (0x1b if a & 0x80 else 0)) & 0xff; b >>= 1
        return r

    def expand(key):
        words = [list(key[i:i + 4]) for i in range(0, 16, 4)]; rcon = 1
        for i in range(4, 44):
            t = list(words[i - 1])
            if i % 4 == 0:
                t = [sbox[t[1]] ^ rcon, sbox[t[2]], sbox[t[3]], sbox[t[0]]]; rcon = mul(rcon, 2)
            words.append([a ^ b for a, b in zip(words[i - 4], t)])
        return [sum(words[r * 4:r * 4 + 4], []) for r in range(11)]

    def block(keys, data, encrypt):
        s = [a ^ b for a, b in zip(data, keys[0 if encrypt else 10])]
        for r in range(1, 11):
            if encrypt:
                s = [sbox[s[(i + 4 * (i % 4)) % 16]] for i in range(16)]
                if r < 10:
                    s = sum(([mul(c[0], 2) ^ mul(c[1], 3) ^ c[2] ^ c[3], c[0] ^ mul(c[1], 2) ^ mul(c[2], 3) ^ c[3],
                              c[0] ^ c[1] ^ mul(c[2], 2) ^ mul(c[3], 3), mul(c[0], 3) ^ c[1] ^ c[2] ^ mul(c[3], 2)]
                             for c in (s[i:i + 4] for i in range(0, 16, 4))), [])
                s = [a ^ b for a, b in zip(s, keys[r])]
            else:
                s = [inverse[s[(i - 4 * (i % 4)) % 16]] for i in range(16)]
                s = [a ^ b for a, b in zip(s, keys[10 - r])]
                if r < 10:
                    s = sum(([mul(c[0], 14) ^ mul(c[1], 11) ^ mul(c[2], 13) ^ mul(c[3], 9),
                              mul(c[0], 9) ^ mul(c[1], 14) ^ mul(c[2], 11) ^ mul(c[3], 13),
                              mul(c[0], 13) ^ mul(c[1], 9) ^ mul(c[2], 14) ^ mul(c[3], 11),
                              mul(c[0], 11) ^ mul(c[1], 13) ^ mul(c[2], 9) ^ mul(c[3], 14)]
                             for c in (s[i:i + 4] for i in range(0, 16, 4))), [])
        return bytes(s)

    def ecb(key, data, encrypt):
        keys = expand(key)
        return b''.join(block(keys, data[at:at + BLOCK], encrypt) for at in range(0, len(data), BLOCK))
    return ecb


@functools.lru_cache(maxsize=1)
def _engine():
    choices = ([_bcrypt] if sys.platform == 'win32' else []) + [_cryptography, _openssl, _pure]
    if os.environ.get('SWR_PURE_AES'): choices = [_pure]
    for choice in choices:
        try:
            return choice()
        except Exception:
            continue
    raise Error('no AES implementation is available')


def ecb(key, data, encrypt=False):
    if len(key) != BLOCK or len(data) % BLOCK:
        raise Error('invalid AES-128 input')
    return _engine()(bytes(key), bytes(data), encrypt) if data else b''


def _xor(left, right):
    return (int.from_bytes(left, 'little') ^ int.from_bytes(right, 'little')).to_bytes(len(left), 'little')


def ctr(key, upper, offset, data):
    """AES-CTR as an NCA section uses it: `upper` is the first 8 counter bytes, `offset` the position in the NCA"""
    if not data: return b''
    first = offset // BLOCK; skip = offset % BLOCK
    count = (skip + len(data) + BLOCK - 1) // BLOCK
    counters = b''.join([upper + number.to_bytes(8, 'big') for number in range(first, first + count)])
    stream = ecb(key, counters, True)[skip:skip + len(data)]
    return _xor(data, stream)


def xts(key, data, sector=0, sector_size=MEDIA, encrypt=False):
    """AES-XTS over whole sectors with Nintendo's tweak (the sector number big-endian)"""
    if len(key) != 32 or len(data) % sector_size: raise Error('invalid XTS input')
    out = bytearray()
    for at in range(0, len(data), sector_size):
        tweak = int.from_bytes(ecb(key[16:], (sector + at // sector_size).to_bytes(16, 'big'), True), 'little')
        tweaks = bytearray()
        for _ in range(sector_size // BLOCK):
            tweaks += tweak.to_bytes(16, 'little')
            tweak <<= 1
            if tweak >> 128: tweak = (tweak & ((1 << 128) - 1)) ^ 0x87
        part = _xor(data[at:at + sector_size], tweaks)
        out += _xor(ecb(key[:16], part, encrypt), tweaks)
    return bytes(out)


# ------------------------------------------------------------------ keys
class Keys:
    def __init__(self, path=None):
        self.path = nsp._keys_path(path)
        self.values = {}
        with open(self.path, encoding='utf-8', errors='replace') as fileobj:
            for line in fileobj:
                name, mark, value = line.partition('=')
                if not mark: continue
                try: self.values[name.strip().lower()] = bytes.fromhex(value.strip())
                except ValueError: continue

    def get(self, name, size=16):
        value = self.values.get(name)
        if value is None or len(value) != size:
            raise Error('the installed keys have no %s (dump prod.keys again from a console with current firmware)' % name)
        return value

    def header_key(self):
        if len(self.values.get('header_key', b'')) == 32: return self.values['header_key']
        kek = ecb(self.get('master_key_00'), self.get('aes_kek_generation_source'))
        kek = ecb(ecb(kek, self.get('header_kek_source')), self.get('aes_key_generation_source'))
        return ecb(kek, self.get('header_key_source', 32))


def ticket_key(data):
    """(rights id, title key still wrapped with titlekek, or None when the ticket is bound to one console)"""
    if len(data) < 4: return None, None
    kind, = struct.unpack_from('<I', data, 0)
    body = {0x10000: 0x240, 0x10003: 0x240, 0x10001: 0x140, 0x10004: 0x140, 0x10002: 0x80, 0x10005: 0x80}.get(kind)
    if body is None or len(data) < body + 0x170: return None, None
    return data[body + 0x160:body + 0x170], None if data[body + 0x141] else data[body + 0x40:body + 0x50]


# ------------------------------------------------------------------ NCA
class Section:
    """one filesystem section of an NCA inside a package file"""
    def __init__(self, nca, index, start, end, fs):
        self.nca, self.index, self.fs = nca, index, fs
        self.offset, self.size = start, end - start          # in the NCA
        self.partition, self.fs_type, self.crypto = fs[2], fs[3], fs[4]
        self.upper = fs[0x140:0x148][::-1]                   # secure value, then generation, both big-endian
        self.patch = fs[0x110:0x114] == b'BKTR' or self.crypto == 4
        self.sparse = any(fs[0x148:0x178]); self.compressed = any(fs[0x178:0x1a0])

    def raw(self, offset, size):
        if offset < 0 or size < 0 or offset + size > self.size:
            raise Error('%s: read past the end of an NCA section' % self.nca.path)
        return nsp._read(self.nca.path, self.nca.offset + self.offset + offset, size)

    def read(self, offset, size, generation=None):
        data = self.raw(offset, size)
        if self.crypto == 1: return data
        upper = self.upper if generation is None else self.upper[:4] + struct.pack('>I', generation)
        return ctr(self.nca.key, upper, self.offset + offset, data)

    def ivfc_data(self):
        """(offset, size) of the RomFS image, the last IVFC level, in this section"""
        if self.fs[8:12] != b'IVFC': raise Error('%s: the RomFS section has no IVFC header' % self.nca.path)
        return struct.unpack_from('<QQ', self.fs, 0x18 + 5 * 0x18)

    def layer(self):
        """how the runtime reaches this section: file offset, size, NCA offset, counter and the wrapped key"""
        return '%x %x %x %d %s %s' % (self.nca.offset + self.offset, self.size, self.offset, self.crypto,
                                      self.upper.hex(), self.nca.wrapped if self.crypto != 1 else '-')


class Nca:
    def __init__(self, path, member, keys, tickets):
        self.path, self.name, self.offset, self.size = path, member.name, member.offset, member.size
        if member.size < 0xc00: raise Error('%s: %s is too small for an NCA' % (path, member.name))
        self.header = header = xts(keys.header_key(), nsp._read(path, member.offset, 0xc00))
        if header[0x200:0x204] != b'NCA3':
            raise Error('%s: the installed keys do not open %s (or it is not an NCA3)' % (path, member.name))
        self.content_type = header[0x205]
        self.title_id, = struct.unpack_from('<Q', header, 0x210)
        generation = max(header[0x206], header[0x220]); generation = max(generation - 1, 0)
        self.rights = header[0x230:0x240]
        self.key = self.wrapped = None
        self.sections = []
        for index in range(4):
            start, end = struct.unpack_from('<II', header, 0x240 + index * 0x10)
            if not start and not end: continue
            if end < start or end * MEDIA > member.size:
                raise Error('%s: an NCA section of %s is out of range' % (path, member.name))
            self.sections.append(Section(self, index, start * MEDIA, end * MEDIA,
                                         header[0x400 + index * 0x200:0x600 + index * 0x200]))
        if all(section.crypto == 1 for section in self.sections): return
        if any(self.rights):
            wrapped = tickets.get(self.rights, False)
            if wrapped is False:
                raise Error('%s: %s needs the ticket %s.tik and the package has none' % (path, member.name, self.rights.hex()))
            if wrapped is None:
                raise Error('%s: the ticket %s is bound to one console (a personalized ticket); dump the game with a '
                            'common ticket' % (path, self.rights.hex()))
            name = 'titlekek_%02x' % generation
        else:
            kind = header[0x207]
            if kind > 2: raise Error('%s: %s uses an unknown key area key' % (path, member.name))
            name = 'key_area_key_%s_%02x' % (('application', 'ocean', 'system')[kind], generation)
            wrapped = header[0x320:0x330]
        self.key = ecb(keys.get(name), wrapped)
        self.wrapped = '%s:%s' % (name, wrapped.hex())

    def section(self, fs_type, partition):
        for section in self.sections:
            if section.fs_type == fs_type and section.partition == partition: return section
        return None


# ------------------------------------------------------------------ BKTR: a patch RomFS section over the base's
class Patched:
    """the patched RomFS section. read() takes offsets in that virtual section"""
    def __init__(self, patch, base):
        self.patch, self.base = patch, base
        fs = patch.fs
        (indirect_at, indirect_size), (ex_at, ex_size) = struct.unpack_from('<QQ', fs, 0x100), struct.unpack_from('<QQ', fs, 0x120)
        if fs[0x110:0x114] != b'BKTR' or fs[0x130:0x134] != b'BKTR':
            raise Error('%s: the update has no BKTR tables' % patch.nca.path)
        for at, size in ((indirect_at, indirect_size), (ex_at, ex_size)):
            if size > MAX_BKTR or at > patch.size or size > patch.size - at:
                raise Error('%s: a BKTR table is out of range' % patch.nca.path)
        self.tables = (indirect_at, indirect_size, ex_at, ex_size)
        # both tables are stored with the section's own counter; then every patch range has its generation
        self.relocations, self.size = self._buckets(patch.read(indirect_at, indirect_size), 0x14, '<QQI')
        generations, _ = self._buckets(patch.read(ex_at, ex_size), 0x10, '<QII')
        self.generations = [(at, generation) for at, _, generation in generations]
        if not self.relocations or self.relocations[0][0] != 0 or not self.generations:
            raise Error('%s: the BKTR tables of the update are empty or do not start at 0 (wrong keys?)' % patch.nca.path)
        for index, (virtual, physical, source) in enumerate(self.relocations):
            end = self.relocations[index + 1][0] if index + 1 < len(self.relocations) else self.size
            limit = patch.size if source else (base.size if base else 0)
            if source > 1 or end < virtual or physical > limit or end - virtual > limit - physical:
                if not source and not base:
                    raise Error('%s: this is an update without its game; use a package that holds both' % patch.nca.path)
                raise Error('%s: the update does not fit the game in this package (BKTR range out of bounds)' % patch.nca.path)

    def _buckets(self, table, entry_size, layout):
        if len(table) < 0x4000: raise Error('%s: a BKTR table is cut short' % self.patch.nca.path)
        _, buckets, total = struct.unpack_from('<IIQ', table, 0)
        entries = []
        for bucket in range(buckets):
            at = 0x4000 + bucket * 0x4000
            if at + 0x4000 > len(table): raise Error('%s: a BKTR bucket is out of range' % self.patch.nca.path)
            _, count, _ = struct.unpack_from('<IIQ', table, at)
            if count > 0x3ff0 // entry_size: raise Error('%s: a BKTR bucket is too large' % self.patch.nca.path)
            entries += [struct.unpack_from(layout, table, at + 0x10 + index * entry_size) for index in range(count)]
        if any(entries[index][0] > entries[index + 1][0] for index in range(len(entries) - 1)):
            raise Error('%s: a BKTR table is not sorted' % self.patch.nca.path)
        return entries, total

    @staticmethod
    def _find(entries, offset):
        low, high = 0, len(entries) - 1
        while low < high:
            middle = (low + high + 1) // 2
            if entries[middle][0] <= offset: low = middle
            else: high = middle - 1
        return low

    def _patch(self, offset, size):
        out = bytearray()
        index = self._find(self.generations, offset)
        while size:
            end = self.generations[index + 1][0] if index + 1 < len(self.generations) else self.patch.size
            part = min(size, end - offset)
            if part <= 0: raise Error('%s: a BKTR patch range is out of bounds' % self.patch.nca.path)
            out += self.patch.read(offset, part, self.generations[index][1])
            offset += part; size -= part; index += 1
        return bytes(out)

    def read(self, offset, size):
        if offset < 0 or size < 0 or offset + size > self.size:
            raise Error('%s: read past the end of the patched RomFS' % self.patch.nca.path)
        out = bytearray()
        index = self._find(self.relocations, offset)
        while size:
            virtual, physical, source = self.relocations[index]
            end = self.relocations[index + 1][0] if index + 1 < len(self.relocations) else self.size
            part = min(size, end - offset); at = physical + offset - virtual
            out += self._patch(at, part) if source else self.base.read(at, part)
            offset += part; size -= part; index += 1
        return bytes(out)


def dlc_base(title_id):
    return title_id & ~0x1fff


# ------------------------------------------------------------------ the package
def open_ncas(path, keys):
    """the NCAs of a package that the keys open: an NSP (PFS0), a card image (the secure partition) or one bare NCA,
    whose ticket is then looked for beside it. Compressed members (.ncz) cannot be read in place and are refused."""
    import dlc
    members = dlc.members(path)
    bare = os.path.splitext(path)[1].lower() == '.nca'
    tickets = {}
    blobs = [nsp._read(path, member.offset, member.size) for member in members
             if member.name.lower().endswith('.tik') and member.size <= 0x10000]
    if bare:
        folder = os.path.dirname(path)
        for name in sorted(os.listdir(folder)):
            beside = os.path.join(folder, name)
            if name.lower().endswith('.tik') and os.path.getsize(beside) <= 0x10000:
                with open(beside, 'rb') as fileobj: blobs.append(fileobj.read())
    for blob in blobs:
        rights, wrapped = ticket_key(blob)
        if rights and tickets.get(rights) is None: tickets[rights] = wrapped
    ncas = []; locked = 0
    for member in members:
        name = member.name.lower()
        if name.endswith('.ncz') and not name.endswith('.cnmt.ncz'):
            raise Error('%s: this is a compressed package (NSZ / XCZ). Its data cannot be read in place: decompress it '
                        'to an NSP or XCI first (nsz -D)' % path)
        if not name.endswith('.nca'): continue
        try:
            ncas.append(Nca(path, member, keys, tickets))
        except Error:
            if name.endswith('.cnmt.nca'): continue
            header = xts(keys.header_key(), nsp._read(path, member.offset, 0xc00)) if member.size >= 0xc00 else b''
            if header[0x200:0x204] != b'NCA3': locked += 1; continue
            if header[0x205] == 0: raise      # a Program NCA that the keys or tickets don't open: say why
    return ncas, locked


def _is_patch(nca):
    return any(section.patch for section in nca.sections)


class Package:
    """an application package whose Program NCA is encrypted and/or comes with an update: the update inside the same
    package, or the one of `update`, a package of its own. embedded=False leaves the package's own update out (the
    game as it was released)."""
    def __init__(self, path, keys=None, update=None, embedded=True):
        self.path = os.path.abspath(path)
        self.keys = Keys(keys)
        self.keys_path = self.keys.path
        self.ncas, _ = open_ncas(self.path, self.keys)
        programs = [nca for nca in self.ncas if nca.content_type == 0]
        if not programs:
            raise (Error if self.ncas else Locked)('%s: no decryptable Program NCA; check the installed keys' % self.path)
        self.title_id = min(nca.title_id for nca in programs)
        programs = [nca for nca in programs if nca.title_id == self.title_id]
        patches = [nca for nca in programs if _is_patch(nca)]
        bases = [nca for nca in programs if nca not in patches]
        if len(patches) > 1 or len(bases) > 1:
            raise Error('%s: the package holds several versions of the game; keep one game and one update' % self.path)
        self.base = bases[0] if bases else None
        self.embedded_update = patches[0] if patches else None
        self.update_path = None
        self.update_ncas = self.ncas                      # where the update's metadata and control data are
        if update:
            if not self.base:
                raise Error('%s: this package is an update on its own; the game\'s package is needed as the source' % self.path)
            self.update_path = os.path.abspath(update)
            self.update_ncas, _ = open_ncas(self.update_path, self.keys)
            theirs = [nca for nca in self.update_ncas if nca.content_type == 0 and _is_patch(nca)]
            mine = [nca for nca in theirs if nca.title_id == self.title_id]
            if not mine:
                if theirs:
                    raise Error('%s: this update is for %016x, not for this game (%016x)'
                                % (self.update_path, theirs[0].title_id, self.title_id))
                raise Error('%s: the package holds no game update' % self.update_path)
            if len(mine) > 1:
                raise Error('%s: the package holds several updates; keep one' % self.update_path)
            self.update = mine[0]
            programs = [self.base, self.update]
        elif embedded:
            self.update = self.embedded_update
        else:
            if not self.base:
                raise Error('%s: this package is an update on its own; the game\'s package is needed as the source' % self.path)
            self.update = None
            programs = [self.base]
        self.program = self.update or self.base           # the code and the metadata come from the update
        self.encrypted = any(section.crypto != 1 for nca in programs for section in nca.sections)
        for nca in programs:
            for section in nca.sections:
                if section.sparse or section.compressed:
                    raise Error('%s: %s storage in the Program NCA is not supported' % (
                        self.path, 'sparse' if section.sparse else 'compressed'))
                if section.crypto not in (1, 3, 4):
                    raise Error('%s: the Program NCA uses an encryption the reader does not support (type %d)' % (
                        self.path, section.crypto))
        for nca in (self.base, self.update):      # the base's too: that proves its key before the game needs it
            if not nca: continue
            code = nca.section(2, 1)
            if not code: raise Error('%s: the Program NCA has no ExeFS' % self.path)
            pfs_at, pfs_size = struct.unpack_from('<QQ', code.fs, 0x40)
            if pfs_at > code.size or pfs_size > code.size - pfs_at:
                raise Error('%s: ExeFS range is out of bounds' % self.path)
            self.exefs_section = code
            self.exefs_nodes = self._exefs(code, pfs_at, pfs_size)
        data = self.program.section(3, 0)
        base_data = self.base.section(3, 0) if self.base else None
        if not data: raise Error('%s: the Program NCA has no RomFS' % self.path)
        if data.patch:
            self.storage = Patched(data, base_data)
        else:
            self.storage = data
        self.romfs_at, self.romfs_size = data.ivfc_data()
        if self.romfs_at > self.storage.size or self.romfs_size > self.storage.size - self.romfs_at:
            raise Error('%s: RomFS range is out of bounds' % self.path)
        self.root = nsp.romfs_tree(self.path, self.romfs_size, lambda offset, size: self.storage.read(self.romfs_at + offset, size))
        self.version, _, wanted = self._metadata(self.update_ncas if self.update else self.ncas, bool(self.update))
        _, self.addons, base_wanted = self._metadata(self.ncas, False)
        self.display_version = None
        self.control = self._control(self.update_ncas if self.update else self.ncas, wanted)
        if not self.control and self.update_path: self.control = self._control(self.ncas, base_wanted)   # an update without control data

    def _exefs(self, section, at, size):
        head = section.read(at, nsp.PFS_HEAD.size)
        magic, count, strings_size, _ = nsp.PFS_HEAD.unpack(head)
        table_size = nsp.PFS_HEAD.size + count * nsp.PFS_ENTRY.size + strings_size
        if magic != b'PFS0' or count > 1000 or table_size > size:
            raise Error('%s: the ExeFS did not decrypt; the installed keys probably lack this game\'s generation (%s)'
                        % (self.path, (section.nca.wrapped or '').split(':')[0]))
        table = section.read(at, table_size)
        strings = table[nsp.PFS_HEAD.size + count * nsp.PFS_ENTRY.size:]
        nodes = []
        for index in range(count):
            relative, length, name_at, _ = nsp.PFS_ENTRY.unpack_from(table, nsp.PFS_HEAD.size + index * nsp.PFS_ENTRY.size)
            end = strings.find(b'\0', name_at)
            if name_at >= len(strings) or end < 0 or relative > size - table_size or length > size - table_size - relative:
                raise Error('%s: ExeFS entry is out of range' % self.path)
            nodes.append(SimpleNamespace(name=strings[name_at:end].decode('utf-8', 'replace'),
                                         offset=at + table_size + relative, size=length))
        return nodes

    def _files(self, section):
        """{name: bytes} of a small PartitionFS section (content metadata)"""
        at, size = struct.unpack_from('<QQ', section.fs, 0x40)
        if size > 16 << 20 or at > section.size or size > section.size - at: return {}
        image = section.read(at, size)
        magic, count, strings_size, _ = nsp.PFS_HEAD.unpack_from(image, 0)
        data_at = nsp.PFS_HEAD.size + count * nsp.PFS_ENTRY.size + strings_size
        if magic != b'PFS0' or count > 1000 or data_at > size: return {}
        strings = image[data_at - strings_size:data_at]; files = {}
        for index in range(count):
            relative, length, name_at, _ = nsp.PFS_ENTRY.unpack_from(image, nsp.PFS_HEAD.size + index * nsp.PFS_ENTRY.size)
            end = strings.find(b'\0', name_at)
            if end < 0 or data_at + relative + length > size: return {}
            files[strings[name_at:end].decode('utf-8', 'replace')] = image[data_at + relative:data_at + relative + length]
        return files

    def _metadata(self, ncas, updated):
        """(title version, add-on content title ids, name of the Control NCA to use) from the content metadata of
        these NCAs: the update's when it is applied, else the game's"""
        import dlc
        version = 0; addons = []; control = {}
        for nca in ncas:
            if nca.content_type != 1 or not nca.sections: continue
            try:
                files = self._files(nca.sections[0])
                for name in sorted(files):
                    if not name.lower().endswith('.cnmt'): continue
                    cnmt = dlc.parse_cnmt(files[name])
                    if cnmt.type == 0x82 and dlc.is_addon_of(cnmt.title_id, self.title_id): addons.append(cnmt.title_id)
                    if cnmt.type != (0x81 if updated else 0x80) or dlc.base_of(cnmt.title_id) != dlc.base_of(self.title_id): continue
                    version = max(version, cnmt.version)
                    for record in cnmt.records:
                        if record.type == 3: control[cnmt.type] = record.nca_id + '.nca'
            except (Error, struct.error):
                continue
        return version, sorted(addons), control.get(0x81 if updated else 0x80)

    def _control(self, ncas, wanted):
        """{file name: bytes} of the Control NCA: control.nacp and the icons. `wanted` is the one the content metadata
        names; when it is not among these NCAs the first Control NCA of the title serves"""
        found = None
        for nca in ncas:
            if nca.content_type != 2 or dlc_base(nca.title_id) != dlc_base(self.title_id): continue
            section = nca.section(3, 0)
            if not section or section.patch or section.sparse or section.compressed: continue
            try:
                at, size = section.ivfc_data()
                if size > 64 << 20 or at > section.size or size > section.size - at: continue
                image = section.read(at, size)
                root = nsp.romfs_tree(self.path, size, lambda offset, length: image[offset:offset + length])
                files = {kid.name: image[kid.offset:kid.offset + kid.size] for kid in root.kids if kid.is_file}
            except Error:
                continue
            if len(files.get('control.nacp', b'')) < 0x3070: continue
            if found is None or nca.name.lower() == (wanted or '').lower(): found = files
        if not found: return {}
        self.display_version = found['control.nacp'][0x3060:0x3070].split(b'\0')[0].decode('utf-8', 'replace')
        return found

    def exefs(self):
        return [(node.name, node.size, functools.partial(self.exefs_section.read, node.offset, node.size))
                for node in self.exefs_nodes]

    def read(self, node):
        if not node.is_file: raise IsADirectoryError(node.name)
        return self.storage.read(self.romfs_at + node.offset, node.size)

    def marker(self):
        """romfs.nsp, version 2: where the runtime finds the RomFS image and how it decrypts it. Keys are named, and
        content keys stay wrapped as they are in the package: the runtime unwraps them with the user's prod.keys."""
        lines = ['SWRNSP2', self.path, 'keys ' + self.keys_path,
                 'romfs %x %x' % (self.romfs_at, self.romfs_size)]
        if isinstance(self.storage, Patched):
            if self.storage.base: lines.append('base ' + self.storage.base.layer())
            # an update of its own package: the patch row's offsets are in that file
            if self.update_path: lines.append('update ' + self.update_path)
            lines.append('patch ' + self.storage.patch.layer() + ' %x %x %x %x' % self.storage.tables)
        else:
            lines.append('base ' + self.storage.layer())
        return '\n'.join(lines) + '\n'


def update_info(path, keys=None, game=None):
    """what an update package is: {'title_id', 'version', 'display'}. With `game` (the game's own package) the update
    is opened over it, which proves that it belongs to that game and that the keys read it."""
    path = os.path.abspath(path)
    if not os.path.isfile(path): raise Error('%s: the update package is missing' % path)
    if game:
        if not (os.path.isfile(game) and nsp.is_nsp(game)):
            raise Error('%s: a separate update needs the game as an NSP package; this game is built from %s'
                        % (path, 'a NUP package, which holds its update already' if str(game).lower().endswith('.nup')
                           else 'an extracted folder or another kind of file'))
        package = Package(game, keys, update=path)
        return {'title_id': package.title_id, 'version': package.version, 'display': package.display_version or ''}
    key_file = Keys(keys)
    ncas, _ = open_ncas(path, key_file)
    patches = [nca for nca in ncas if nca.content_type == 0 and _is_patch(nca)]
    if not patches: raise Error('%s: the package holds no game update' % path)
    probe = Package.__new__(Package); probe.path = path; probe.title_id = patches[0].title_id
    version, _, wanted = probe._metadata(ncas, True)
    probe.display_version = None; probe._control(ncas, wanted)
    return {'title_id': probe.title_id, 'version': version, 'display': probe.display_version or ''}


class PackageRomfs:
    def __init__(self, package):
        self.nsp = package
        self.path = package.path
        self.root = package.root

    def read(self, node): return self.nsp.read(node)

    walk_files = nsp.NspRomfs.walk_files
