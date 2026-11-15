#!/usr/bin/env python3
# SwitchRecomp - Copyright 2026 MZ Labs
# Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
# Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen.
"""Read standard application NSPs in place without copying their RomFS.

The NCA header is inspected with hactool and the supported Switch 2 dumps keep their
ExeFS and RomFS sections plaintext. Encrypted sections and BKTR updates are rejected.
"""
import functools
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from types import SimpleNamespace

ROOT = os.path.dirname(os.path.abspath(__file__))
PFS_HEAD = struct.Struct('<4sIII')
PFS_ENTRY = struct.Struct('<QQII')
ROMFS_HEAD = struct.Struct('<10Q')
MAX_TABLE = 256 << 20
EMPTY = 0xffffffff


class Error(ValueError):
    pass


def is_nsp(path):
    try:
        with open(path, 'rb') as fileobj:
            return fileobj.read(4) == b'PFS0'
    except (OSError, TypeError):
        return False


def _read(path, offset, size):
    with open(path, 'rb') as fileobj:
        fileobj.seek(offset)
        data = fileobj.read(size)
    if len(data) != size:
        raise Error('%s: data at %#x is cut short' % (path, offset))
    return data


def _pfs(path, offset, size):
    head = _read(path, offset, PFS_HEAD.size)
    magic, count, strings_size, _ = PFS_HEAD.unpack(head)
    table_size = PFS_HEAD.size + count * PFS_ENTRY.size + strings_size
    if magic != b'PFS0' or count > 100000 or strings_size > MAX_TABLE or table_size > size:
        raise Error('%s: invalid PFS0 at %#x' % (path, offset))
    table = _read(path, offset, table_size)
    strings_at = PFS_HEAD.size + count * PFS_ENTRY.size
    strings = table[strings_at:]
    data_at = offset + table_size
    result = []
    names = set()
    for index in range(count):
        relative, length, name_at, _ = PFS_ENTRY.unpack_from(table, PFS_HEAD.size + index * PFS_ENTRY.size)
        if name_at >= len(strings):
            raise Error('%s: PFS0 name is out of range' % path)
        end = strings.find(b'\0', name_at)
        if end < 0:
            raise Error('%s: unterminated PFS0 name' % path)
        try:
            name = strings[name_at:end].decode('utf-8')
        except UnicodeDecodeError:
            raise Error('%s: non-UTF-8 PFS0 name' % path)
        if not name or '/' in name or '\\' in name or name in names or relative > size - table_size or length > size - table_size - relative:
            raise Error('%s: unsafe PFS0 entry %r' % (path, name))
        names.add(name)
        result.append(SimpleNamespace(name=name, offset=data_at + relative, size=length))
    return result


def _keys_path(explicit=None):
    path = explicit or os.environ.get('SWR_PROD_KEYS') or os.path.join(ROOT, 'keys', 'prod.keys')
    if not os.path.isfile(path):
        raise Error('Nintendo keys are not installed. In the launcher use Keys > Install keys, or set SWR_PROD_KEYS.')
    return os.path.abspath(path)


def _hactool_path(explicit=None):
    choices = [explicit, os.environ.get('SWR_HACTOOL'),
               os.path.join(ROOT, 'tools', 'hactool', 'hactool.exe'),
               os.path.join(ROOT, 'tools', 'hactool', 'hactool')]
    for choice in choices:
        if choice and os.path.isfile(choice): return os.path.abspath(choice)
    found = shutil.which('hactool')
    if found: return found
    raise Error('hactool is missing. Run the Switch2RECOMP setup script again.')


