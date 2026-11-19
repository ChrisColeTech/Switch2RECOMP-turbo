#!/usr/bin/env python3
# Copyright 2026 ChrisColeTech
# SPDX-License-Identifier: MIT
"""Validated reader for Switch 2 NUP packages.

NUP stores ExeFS and RomFS in one indexed file. Raw entries are read from their
package ranges and codec-1 entries are one whole-file zstd frame.
"""
import functools
import io
import os
import struct
from types import SimpleNamespace


NUP_MAGIC = b'NUP\x01'
TOC_MAGIC = b'TOC\x01'
TOC_MAX = 256 << 20
READ_SIZE = 8 << 20
TOC_HEAD = struct.Struct('<4sIQQQQ')
TITLE = struct.Struct('<QIIII')
DIR = struct.Struct('<IIII8x')
FILE = struct.Struct('<IIQQQ32sB15x')


class Error(ValueError):
    pass


class _LimitedReader(io.RawIOBase):
    def __init__(self, fileobj, size):
        self.fileobj = fileobj
        self.left = size

    def readable(self):
        return True

    def readinto(self, buffer):
        if not self.left:
            return 0
        size = min(len(buffer), self.left)
        data = self.fileobj.read(size)
        buffer[:len(data)] = data
        self.left -= len(data)
        return len(data)


def is_nup(path):
    try:
        with open(path, 'rb') as fileobj:
            return fileobj.read(4) == NUP_MAGIC
    except (OSError, TypeError):
        return False


def _stream_decompress(nup, node):
    try:
        from backports import zstd
    except ImportError:
        try:
            from compression import zstd
        except ImportError:
            zstd = None

    if zstd is not None:
        limit = getattr(getattr(zstd, 'DecompressionParameter', None), 'window_log_max', None)
        decoder = (zstd.ZstdDecompressor(options={limit: 31}) if limit is not None
                   else zstd.ZstdDecompressor(max_window_size=1 << 31))
        output = bytearray()
        with open(nup.path, 'rb') as fileobj:
            fileobj.seek(node.offset)
            left = node.stored
            while left:
                chunk = fileobj.read(min(READ_SIZE, left))
                if not chunk:
                    break
                left -= len(chunk)
                pending = chunk
                while pending or not decoder.needs_input:
                    limit = min(READ_SIZE, node.size + 1 - len(output))
                    if limit <= 0:
                        raise Error('%s: %s expands past its declared size' % (nup.path, node.name))
                    before = len(output)
                    output += decoder.decompress(pending, max_length=limit)
                    pending = b''
                    if len(output) > node.size:
                        raise Error('%s: %s expands past its declared size' % (nup.path, node.name))
                    if decoder.eof:
                        if left or decoder.unused_data:
                            raise Error('%s: %s has data after its zstd frame' % (nup.path, node.name))
                        break
                    if decoder.needs_input:
                        break
                    if len(output) == before:
                        raise Error('%s: %s has invalid zstd data' % (nup.path, node.name))
            while not decoder.eof:
                limit = min(READ_SIZE, node.size + 1 - len(output))
                if limit <= 0:
                    raise Error('%s: %s expands past its declared size' % (nup.path, node.name))
                chunk = decoder.decompress(b'', max_length=limit)
                if not chunk and decoder.needs_input:
                    break
                output += chunk
                if len(output) > node.size:
                    raise Error('%s: %s expands past its declared size' % (nup.path, node.name))
        data = bytes(output)
    else:
        try:
            import zstandard
        except ImportError:
            raise Error('reading compressed NUP entries needs Python 3.14 or: pip install zstandard')
        with open(nup.path, 'rb') as fileobj:
            fileobj.seek(node.offset)
            source = io.BufferedReader(_LimitedReader(fileobj, node.stored), READ_SIZE)
            with zstandard.ZstdDecompressor(max_window_size=1 << 31).stream_reader(source) as reader:
                output = bytearray()
                while len(output) <= node.size:
                    chunk = reader.read(min(READ_SIZE, node.size + 1 - len(output)))
                    if not chunk:
                        break
                    output += chunk
                if len(output) > node.size:
                    raise Error('%s: %s expands past its declared size' % (nup.path, node.name))
            data = bytes(output)

    if len(data) != node.size:
        raise Error('%s: %s decompresses to %d of %d bytes' %
                    (nup.path, node.name, len(data), node.size))
    return data


def read_whole(nup, node):
    if node.codec:
        return _stream_decompress(nup, node)
    with open(nup.path, 'rb') as fileobj:
        fileobj.seek(node.offset)
        data = fileobj.read(node.size)
    if len(data) != node.size:
        raise Error('%s: %s is cut short' % (nup.path, node.name))
    return data


