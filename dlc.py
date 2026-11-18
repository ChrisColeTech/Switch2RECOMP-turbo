#!/usr/bin/env python3
# SwitchRecomp - Copyright 2026 MZ Labs
# Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
# Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen.
"""Add-on content (DLC) for a built game.

A game sees DLC as two things: the aoc:u service says which add-on titles are installed, and each title has its own
RomFS that the game mounts (nn::fs::MountAddOnContent). It is never layered over the game's RomFS. Installing a
package therefore writes, into the game's data folder:

    dlc.manifest                  one line a title: <title id> <version> <data|stub> <package>
    dlc/<title id>/romfs.nsp      a marker for a plaintext RomFS, read in place from the package, or
    dlc/<title id>/romfs.bin      the RomFS image, decrypted or decompressed once here
    dlc/<title id>/install.json   what was installed from where (to skip the work next time)

Packages: .nsp and .nsz (PFS0, also one that holds the game and its update too), .xci and .xcz (the secure
partition) and a bare .nca. The package is only read. NCA headers, content metadata and encrypted sections go
through hactool with the user's keys, like nsp.py; .ncz members are unpacked here (their data is stored decrypted).
"""
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from types import SimpleNamespace

import nsp

Error = nsp.Error
EXTENSIONS = ('.nsp', '.nsz', '.xci', '.xcz', '.nca')
HFS_HEAD = struct.Struct('<4sIII')
HFS_ENTRY = struct.Struct('<QQII8x32x')
CHUNK = 4 << 20
TYPE_NAMES = {0x80: 'the game', 0x81: 'a game update', 0x82: 'add-on content', 0x83: 'an update delta'}


def base_of(title_id):
    return title_id & ~0x1fff


def is_addon_of(title_id, application):
    """0100152000022000 owns 0100152000023000..37ff"""
    return base_of(title_id) == base_of(application) and bool(title_id & 0x1000)


# ------------------------------------------------------------------ containers
def _hfs(path, offset, limit):
    head = nsp._read(path, offset, HFS_HEAD.size)
    magic, count, strings_size, _ = HFS_HEAD.unpack(head)
    table_size = HFS_HEAD.size + count * HFS_ENTRY.size + strings_size
    if magic != b'HFS0' or count > 100000 or strings_size > nsp.MAX_TABLE or table_size > limit:
        raise Error('%s: invalid HFS0 at %#x' % (path, offset))
    table = nsp._read(path, offset, table_size)
    strings = table[HFS_HEAD.size + count * HFS_ENTRY.size:]
    result = []
    for index in range(count):
        relative, length, name_at, _ = HFS_ENTRY.unpack_from(table, HFS_HEAD.size + index * HFS_ENTRY.size)
        end = strings.find(b'\0', name_at)
        if name_at >= len(strings) or end < 0 or relative > limit - table_size or length > limit - table_size - relative:
            raise Error('%s: HFS0 entry is out of range' % path)
        name = strings[name_at:end].decode('utf-8', 'replace')
        result.append(SimpleNamespace(name=name, offset=offset + table_size + relative, size=length))
    return result


def members(path):
    """the files of a package: (name, offset, size) inside it. A bare NCA is its own only member"""
    size = os.path.getsize(path)
    head = nsp._read(path, 0, min(size, 0x1200)) if size >= 0x10 else b''
    extension = os.path.splitext(path)[1].lower()
    if extension not in EXTENSIONS:
        raise Error('%s: not a package (%s)' % (path, ' '.join(EXTENSIONS)))
    if extension == '.nca':
        return [SimpleNamespace(name=os.path.basename(path), offset=0, size=size)]
    if head[:4] == b'PFS0':
        return nsp._pfs(path, 0, size)
    for card in (0, 0x1000):      # a card image, without or with the 0x1000 key area in front
        if head[card + 0x100:card + 0x104] == b'HEAD':
            root_at, = struct.unpack_from('<Q', head, card + 0x130)
            for part in _hfs(path, card + root_at, size - card - root_at):
                if part.name == 'secure':
                    return _hfs(path, part.offset, part.size)
            raise Error('%s: the card image has no secure partition' % path)
    raise Error('%s: neither a PFS0 package (NSP, NSZ) nor a card image (XCI, XCZ)' % path)