class Nsp:
    def __init__(self, path, keys=None, hactool=None):
        self.path = os.path.abspath(path)
        self.keys_path = _keys_path(keys)
        self.hactool = _hactool_path(hactool)
        package_size = os.path.getsize(self.path)
        members = _pfs(self.path, 0, package_size)
        programs = []
        with tempfile.TemporaryDirectory(prefix='swr_nsp_') as work:
            for member in members:
                if not member.name.lower().endswith('.nca') or member.size < 0xc00:
                    continue
                sparse = os.path.join(work, member.name)
                header_path = sparse + '.header'
                with open(sparse, 'wb') as output:
                    output.write(_read(self.path, member.offset, 0xc00))
                    output.truncate(member.size)
                command = ([sys.executable, self.hactool] if self.hactool.lower().endswith('.py') else [self.hactool])
                result = subprocess.run(command + ['-i', '-t', 'nca', '-k', self.keys_path,
                                         '--header=' + header_path, sparse],
                                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                        timeout=30)
                if result.returncode or not os.path.isfile(header_path):
                    continue
                header = _read(header_path, 0, 0xc00)
                if header[0x200:0x204] == b'NCA3' and header[0x205] == 0:
                    programs.append((member, header))
        if not programs:
            raise Error('%s: no decryptable Program NCA; check the installed keys' % self.path)
        if len(programs) != 1:
            raise Error('%s: multiple Program NCAs are not supported yet' % self.path)
        self.member, header = programs[0]
        self.title_id, = struct.unpack_from('<Q', header, 0x210)
        exefs = romfs = None
        for index in range(4):
            start, end = struct.unpack_from('<II', header, 0x240 + index * 0x10)
            if not start and not end:
                continue
            section_at, section_end = start * 0x200, end * 0x200
            if section_at > self.member.size or section_end < section_at or section_end > self.member.size:
                raise Error('%s: NCA section is out of range' % self.path)
            fs = header[0x400 + index * 0x200:0x600 + index * 0x200]
            partition, fs_type, crypto = fs[2], fs[3], fs[4]
            if crypto == 4 or fs[0x100:0x104] == b'BKTR' or fs[0x120:0x124] == b'BKTR':
                raise Error('%s: this is an update-only BKTR NSP; a matching base NSP is required' % self.path)
            if crypto != 1:
                raise Error('%s: encrypted NCA sections are not supported by the in-place reader' % self.path)
            absolute = self.member.offset + section_at
            section_size = section_end - section_at
            if partition == 1 and fs_type == 2:
                pfs_at, pfs_size = struct.unpack_from('<QQ', fs, 0x40)
                if pfs_at > section_size or pfs_size > section_size - pfs_at:
                    raise Error('%s: ExeFS range is out of bounds' % self.path)
                exefs = _pfs(self.path, absolute + pfs_at, pfs_size)
            elif partition == 0 and fs_type == 3:
                if fs[8:12] != b'IVFC':
                    raise Error('%s: RomFS has no IVFC header' % self.path)
                level_at = 0x18 + 5 * 0x18
                data_at, data_size = struct.unpack_from('<QQ', fs, level_at)
                if data_at > section_size or data_size > section_size - data_at:
                    raise Error('%s: RomFS range is out of bounds' % self.path)
                romfs = (absolute + data_at, data_size)
        if not exefs or not romfs:
            raise Error('%s: Program NCA has no supported ExeFS and RomFS pair' % self.path)
        self.exefs_nodes = exefs
        self.romfs_offset, self.romfs_size = romfs
        self.root = self._romfs_tree()

    def _romfs_tree(self):
        values = ROMFS_HEAD.unpack(_read(self.path, self.romfs_offset, ROMFS_HEAD.size))
        if values[0] != 0x50:
            raise Error('%s: invalid RomFS header' % self.path)
        _, _, _, dirs_at, dirs_size, _, _, files_at, files_size, data_at = values
        for at, size in ((dirs_at, dirs_size), (files_at, files_size)):
            if size > MAX_TABLE or at > self.romfs_size or size > self.romfs_size - at:
                raise Error('%s: RomFS metadata is out of range' % self.path)
        if data_at > self.romfs_size:
            raise Error('%s: RomFS data offset is out of range' % self.path)
        dirs = _read(self.path, self.romfs_offset + dirs_at, dirs_size)
        files = _read(self.path, self.romfs_offset + files_at, files_size)
        seen_dirs, seen_files = set(), set()

        def name(table, at, fixed):
            if at > len(table) - fixed:
                raise Error('%s: RomFS entry is out of range' % self.path)
            length, = struct.unpack_from('<I', table, at + fixed - 4)
            if length > 0x1000 or length > len(table) - at - fixed:
                raise Error('%s: RomFS name is out of range' % self.path)
            try:
                return table[at + fixed:at + fixed + length].decode('utf-8')
            except UnicodeDecodeError:
                raise Error('%s: non-UTF-8 RomFS name' % self.path)

        def directory(at, depth=0):
            if at in seen_dirs or depth > 128:
                raise Error('%s: cyclic RomFS directory tree' % self.path)
            seen_dirs.add(at)
            if at > len(dirs) - 0x18:
                raise Error('%s: RomFS directory is out of range' % self.path)
            _, sibling, child, first_file = struct.unpack_from('<4I', dirs, at)
            node = SimpleNamespace(name=name(dirs, at, 0x18), is_file=False, kids=[])
            current = child
            while current != EMPTY:
                child_node = directory(current, depth + 1)
                node.kids.append(child_node)
                _, current = struct.unpack_from('<II', dirs, current)
            current = first_file
            while current != EMPTY:
                if current in seen_files or current > len(files) - 0x20:
                    raise Error('%s: cyclic or invalid RomFS file list' % self.path)
                seen_files.add(current)
                _, sibling, relative, size = struct.unpack_from('<IIQQ', files, current)
                if relative > self.romfs_size - data_at or size > self.romfs_size - data_at - relative:
                    raise Error('%s: RomFS file is out of range' % self.path)
                node.kids.append(SimpleNamespace(name=name(files, current, 0x20), is_file=True,
                                                  kids=(), offset=self.romfs_offset + data_at + relative,
                                                  size=size))
                current = sibling
            node.kids.sort(key=lambda item: item.name.encode('utf-8'))
            if any(node.kids[i - 1].name == node.kids[i].name for i in range(1, len(node.kids))):
                raise Error('%s: duplicate RomFS name' % self.path)
            return node
        return directory(0)

    def exefs(self):
        return [(node.name, node.size, functools.partial(_read, self.path, node.offset, node.size))
                for node in self.exefs_nodes]

    def read(self, node):
        if not node.is_file: raise IsADirectoryError(node.name)
        return _read(self.path, node.offset, node.size)

    def marker(self):
        return 'SWRNSP1\n%s\n%x\n%x\n' % (self.path, self.romfs_offset, self.romfs_size)


class NspRomfs:
    def __init__(self, package):
        self.nsp = package
        self.path = package.path
        self.root = package.root

    def read(self, node): return self.nsp.read(node)

    def walk_files(self):
        stack = [('', self.root)]
        while stack:
            prefix, node = stack.pop()
            for child in reversed(node.kids):
                relative = (prefix + '/' + child.name).lstrip('/')
                if child.is_file: yield relative, child
                else: stack.append((relative, child))


@functools.lru_cache(maxsize=4)
def _load(path, identity, keys_path, keys_identity, hactool):
    return Nsp(path, keys_path, hactool)


def load(path, keys=None, hactool=None):
    stat = os.stat(path); keys_path = _keys_path(keys); key_stat = os.stat(keys_path)
    identity = (stat.st_ino, stat.st_size, stat.st_mtime_ns, stat.st_ctime_ns)
    keys_identity = (key_stat.st_size, key_stat.st_mtime_ns)
    return _load(os.path.abspath(os.fsdecode(path)), identity, keys_path, keys_identity,
                 _hactool_path(hactool))