class Nup:
    def __init__(self, path):
        self.path = os.path.abspath(path)
        stat = os.stat(self.path)
        self.package_size = stat.st_size
        with open(self.path, 'rb') as fileobj:
            head = fileobj.read(12)
            if len(head) < 12 or head[:4] != NUP_MAGIC:
                raise Error('%s: no NUP magic' % path)
            toc_at, = struct.unpack_from('<I', head, 8)
            fileobj.seek(toc_at)
            toc = fileobj.read(TOC_HEAD.size)
            if len(toc) != TOC_HEAD.size or toc[:4] != TOC_MAGIC:
                raise Error('%s: no NUP table of contents' % path)
            _, nt, nd, nf, pool_at, pool_size = TOC_HEAD.unpack(toc)
            table_size = pool_at + pool_size
            if (not nt or nt > 4096 or not nd or nd > 0xffffffff or nf > 0xffffffff
                    or pool_at > TOC_MAX or pool_size > TOC_MAX or table_size > TOC_MAX
                    or toc_at > stat.st_size or table_size > stat.st_size - toc_at):
                raise Error('%s: table of contents is out of range' % path)
            fileobj.seek(toc_at)
            table = fileobj.read(table_size)
        if len(table) != table_size:
            raise Error('%s: table of contents is unreadable' % path)

        self.table = table
        self.nd = nd
        self.nf = nf
        self.dirs_at = 0x30 + nt * 0x40
        self.files_at = self.dirs_at + nd * DIR.size
        self.children_at = self.files_at + nf * FILE.size
        self.pool_at = pool_at
        if self.children_at > self.pool_at:
            raise Error('%s: overlapping NUP tables' % path)
        titles = [TITLE.unpack_from(table, 0x30 + index * 0x40) for index in range(nt)]
        selected = [(index, title) for index, title in enumerate(titles)
                    if title[2] < nd and title[3] < nd]
        if not selected:
            raise Error('%s: no title has both ExeFS and RomFS' % path)
        self.title_index, self.title = selected[-1]
        self.root = self._tree(self.title[3], set(), set(), 0)
        self.exefs_root = self._tree(self.title[2], set(), set(), 0)

    def _name(self, offset, length):
        if not length or offset > len(self.table) - self.pool_at or length > len(self.table) - self.pool_at - offset:
            raise Error('%s: bad name in the table of contents' % self.path)
        raw = self.table[self.pool_at + offset:self.pool_at + offset + length]
        if b'/' in raw or b'\0' in raw or raw in (b'.', b'..'):
            raise Error('%s: bad name in the table of contents' % self.path)
        try:
            return raw.decode('utf-8')
        except UnicodeDecodeError:
            raise Error('%s: non-UTF-8 name in the table of contents' % self.path)

    def _tree(self, directory, seen_dirs, seen_files, depth):
        if directory >= self.nd or directory in seen_dirs or depth > 64:
            raise Error('%s: bad directory tree' % self.path)
        seen_dirs.add(directory)
        at = self.dirs_at + directory * DIR.size
        first, count = DIR.unpack_from(self.table, at)[2:4]
        child_count = (self.pool_at - self.children_at) // 4
        if first > child_count or count > child_count - first:
            raise Error('%s: child list is out of range' % self.path)
        node = SimpleNamespace(name='', is_file=False, kids=[], source=directory)
        kids = []
        for index in range(first, first + count):
            source, = struct.unpack_from('<I', self.table, self.children_at + index * 4)
            is_file = source >= self.nd
            item = source - self.nd if is_file else source
            if (is_file and item >= self.nf) or (not is_file and item >= self.nd):
                raise Error('%s: child index is out of range' % self.path)
            row_at = self.files_at + item * FILE.size if is_file else self.dirs_at + item * DIR.size
            name_offset, name_length = struct.unpack_from('<II', self.table, row_at)
            name = self._name(name_offset, name_length)
            if is_file:
                if item in seen_files:
                    raise Error('%s: file appears more than once in a tree' % self.path)
                seen_files.add(item)
                _, _, size, offset, stored, _, codec = FILE.unpack_from(self.table, row_at)
                if codec > 1 or (not codec and stored != size):
                    raise Error('%s: invalid codec or stored size for %s' % (self.path, name))
                if offset > self.package_size or stored > self.package_size - offset:
                    raise Error('%s: %s extends past the package' % (self.path, name))
                child = SimpleNamespace(name=name, is_file=True, kids=(), source=item,
                                        size=size, offset=offset, stored=stored, codec=codec)
            else:
                child = self._tree(item, seen_dirs, seen_files, depth + 1)
                child.name = name
            kids.append(child)
        kids.sort(key=lambda child: child.name.encode('utf-8'))
        if any(kids[index - 1].name == kids[index].name for index in range(1, len(kids))):
            raise Error('%s: duplicate directory entry' % self.path)
        node.kids = kids
        return node

    @staticmethod
    def find_from(root, relative):
        node = root
        for part in relative.replace('\\', '/').split('/'):
            if not part or part == '.':
                continue
            if part == '..' or node.is_file:
                raise FileNotFoundError(relative)
            lo, hi = 0, len(node.kids)
            target = part.encode('utf-8')
            while lo < hi:
                middle = (lo + hi) // 2
                if node.kids[middle].name.encode('utf-8') < target:
                    lo = middle + 1
                else:
                    hi = middle
            if lo == len(node.kids) or node.kids[lo].name != part:
                raise FileNotFoundError(relative)
            node = node.kids[lo]
        return node

    def find(self, relative):
        return self.find_from(self.root, relative)

    def read(self, node):
        if not node.is_file:
            raise IsADirectoryError(node.name)
        return read_whole(self, node)

    def exefs(self):
        return [(node.name, node.size, functools.partial(read_whole, self, node))
                for node in self.exefs_root.kids if node.is_file]


class NupRomfs:
    def __init__(self, package):
        self.nup = package
        self.path = package.path
        self.root = package.root

    def read(self, node):
        return self.nup.read(node)

    def walk_files(self):
        stack = [('', self.root)]
        while stack:
            prefix, node = stack.pop()
            for child in reversed(node.kids):
                relative = (prefix + '/' + child.name).lstrip('/')
                if child.is_file:
                    yield relative, child
                else:
                    stack.append((relative, child))


@functools.lru_cache(maxsize=8)
def _load(path, identity):
    return Nup(path)


def load(path):
    stat = os.stat(path)
    identity = (stat.st_ino, stat.st_size, stat.st_mtime_ns, stat.st_ctime_ns)
    return _load(os.path.abspath(os.fsdecode(path)), identity)