# ------------------------------------------------------------------ hactool
class Tools:
    def __init__(self, keys=None, hactool=None):
        self.keys = nsp._keys_path(keys)
        self.hactool = nsp._hactool_path(hactool)

    def run(self, arguments, cwd=None):
        command = [sys.executable, self.hactool] if self.hactool.lower().endswith('.py') else [self.hactool]
        try:
            return subprocess.run(command + ['-k', self.keys] + arguments, cwd=cwd, stdout=subprocess.DEVNULL,
                                  stderr=subprocess.DEVNULL, timeout=3600).returncode
        except (OSError, subprocess.TimeoutExpired):
            return -1


def nca_header(tools, path, member, work):
    """the decrypted 0xc00-byte header of an NCA (or of the NCA an .ncz holds), None when the keys don't open it"""
    sparse = os.path.join(work, 'header.nca')
    with open(sparse, 'wb') as output:
        output.write(nsp._read(path, member.offset, 0xc00))
        output.truncate(max(member.size, 0xc00))
    out = sparse + '.header'
    if os.path.exists(out): os.remove(out)
    tools.run(['-i', '-t', 'nca', '--header=' + out, sparse])
    header = None
    if os.path.isfile(out) and os.path.getsize(out) >= 0xc00:
        header = nsp._read(out, 0, 0xc00)
        if header[0x200:0x204] not in (b'NCA3', b'NCA2'): header = None
    for name in (sparse, out):
        if os.path.exists(name): os.remove(name)
    return header


def parse_cnmt(data):
    if len(data) < 0x20:
        raise Error('content metadata is cut short')
    title_id, version, kind, _, table, contents, _ = struct.unpack_from('<QIBBHHH', data, 0)
    records = []
    for index in range(contents):
        at = 0x20 + table + index * 0x38
        if at + 0x38 > len(data):
            raise Error('content metadata record is out of range')
        size = int.from_bytes(data[at + 0x30:at + 0x36], 'little')
        records.append(SimpleNamespace(nca_id=data[at + 0x20:at + 0x30].hex(), size=size, type=data[at + 0x36],
                                       id_offset=data[at + 0x37]))
    return SimpleNamespace(title_id=title_id, version=version, type=kind, records=records)


def read_cnmt(tools, path, member, work):
    """the content metadata inside a .cnmt.nca, None when the keys don't open it"""
    meta = os.path.join(work, 'meta.nca'); out = os.path.join(work, 'meta')
    shutil.rmtree(out, ignore_errors=True)
    with open(meta, 'wb') as output:
        output.write(nsp._read(path, member.offset, member.size))
    tools.run(['-t', 'nca', '--section0dir=' + out, meta])
    os.remove(meta)
    found = None
    if os.path.isdir(out):
        for name in sorted(os.listdir(out)):
            if name.lower().endswith('.cnmt'):
                with open(os.path.join(out, name), 'rb') as fileobj:
                    found = parse_cnmt(fileobj.read())
                break
        shutil.rmtree(out, ignore_errors=True)
    return found


def ticket_key(data):
    """(rights id hex, encrypted title key hex or None when it is bound to one console)"""
    sig, = struct.unpack_from('<I', data, 0)
    body = {0x10000: 0x240, 0x10003: 0x240, 0x10001: 0x140, 0x10004: 0x140, 0x10002: 0x80, 0x10005: 0x80}.get(sig)
    if body is None or len(data) < body + 0x170:
        return None, None
    personalized = data[body + 0x141] != 0
    return data[body + 0x160:body + 0x170].hex(), None if personalized else data[body + 0x40:body + 0x50].hex()


# ------------------------------------------------------------------ what a package holds
class Title:
    """one add-on content title of a package: its id, version and the member that holds its data"""
    def __init__(self, path, title_id, version, member, files):
        self.path, self.title_id, self.version, self.member, self.files = path, title_id, version, member, files


def scan(path, tools=None, keys=None, hactool=None):
    """(add-on content titles, other title types found) of a package"""
    path = os.path.abspath(path)
    if not os.path.isfile(path):
        raise Error('%s: the package is missing' % path)
    tools = tools or Tools(keys, hactool)
    files = members(path)
    by_name = {member.name.lower(): member for member in files}
    titles, others, locked = [], [], 0
    with tempfile.TemporaryDirectory(prefix='swr_dlc_') as work:
        if os.path.splitext(path)[1].lower() == '.nca':
            header = nca_header(tools, path, files[0], work)
            if header is None:
                raise Error('%s: the installed keys do not open this NCA' % path)
            title_id, = struct.unpack_from('<Q', header, 0x210)
            if header[0x205] not in (4, 5) or not title_id & 0x1000:
                raise Error('%s: this NCA is not add-on content data (content type %d, title %016x)'
                            % (path, header[0x205], title_id))
            return [Title(path, title_id, 0, files[0], files)], []
        for member in files:
            if not member.name.lower().endswith('.cnmt.nca') or member.size > 16 << 20:
                continue
            cnmt = read_cnmt(tools, path, member, work)
            if cnmt is None:
                locked += 1; continue
            if cnmt.type != 0x82:
                others.append(cnmt); continue
            data = [record for record in cnmt.records if record.type == 2]
            holder = None
            for record in data:
                holder = by_name.get(record.nca_id + '.nca') or by_name.get(record.nca_id + '.ncz')
                if holder: break
            if not holder:
                raise Error('%s: add-on content %016x lists data that is not in the package' % (path, cnmt.title_id))
            titles.append(Title(path, cnmt.title_id, cnmt.version, holder, files))
    if not titles and locked:
        raise Error('%s: the installed keys do not open its content metadata (newer keys are needed)' % path)
    titles.sort(key=lambda title: title.title_id)
    return titles, others


# ------------------------------------------------------------------ the title's RomFS
def _romfs_range(header, member_size, path):
    """(offset in the NCA, size, section encryption) of the IVFC RomFS data level"""
    for index in range(4):
        start, end = struct.unpack_from('<II', header, 0x240 + index * 0x10)
        if not start and not end: continue
        fs = header[0x400 + index * 0x200:0x600 + index * 0x200]
        if fs[3] != 3 or fs[8:12] != b'IVFC': continue
        data_at, data_size = struct.unpack_from('<QQ', fs, 0x18 + 5 * 0x18)
        section_at, section_size = start * 0x200, (end - start) * 0x200
        if end < start or data_at > section_size or data_size > section_size - data_at or data_size < 0x50:
            raise Error('%s: the RomFS range of the add-on content is out of bounds' % path)
        return section_at + data_at, data_size, fs[4]
    raise Error('%s: the add-on content has no RomFS' % path)


def _romfs_ok(image):
    try:
        size = os.path.getsize(image)
        with open(image, 'rb') as fileobj:
            head = fileobj.read(0x50)
    except OSError:
        return False
    if len(head) < 0x50: return False
    values = struct.unpack('<10Q', head)
    return values[0] == 0x50 and all(value <= size for value in (values[3], values[7], values[9]))


def _zstd():
    """(one-shot decompress, new streaming decompressor) from whichever zstd module this Python has"""
    module = None
    try:
        from backports import zstd as module
    except ImportError:
        try:
            from compression import zstd as module
        except ImportError:
            pass
    if module is not None:
        return module.decompress, module.ZstdDecompressor
    try:
        import zstandard
    except ImportError:
        raise Error('compressed packages (NSZ, XCZ) need Python 3.14 or: pip install zstandard')
    return (lambda data: zstandard.ZstdDecompressor().decompress(data, max_output_size=1 << 31),
            lambda: zstandard.ZstdDecompressor().decompressobj())


def ncz_plain(path, member):
    """the NCA an .ncz holds, from 0x4000 on, as it is stored: zstd over the section data decrypted. Yields chunks."""
    at = member.offset + 0x4000; end = member.offset + member.size
    if member.size < 0x4010 or nsp._read(path, at, 8) != b'NCZSECTN':
        raise Error('%s: %s is not an NCZ' % (path, member.name))
    count, = struct.unpack('<Q', nsp._read(path, at + 8, 8))
    if count > 0x10000 or at + 16 + count * 0x40 + 8 > end:     # 0x40 an entry: offset, size, crypto type, pad, key, counter
        raise Error('%s: %s has an invalid NCZ section table' % (path, member.name))
    at += 16 + count * 0x40
    once, stream = _zstd()
    with open(path, 'rb') as fileobj:
        fileobj.seek(at)
        if fileobj.read(8) == b'NCZBLOCK':
            version, kind, _, exponent, blocks, total = struct.unpack('<BBBBIQ', fileobj.read(16))
            if not 14 <= exponent <= 32 or not 0 < blocks <= 1 << 24:
                raise Error('%s: %s uses an unknown NCZ block format' % (path, member.name))
            sizes = struct.unpack('<%dI' % blocks, fileobj.read(4 * blocks))
            block = 1 << exponent
            if fileobj.tell() + sum(sizes) > end or total > blocks * block:
                raise Error('%s: %s has NCZ blocks out of range' % (path, member.name))
            for index, stored in enumerate(sizes):
                want = min(block, total - index * block)
                data = fileobj.read(stored)
                if len(data) != stored: raise Error('%s: %s is cut short' % (path, member.name))
                if stored < want:
                    try: data = once(data)
                    except Exception: raise Error('%s: %s has invalid zstd data' % (path, member.name))
                if len(data) != want: raise Error('%s: %s has an NCZ block of the wrong size' % (path, member.name))
                yield data
            return
        fileobj.seek(at)
        decoder = stream(); left = end - at
        while left:
            chunk = fileobj.read(min(CHUNK, left)); left -= len(chunk)
            if not chunk: raise Error('%s: %s is cut short' % (path, member.name))
            while chunk:
                try: data = decoder.decompress(chunk)
                except Exception: raise Error('%s: %s has invalid zstd data' % (path, member.name))
                if data: yield data
                chunk = b''
                if getattr(decoder, 'eof', False):      # the next frame, if there is one
                    chunk = getattr(decoder, 'unused_data', b''); decoder = stream()


def _copy(path, offset, size, output):
    with open(path, 'rb') as source:
        source.seek(offset)
        while size:
            data = source.read(min(CHUNK, size))
            if not data: raise Error('%s: data at %#x is cut short' % (path, offset))
            output.write(data); size -= len(data)


def extract(tools, title, folder, say=lambda message: None):
    """put the title's RomFS in folder: a romfs.nsp marker when it can be read in place, else romfs.bin.
    Returns 'package' or 'image'."""
    path, member = title.path, title.member
    os.makedirs(folder, exist_ok=True)
    for name in ('romfs.nsp', 'romfs.bin', 'romfs.bin.tmp', 'content.nca.tmp'):
        if os.path.exists(os.path.join(folder, name)): os.remove(os.path.join(folder, name))
    with tempfile.TemporaryDirectory(prefix='swr_dlc_') as work:
        header = nca_header(tools, path, member, work)
    if header is None:
        raise Error('%s: the installed keys do not open the data of %016x' % (path, title.title_id))
    packed = member.name.lower().endswith('.ncz')
    data_at, data_size, crypto = _romfs_range(header, struct.unpack_from('<Q', header, 0x208)[0], path)
    image = os.path.join(folder, 'romfs.bin'); partial = image + '.tmp'
    if packed:
        say('Unpacking %016x (%.1f MB)...' % (title.title_id, data_size / 1048576.0))
        position = 0x4000; left = data_size
        if data_at < position:
            raise Error('%s: the RomFS of %016x starts inside the NCZ header' % (path, title.title_id))
        with open(partial, 'wb') as output:
            for chunk in ncz_plain(path, member):
                if position + len(chunk) > data_at and left:
                    part = chunk[max(0, data_at - position):]
                    part = part[:left]; output.write(part); left -= len(part)
                position += len(chunk)
                if not left: break
        if left:
            os.remove(partial); raise Error('%s: the NCZ of %016x ends before its RomFS' % (path, title.title_id))
    elif crypto == 1:
        # plaintext: nothing to copy, the runtime reads the range from the package
        with open(os.path.join(folder, 'romfs.nsp'), 'w', encoding='utf-8', newline='\n') as marker:
            marker.write('SWRNSP1\n%s\n%x\n%x\n' % (path, member.offset + data_at, data_size))
        return 'package'
    elif crypto == 3:
        rights = header[0x230:0x240]
        arguments = ['-t', 'nca', '--romfs=romfs.bin.tmp', 'content.nca.tmp']
        if any(rights):
            key = None; bound = False
            tickets = [item for item in title.files if item.name.lower().endswith('.tik') and item.size <= 0x10000]
            beside = os.path.join(os.path.dirname(path), rights.hex() + '.tik')
            blobs = [nsp._read(path, item.offset, item.size) for item in tickets]
            if os.path.splitext(path)[1].lower() == '.nca' and os.path.isfile(beside):
                with open(beside, 'rb') as fileobj: blobs.append(fileobj.read(0x10000))
            for blob in blobs:
                rights_id, value = ticket_key(blob)
                if rights_id == rights.hex():
                    key = value; bound = value is None
            if bound:
                raise Error('%s: the ticket of %016x is bound to one console (a personalized ticket); its content '
                            'key cannot be read here. Dump the add-on content with a common ticket.'
                            % (path, title.title_id))
            if not key:
                raise Error('%s: the data of %016x needs a ticket (.tik) and the package has none for it'
                            % (path, title.title_id))
            arguments.insert(2, '--titlekey=' + key)
        say('Decrypting %016x (%.1f MB)...' % (title.title_id, data_size / 1048576.0))
        # hactool reads files, not ranges: the NCA goes next to its output for the time of the call (relative names,
        # so the game folder's own path never reaches hactool's command line)
        copy = os.path.join(folder, 'content.nca.tmp')
        try:
            with open(copy, 'wb') as output: _copy(path, member.offset, member.size, output)
            tools.run(arguments, cwd=folder)
        finally:
            if os.path.exists(copy): os.remove(copy)
    else:
        raise Error('%s: the data of %016x uses an encryption the installer does not read (type %d)'
                    % (path, title.title_id, crypto))
    if not _romfs_ok(partial) or os.path.getsize(partial) < data_size:
        if os.path.exists(partial): os.remove(partial)
        generation = max(header[0x206], header[0x220])
        raise Error('%s: the data of %016x did not decrypt to a RomFS; the installed keys probably lack this '
                    'content\'s generation (titlekek_%02x / key_area_key_application_%02x)'
                    % (path, title.title_id, max(generation - 1, 0), max(generation - 1, 0)))
    os.replace(partial, image)
    return 'image'


# ------------------------------------------------------------------ the manifest
def manifest_path(datadir):
    return os.path.join(datadir, 'dlc.manifest')


def read_manifest(datadir):
    """[(title id, version, 'data' or 'stub', package path)] in file order"""
    entries = []
    try:
        with open(manifest_path(datadir), encoding='utf-8') as fileobj:
            lines = fileobj.read().splitlines()
    except OSError:
        return entries
    for line in lines:
        line = line.strip()
        if not line or line.startswith('#'): continue
        parts = line.split(None, 2)
        if len(parts) < 2 or len(parts[0]) != 16: continue
        try: title_id, version = int(parts[0], 16), int(parts[1])
        except ValueError: continue
        rest = parts[2] if len(parts) > 2 else ''
        kind = 'data'
        word = rest.split(None, 1)
        if word and word[0] in ('data', 'stub'):
            kind = word[0]; rest = word[1] if len(word) > 1 else ''
        entries.append((title_id, version, kind, rest.strip()))
    return entries


def write_manifest(datadir, entries):
    path = manifest_path(datadir)
    if not entries:
        if os.path.exists(path): os.remove(path)
        return
    entries = sorted(entries)
    text = '# SwitchRecomp add-on content: <title id> <version> <data|stub> <package>\n'
    text += ''.join('%016x %d %s %s\n' % entry for entry in entries)
    with open(path + '.tmp', 'w', encoding='utf-8', newline='\n') as fileobj: fileobj.write(text)
    os.replace(path + '.tmp', path)


def title_folder(datadir, title_id):
    return os.path.join(datadir, 'dlc', '%016x' % title_id)


def same_file(left, right):
    return os.path.normcase(os.path.abspath(left)) == os.path.normcase(os.path.abspath(right))


def game_title_id(datadir):
    try:
        with open(os.path.join(datadir, 'exefs', 'main.npdm'), 'rb') as fileobj:
            data = fileobj.read()
    except OSError:
        raise Error('%s is not a built game\'s data folder (build the game first)' % datadir)
    import nxloader
    return nxloader.parse_npdm(data)['title_id']


def _stamp(path):
    info = os.stat(path)
    return {'source': os.path.abspath(path), 'size': info.st_size, 'mtime_ns': info.st_mtime_ns}


def _installed(datadir, title_id, source):
    """True when this title is already installed from this very file"""
    folder = title_folder(datadir, title_id)
    try:
        with open(os.path.join(folder, 'install.json'), encoding='utf-8') as fileobj:
            state = json.load(fileobj)
        stamp = _stamp(source)
    except (OSError, ValueError):
        return False
    if any(state.get(key) != stamp[key] for key in ('size', 'mtime_ns')) or not same_file(state.get('source', ''), source):
        return False
    if state.get('storage') == 'image': return _romfs_ok(os.path.join(folder, 'romfs.bin'))
    if state.get('storage') == 'package': return os.path.isfile(os.path.join(folder, 'romfs.nsp'))
    return state.get('storage') == 'stub'


def _has_data(datadir, title_id):
    folder = title_folder(datadir, title_id)
    return os.path.isfile(os.path.join(folder, 'romfs.nsp')) or _romfs_ok(os.path.join(folder, 'romfs.bin'))


def install(paths, datadir, say=print, keys=None, hactool=None, allow_stub=False):
    """install the add-on content of each package for the game built in datadir. Returns (installed, errors)."""
    datadir = os.path.abspath(datadir)
    application = game_title_id(datadir)
    tools = Tools(keys, hactool)
    entries = {entry[0]: entry for entry in read_manifest(datadir)}
    done, errors = [], []
    for path in paths:
        path = os.path.abspath(path)
        try:
            titles, others = scan(path, tools)
            mine = [title for title in titles if is_addon_of(title.title_id, application)]
            if not mine:
                if titles:
                    raise Error('%s: this add-on content is for %016x, not for this game (%016x)'
                                % (path, base_of(titles[0].title_id), application))
                held = ', '.join(sorted({TYPE_NAMES.get(other.type, 'other content') for other in others}))
                raise Error('%s: the package has no add-on content%s' % (path, ' (it holds %s)' % held if held else ''))
            for title in mine:      # one title failing does not stop the package's other titles
                try:
                    done.append(_install_title(tools, title, datadir, entries, say, allow_stub))
                except (Error, OSError) as problem:
                    errors.append(str(problem)); say('DLC error: %s' % problem)
        except (Error, OSError) as problem:
            errors.append(str(problem)); say('DLC error: %s' % problem)
    return done, errors


def _install_title(tools, title, datadir, entries, say, allow_stub):
    path = title.path
    folder = title_folder(datadir, title.title_id)
    if _installed(datadir, title.title_id, path) and title.title_id in entries:
        say('DLC: %016x v%d is already installed' % (title.title_id, title.version))
        return title.title_id
    # two packages with the same title (a standalone DLC file and a combined one): the installed one stays unless
    # this one is newer, so that every listed package is either installed or refused with a reason
    have = entries.get(title.title_id)
    if (have and have[3] and not same_file(have[3], path) and have[1] >= title.version
            and _has_data(datadir, title.title_id)):
        raise Error('%s: %016x v%d is already installed from %s (remove that one first to install it from this file)'
                    % (path, title.title_id, title.version, os.path.basename(have[3])))
    kind = 'data'
    try:
        storage = extract(tools, title, folder, say)
    except (Error, OSError) as problem:
        if not allow_stub:
            # nothing half-installed: without data the title leaves the manifest and its folder goes
            shutil.rmtree(folder, ignore_errors=True)
            if entries.pop(title.title_id, None): write_manifest(datadir, list(entries.values()))
            root = os.path.join(datadir, 'dlc')
            if os.path.isdir(root) and not os.listdir(root): os.rmdir(root)
            raise
        say('DLC warning: %s' % problem); storage = kind = 'stub'
    state = _stamp(path); state.update(storage=storage, version=title.version)
    with open(os.path.join(folder, 'install.json'), 'w', encoding='utf-8') as fileobj:
        json.dump(state, fileobj, indent=1)
    entries[title.title_id] = (title.title_id, title.version, kind, path)
    write_manifest(datadir, list(entries.values()))
    what = {'package': 'read in place from the package', 'image': 'RomFS image, %.1f KB' % (
        os.path.getsize(os.path.join(folder, 'romfs.bin')) / 1024.0 if storage == 'image' else 0),
        'stub': 'license only, no data: the game may not accept it'}[storage]
    say('DLC: installed %016x v%d (%s) from %s' % (title.title_id, title.version, what, path))
    return title.title_id


def remove(datadir, what, say=print):
    """remove titles by id (16 hex digits) or by the package they came from. Returns the number removed."""
    datadir = os.path.abspath(datadir)
    entries = read_manifest(datadir); keep = []
    for entry in entries:
        match = ('%016x' % entry[0] == what.lower()) or (entry[3] and same_file(entry[3], what))
        if not match:
            keep.append(entry); continue
        folder = title_folder(datadir, entry[0])
        if os.path.isdir(folder): shutil.rmtree(folder, ignore_errors=True)
        say('DLC: removed %016x' % entry[0])
    write_manifest(datadir, keep)
    root = os.path.join(datadir, 'dlc')
    if os.path.isdir(root) and not os.listdir(root): os.rmdir(root)
    return len(entries) - len(keep)


def sync(paths, datadir, say=print, keys=None, hactool=None):
    """make the installed add-on content exactly that of these packages. Returns (installed, errors)."""
    datadir = os.path.abspath(datadir)
    wanted = [os.path.abspath(path) for path in paths]
    for entry in read_manifest(datadir):
        if not any(same_file(entry[3], path) for path in wanted if entry[3]):
            remove(datadir, '%016x' % entry[0], say)
    todo = []
    for path in wanted:
        current = [entry for entry in read_manifest(datadir) if entry[3] and same_file(entry[3], path)]
        if current and not os.path.isfile(path):
            # an installed image doesn't need its package any more; one read in place does
            for entry in current:
                if os.path.isfile(os.path.join(title_folder(datadir, entry[0]), 'romfs.nsp')):
                    say('DLC warning: %016x is read from %s, which is missing' % (entry[0], path))
            continue
        if current and all(_installed(datadir, entry[0], path) for entry in current): continue
        todo.append(path)
    if not todo: return [entry[0] for entry in read_manifest(datadir)], []
    return install(todo, datadir, say, keys, hactool)


def describe(datadir, say=print):
    entries = read_manifest(datadir)
    for title_id, version, kind, source in entries:
        say('%016x v%d %s %s' % (title_id, version, kind, source))
    if not entries: say('No add-on content is installed.')
    return entries
