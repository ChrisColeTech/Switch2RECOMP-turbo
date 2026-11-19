#!/usr/bin/env python3
"""nxs_prebuild: every shader a game ships, found and translated when the game is built (renderer=vulkan).

Without this the game finds out which shaders it has while it runs: the NVN layer dumps each unknown one, nxs_live.py
translates it, and the draws that need it wait (up to 3 s a frame). A freshly built Mario Kart World spent its first
minutes at 3 s a frame. With it the build reads the game's files once, writes every shader it finds as the same dump the
runtime would write (same key: runtime/nvn/hle_nvn.c keys_hash), and runs the translator over them, several at a time at
below-normal priority. The runtime then finds each shader in the cache at its first lookup.

  nxs_prebuild.py --game <build folder> [--jobs N] [--scan-only] [--rescan] [--all-files] [--quiet]
  (switchrecomp.py calls run() at the end of a build)

Where the files are read from: <Game>_data/romfs (a folder), romfs.nup / romfs.nsp (the package the game reads in
place, through the engine's own readers nup.py / nsp.py), installed add-on content folders, and exefs/main.

What is found: BNSH (bfsha, effects, layouts, main), agl's BAHS (sharcb), NDSR / NDVE packs and sead's primitive drawer,
inside zstd (with the game's dictionaries), Yaz0 and SARC. Game Freak's packs (Trinity .trpfs, GFPAK) compress their
files with Oodle, which this tool has no decoder for: those games keep the live path (the cache survives rebuilds).

The scanning rules (the functions marked "shader_split.py") are taken from nxnative's dm/gfx/tools/shader_split.py:
  MIT License, Copyright (c) 2026 seedds (the full text: tools/nxshader/xlate_bin/LICENSE-nxnative.txt)."""
import argparse, hashlib, json, multiprocessing, os, re, shutil, struct, subprocess, sys, threading, time

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.normpath(os.path.join(HERE, '..', '..'))
VERSION = 2            # of the scan: a change here scans every game again
CTL_MAGIC, GFX_MAGIC, CS_MAGIC = bytes.fromhex('34127698'), bytes.fromhex('78563412'), bytes.fromhex('79563412')
ZSTD_MAGIC = bytes.fromhex('28b52ffd')
STAGES = ['vs', 'hs', 'ds', 'gs', 'ps', 'cs']
SPH_STAGE = {1: 'vs', 2: 'hs', 3: 'ds', 4: 'gs', 5: 'ps'}
NEST = 8               # archives inside archives
MAX_FILE = 3 << 29     # a file larger than 1.5 GB is not read whole (movies, sound and Game Freak's packs)
# names that hold no shaders: sound, movies and textures. Everything else is read and looked at by its content
MEDIA = ('.bwav', '.bfstm', '.bfwav', '.bars', '.bfsar', '.bfstp', '.bfgrp', '.opus', '.lopus', '.webm', '.ivf', '.mp4', '.av1', '.h264', '.h265',
         '.bntx', '.txtg', '.bfttf', '.bfotf', '.ttf', '.otf', '.bcfnt', '.bffnt', '.msbt', '.bwar', '.wav', '.ogg', '.png', '.jpg', '.astc', '.ktx')


class Bad(Exception):
    """a file this reader cannot follow: it is reported and left out, the scan goes on"""


# ---------- zstd, whichever module this Python has (3.14's compression.zstd, its backport, or zstandard) ----------
def _zstd_module():
    try:
        from compression import zstd
        return 'std', zstd
    except ImportError: pass
    try:
        from backports import zstd
        return 'std', zstd
    except ImportError: pass
    try:
        import zstandard
        return 'zstandard', zstandard
    except ImportError: return None, None


_ZKIND, _ZMOD = _zstd_module()
ZDICTS, _ZD = {}, {}   # the game's zstd dictionaries by ID, and their digested form


def unzstd(d, did=0, where=''):
    if _ZMOD is None: raise Bad('%s: no zstd module (pip install backports.zstd)' % where)
    if did and did not in ZDICTS: raise Bad('%s: a frame needs zstd dictionary %d, which the game does not ship' % (where, did))
    try:
        if _ZKIND == 'std':
            if did and did not in _ZD: _ZD[did] = _ZMOD.ZstdDict(ZDICTS[did])
            return _ZMOD.decompress(d, zstd_dict=_ZD[did]) if did else _ZMOD.decompress(d)
        if did and did not in _ZD: _ZD[did] = _ZMOD.ZstdCompressionDict(ZDICTS[did])
        ctx = _ZMOD.ZstdDecompressor(dict_data=_ZD[did], max_window_size=1 << 31) if did else _ZMOD.ZstdDecompressor(max_window_size=1 << 31)
        out = []
        while d[:4] == ZSTD_MAGIC:
            o = ctx.decompressobj(); out.append(o.decompress(d)); d = o.unused_data
        return b''.join(out)
    except Bad: raise
    except Exception as e: raise Bad('%s: zstd: %s' % (where, str(e)[:120]))


def zs_dict_id(d):
    """shader_split.py: the zstd dictionary a frame names (0: none)"""
    fhd = d[4] if len(d) > 5 and d[:4] == ZSTD_MAGIC else 0
    n, o = (0, 1, 2, 4)[fhd & 3], 6 - (fhd >> 5 & 1)
    return int.from_bytes(d[o:o + n], 'little')


# ---------- the shader rules (nxnative shader_split.py, MIT) ----------
def field(d, fmt, at, where):
    if not 0 <= at <= len(d) - struct.calcsize(fmt): raise Bad('%s: %#x bytes at %#x lie outside the %#x bytes there are' % (where, struct.calcsize(fmt), at, len(d)))
    return struct.unpack_from(fmt, d, at)


def span(d, at, n, where):
    if not (0 <= at and 0 <= n <= len(d) - at): raise Bad('%s: %#x bytes at %#x lie outside the %#x bytes there are' % (where, n, at, len(d)))
    return d[at:at + n]


def sph_stage(code):
    if len(code) < 0x34: return None
    w = struct.unpack_from('<I', code, 0x30)[0]
    return 'vs' if (w >> 10) & 0xf == 0 and w & 0x1f == 1 else SPH_STAGE.get((w >> 10) & 0xf)


def nvn2_ctl(ctl):
    """a Switch 2 control section: version 0xc9 at +4, the code area's length at +0x44"""
    return len(ctl) >= 0x48 and ctl[:4] == CTL_MAGIC and ctl[4:8] == b'\xc9\0\0\0'


def ctl_len(ctl):
    if nvn2_ctl(ctl): return 0x48
    n = struct.unpack_from('<I', ctl, 0x20)[0]
    return n if 0x760 <= n < 0x880 else 0x880


def skey(ctl, code):
    """the runtime's key (hle_nvn.c keys_hash): 16 hex digits of sha256(control bytes + code area)"""
    c = ctl[:ctl_len(ctl)]
    if nvn2_ctl(ctl): c = c[:0x30] + bytes(4) + c[0x34:]
    return hashlib.sha256(c + code).hexdigest()[:16]


def check(st, ctl, code):
    """control section and code area consistent; the reason if not"""
    if ctl[:4] != CTL_MAGIC or len(ctl) < 0x24 or len(ctl) < ctl_len(ctl): return 'bad control section'
    if nvn2_ctl(ctl): return None if struct.unpack_from('<I', ctl, 0x44)[0] == len(code) else 'control +0x44 does not match the code area length'
    if st == 'cs' and code[:4] in (CS_MAGIC, GFX_MAGIC) and code[:4] != CS_MAGIC or st != 'cs' and code[:4] != GFX_MAGIC: return 'code area header magic does not match the stage'
    if struct.unpack_from('<I', ctl, 0x704)[0] != len(code): return 'control +0x704 does not match the code area length'
    return None


def nvn2_stage(ctl, code):
    if len(ctl) > 0x56 and ctl[0x56] in (5, 6, 7, 8): return {5: 'cs', 6: 'ts', 7: 'ms', 8: 'rt'}[ctl[0x56]]
    return sph_stage(bytes(0x30) + code[:4]) if len(code) >= 4 else None


def bnsh_shaders(d, base, label):
    """BNSH: +0x60 grsc block, +0x7c variation count, +0x80 variation table; a variation's program at +0x10 has six stage pointers from +8 (vertex, hull, domain,
    geometry, pixel, compute), Switch 2 adds task / mesh at +0x40 / +0x48. A stage: +8 control pointer, +0x10 code pointer, +0x18 code length, +0x1c control length"""
    where = '%s BNSH @%#x' % (label, base)
    if d[base + 0x60:base + 0x64] != b'grsc': raise Bad(where + ' has no grsc block')
    nvar, pvar = field(d, '<IQ', base + 0x7c, where + ' grsc')
    out = []
    for v in range(nvar):
        prog = field(d, '<Q', base + pvar + 0x40 * v + 0x10, where + ' variation')[0]
        if not prog: continue
        for si, st in enumerate(STAGES):
            sc = field(d, '<Q', base + prog + 8 + 8 * si, where + ' program')[0]
            if not sc: continue
            pctl, pcode, ncode, nctl = field(d, '<QQII', base + sc + 8, where + ' stage')
            ctl, code = span(d, base + pctl, nctl, where + ' control section'), span(d, base + pcode, ncode, where + ' code area')
            why = check(st, ctl, code)
            if why: raise Bad('%s variation %d %s: %s' % (where, v, st, why))
            out.append((st, ctl, code))
        for off, st, stage in ((0x40, 'ts', 6), (0x48, 'ms', 7)):
            if base + prog + off + 8 > len(d): continue
            sc = struct.unpack_from('<Q', d, base + prog + off)[0]
            if not sc or base + sc + 0x20 > len(d): continue
            pctl, pcode, ncode, nctl = struct.unpack_from('<QQII', d, base + sc + 8)
            if not 0x48 <= nctl < 0x10000 or base + pctl + nctl > len(d) or base + pcode + ncode > len(d): continue
            ctl, code = d[base + pctl:base + pctl + nctl], d[base + pcode:base + pcode + ncode]
            if not nvn2_ctl(ctl) or len(ctl) <= 0x56 or ctl[0x56] != stage: continue
            if check(st, ctl, code): continue
            out.append((st, ctl, code))
    return out


def bahs_shaders(d, label):
    """agl's sharcb: +0x1c code region start, +0x18 its length; 16-byte entries {code offset, 0, control length, signed offset of the control from the entry}"""
    rsz, region = field(d, '<II', 0x18, label + ' BAHS header')
    if region + rsz > len(d): raise Bad(label + ' BAHS code region out of bounds')
    found = []
    for a in range(0, region - 16, 4):
        off, z, csz, rel = struct.unpack_from('<IIIi', d, a)
        if z or off % 0x100: continue
        nvn2 = 0x48 <= csz < 0x10000 and 0 <= a + rel and a + rel + csz <= region and nvn2_ctl(d[a + rel:a + rel + csz])
        if not (nvn2 or csz in (0x880, 0x88c) or 0x760 <= csz < 0x880): continue
        c = a + rel
        if c < 0 or c + csz > region or d[c:c + 4] != CTL_MAGIC: continue
        n = struct.unpack_from('<I', d, c + (0x44 if nvn2 else 0x704))[0]
        if off + n > rsz: continue
        code, ctl = d[region + off:region + off + n], d[c:c + csz]
        st = nvn2_stage(ctl, code) if nvn2 else 'cs' if code[:4] == CS_MAGIC else sph_stage(code) if code[:4] == GFX_MAGIC else None
        if not st or check(st, ctl, code): continue
        found.append((a, c, (st, ctl, code)))
    # an entry never lies inside a control section (a section's +0x38..+0x47 reads as one)
    spans = sorted({(c, c + ctl_len(s[1])) for _, c, s in found})
    import bisect
    starts = [lo for lo, _ in spans]
    def inside(a):
        i = bisect.bisect_right(starts, a) - 1
        return i >= 0 and spans[i][0] <= a < spans[i][1]
    return [s for a, _, s in found if not inside(a)]


def sead_prim_shaders(d):
    """sead's primitive drawer for Switch 2: four u32 offsets (vertex control, pixel control, vertex code, pixel code)"""
    if len(d) < 0x10: return []
    offs = struct.unpack_from('<4I', d)
    out = []
    for st, c, k in (('vs', offs[0], offs[2]), ('ps', offs[1], offs[3])):
        if not (0x10 <= c and c + 0x48 <= len(d) and nvn2_ctl(d[c:c + 0x48])): return []
        own = struct.unpack_from('<I', d, c + 0x30)[0]
        ctl = d[c:c + max(own, 0x48)]
        n = struct.unpack_from('<I', ctl, 0x44)[0]
        code = d[k:k + n]
        if len(code) != n or nvn2_stage(ctl, code) != st or check(st, ctl, code): return []
        out.append((st, ctl, code))
    return out


def ndsr_shaders(d):
    """agl's NDSR pack (the upscaler): 16-byte records from +0x10 (or +0x14) up to a zero one; those whose first word points at a Switch 2 control section are shaders"""
    if d[:4] != b'NDSR' or len(d) < 0x20: return []
    def records_from(o):
        count = min(16, (len(d) - o) // 16)
        return sum(1 for c, _, _, _ in struct.iter_unpack('<4I', d[o:o + 16 * count]) if c + 0x48 <= len(d) and nvn2_ctl(d[c:c + 0x48]))
    out = []; o = max((0x10, 0x14), key=records_from)
    while o + 16 <= len(d):
        c, cl, k, x = struct.unpack_from('<4I', d, o)
        if not (c or cl or k or x): break
        if not (c + 0x48 <= len(d) and nvn2_ctl(d[c:c + 0x48])): o += 8; continue
        o += 16
        ctl = d[c:c + max(cl >> 4, 0x48)]
        code = d[k:k + struct.unpack_from('<I', ctl, 0x44)[0]]
        st = nvn2_stage(ctl, code)
        if st is None or check(st, ctl, code): continue
        out.append((st, ctl, code))
    return out


def ndve_shaders(d):
    """nn::ndve's movie enhancement file: 'NDVE', +0xc length of an NDSR pack, +0x14 count of 8-byte records, the pack, then networks whose control sections each
    name their own length (+0x30) and are followed by their code area"""
    if len(d) < 0x18: return []
    pack = 0x18 + 8 * struct.unpack_from('<I', d, 0x14)[0]; nets = pack + struct.unpack_from('<I', d, 0xc)[0]
    out = ndsr_shaders(d[pack:nets])
    c = d.find(CTL_MAGIC, nets)
    while c >= 0:
        if not c & 3 and c + 0x48 <= len(d) and nvn2_ctl(d[c:c + 0x48]):
            own, n = struct.unpack_from('<I', d, c + 0x30)[0], struct.unpack_from('<I', d, c + 0x44)[0]
            ctl, code = d[c:c + own], d[c + own:c + own + n]
            st = nvn2_stage(ctl, code)
            if st and len(code) == n and not check(st, ctl, code): out.append((st, ctl, code))
        c = d.find(CTL_MAGIC, c + 4)
    return out


def yaz0(s, label='Yaz0'):
    """shader_split.py: Yaz0 (group header bits: 1 = a literal byte, 0 = a back-reference of 2 or 3 bytes)"""
    n = int.from_bytes(s[4:8], 'big'); out = bytearray(); si = 16
    try:
        while len(out) < n:
            h = s[si]; si += 1
            if h == 0xff:   # eight literals
                out += s[si:si + 8]; si += 8; continue
            for _ in range(8):
                if h & 0x80:
                    out.append(s[si]); si += 1
                else:
                    b1, b2 = s[si], s[si + 1]; si += 2
                    dist, ln = ((b1 & 15) << 8 | b2) + 1, b1 >> 4
                    if ln: ln += 2
                    else: ln = s[si] + 0x12; si += 1
                    st = len(out) - dist
                    if st < 0: raise Bad('%s: a Yaz0 back-reference reaches before the start' % label)
                    out += out[st:st + ln] if dist >= ln else (out[st:] * (ln // dist + 1))[:ln]
                if len(out) >= n: break
                h <<= 1
    except IndexError: raise Bad('%s: the Yaz0 stream ends early' % label)
    return bytes(out[:n])


def sarc_members(d, label='SARC'):
    hl, bom, size, doff = field(d, '<HHII', 4, label + ' SARC header')
    n = field(d, '<H', hl + 6, label + ' SFAT header')[0]
    names = hl + 12 + 16 * n + 8
    if not names <= doff <= len(d): raise Bad(label + ': SARC tables past its data')
    for i in range(n):
        h, attr, s, e = struct.unpack_from('<IIII', d, hl + 12 + 16 * i)
        if not s <= e <= len(d) - doff: raise Bad('%s: SARC member %d lies outside the data' % (label, i))
        if attr >> 24:
            o = names + (attr & 0xffffff) * 4; z = d.find(b'\0', o)
            nm = d[o:z if z >= 0 else len(d)].decode('utf-8', 'replace')
        else: nm = '#%d' % i
        yield nm, d[doff + s:doff + e]


def has_shaders(d):
    return (b'BNSH\0\0\0\0' in d or b'BAHS' in d or d[:4] in (b'NDSR', b'NDVE')
            or (d[:4] == b'SARC' and (b'Yaz0' in d or ZSTD_MAGIC in d)))


def scan_blob(d, label, out, depth=0):
    """the shaders of one file's bytes (already out of its outer compression) → out: (stage, control, code)"""
    if depth > NEST: raise Bad(label + ': archives nested too deep')
    if d[:4] == ZSTD_MAGIC: d = unzstd(d, zs_dict_id(d), label)
    if d[:4] == b'Yaz0': d = yaz0(d, label)
    if d[:4] == b'SARC':
        for nm, m in sarc_members(d, label):
            if depth == 0 and not has_shaders(m) and m[:4] not in (b'Yaz0', ZSTD_MAGIC) and not (len(m) < 0x4000 and sead_prim_shaders(m)): continue
            try: scan_blob(m, label + '/' + nm, out, depth + 1)
            except Bad as e: out.append(('bad', str(e), b''))
        return
    if d[:4] == b'BAHS': out.extend(bahs_shaders(d, label)); return
    if d[:4] == b'NDSR': out.extend(ndsr_shaders(d)); return
    if d[:4] == b'NDVE': out.extend(ndve_shaders(d)); return
    prim = sead_prim_shaders(d)
    if prim: out.extend(prim); return
    for m in re.finditer(b'BNSH\0\0\0\0', d):
        if m.start() + 0x20 > len(d): continue
        size = struct.unpack_from('<I', d, m.start() + 0x1c)[0]
        if m.start() + size <= len(d) and d[m.start() + 0x60:m.start() + 0x64] == b'grsc':
            try: out.extend(bnsh_shaders(d, m.start(), label))
            except Bad as e: out.append(('bad', str(e), b''))


# ---------- where a game's files are ----------
class DirSource:
    def __init__(self, root): self.root = root
    def files(self):
        for base, _, names in os.walk(self.root):
            for n in names:
                p = os.path.join(base, n)
                try: yield os.path.relpath(p, self.root).replace('\\', '/'), os.path.getsize(p)
                except OSError: pass
    def read(self, rel):
        with open(os.path.join(self.root, rel), 'rb') as f: return f.read()


class PackageSource:
    """a NUP / NSP read in place through the engine's readers (walk_files, read)"""
    def __init__(self, romfs):
        self.romfs = romfs; self.nodes = None
    def files(self):
        self.nodes = {}
        for rel, node in self.romfs.walk_files():
            self.nodes[rel] = node
            yield rel, node.size
    def read(self, rel):
        if self.nodes is None: list(self.files())
        return self.romfs.read(self.nodes[rel])


def open_source(spec):
    kind = spec[0]
    if kind == 'dir': return DirSource(spec[1])
    if ENGINE not in sys.path: sys.path.insert(0, ENGINE)
    if kind == 'nup':
        import nup
        return PackageSource(nup.NupRomfs(nup.load(spec[1])))
    if kind == 'nsp':
        import nsp
        return PackageSource(nsp.romfs_of(nsp.load(spec[1], keys=spec[2], update=spec[3], embedded=spec[4])))
    raise ValueError(spec)


def source_identity(spec):
    out = [spec[0]]
    for p in spec[1:]:
        if isinstance(p, str) and os.path.isfile(p):
            s = os.stat(p); out.append([p, s.st_size, s.st_mtime_ns])
        elif isinstance(p, str) and os.path.isdir(p):
            n = size = newest = 0
            for base, _, names in os.walk(p):
                for name in names:
                    try: s = os.stat(os.path.join(base, name))
                    except OSError: continue
                    n += 1; size += s.st_size; newest = max(newest, s.st_mtime_ns)
            out.append([p, n, size, newest])
        else: out.append(p)
    return out


def game_sources(datadir):
    """what <Game>_data says the game reads: [(label, spec)]"""
    out = []
    romfs = os.path.join(datadir, 'romfs')
    nup_marker, nsp_marker = os.path.join(datadir, 'romfs.nup'), os.path.join(datadir, 'romfs.nsp')
    if os.path.isfile(nup_marker):
        out.append(('romfs', ('nup', open(nup_marker, encoding='utf-8').read().strip())))
    elif os.path.isfile(nsp_marker):
        lines = open(nsp_marker, encoding='utf-8').read().splitlines()
        if lines and lines[0] in ('SWRNSP1', 'SWRNSP2') and len(lines) > 1:
            keys = next((l[5:] for l in lines if l.startswith('keys ')), None)
            update = next((l[7:] for l in lines if l.startswith('update ')), None)
            patched = any(l.startswith('patch ') for l in lines)
            out.append(('romfs', ('nsp', lines[1], keys, update, patched or lines[0] == 'SWRNSP1')))
    elif os.path.isdir(romfs):
        out.append(('romfs', ('dir', romfs)))
    dlc = os.path.join(datadir, 'dlc')
    if os.path.isdir(dlc):
        for name in sorted(os.listdir(dlc)):
            p = os.path.join(dlc, name)
            if os.path.isdir(p): out.append(('dlc/' + name, ('dir', p)))
    exefs = os.path.join(datadir, 'exefs')
    if os.path.isdir(exefs): out.append(('exefs', ('dir', exefs)))
    return out


def nso_image(d):
    """an NSO's three segments, out of LZ4 (shaders embedded in main)"""
    if d[:4] != b'NSO0' or len(d) < 0x70: return d
    if ENGINE not in sys.path: sys.path.insert(0, ENGINE)
    try:
        import nxloader
        flags = struct.unpack_from('<I', d, 0xc)[0]
        parts = []
        for i in range(3):
            off, _, size = struct.unpack_from('<3I', d, 0x10 + 0x10 * i)
            stored = struct.unpack_from('<I', d, 0x60 + 4 * i)[0]
            raw = d[off:off + stored]
            parts.append(nxloader.lz4_block(raw, size) if flags >> i & 1 else raw)
        return b''.join(bytes(p) for p in parts)
    except Exception: return d


# ---------- the scan, in worker processes ----------
_SRC = None; _DUMP = None; _ALL = False


def _worker_init(spec, dump, zdicts, all_files):
    global _SRC, _DUMP, _ALL
    _SRC, _DUMP, _ALL = open_source(spec), dump, all_files
    ZDICTS.update(zdicts)
    if sys.platform == 'win32':
        try:
            import ctypes
            ctypes.windll.kernel32.SetPriorityClass(ctypes.windll.kernel32.GetCurrentProcess(), 0x00004000)
        except Exception: pass


def write_dump(dump, key, ctl, code):
    """the runtime's dump (nxs_rt.c): <key>.ctl = 0x880 bytes (a Switch 2 block is its first 0xe0 bytes, the rest zeros), <key>.code = the code area"""
    cp, kp = os.path.join(dump, key + '.ctl'), os.path.join(dump, key + '.code')
    if os.path.exists(cp) and os.path.exists(kp): return
    c = (ctl[:0xe0] if nvn2_ctl(ctl) else ctl[:0x880]).ljust(0x880, b'\0')
    for path, data in ((kp, code), (cp, c)):   # the control last: a dump counts once its .ctl is there
        tmp = path + '.tmp-%d' % os.getpid()
        with open(tmp, 'wb') as f: f.write(data)
        try: os.replace(tmp, path)
        except OSError:   # another worker found the same shader in another file and is writing it
            try: os.remove(tmp)
            except OSError: pass
            if not os.path.exists(path): raise


def scan_one(rel):
    """→ (rel, {key: stage}, [problems], bytes read)"""
    keys, bad = {}, []
    try:
        d = _SRC.read(rel)
        n = len(d)
        if d[:4] == b'NSO0': d = nso_image(d)
        if d[:4] == ZSTD_MAGIC: d = unzstd(d, zs_dict_id(d), rel)
        elif d[4:8] == ZSTD_MAGIC:   # a 4-byte size, then a frame (HAL's .cmp)
            try: d = unzstd(d[4:], zs_dict_id(d[4:]), rel)
            except Bad: pass
        found = []
        if d[:4] == b'Yaz0' or has_shaders(d) or (len(d) < 0x4000 and sead_prim_shaders(d)): scan_blob(d, rel, found)
        for st, ctl, code in found:
            if st == 'bad': bad.append(ctl); continue
            if st == 'rt': continue   # ray tracing: the renderer has no path for it
            k = skey(ctl, code)
            if k not in keys:
                keys[k] = st; write_dump(_DUMP, k, ctl, code)
        return rel, keys, bad, n
    except Bad as e: return rel, keys, bad + [str(e)], 0
    except MemoryError: return rel, keys, bad + ['%s: not enough memory to read it' % rel], 0
    except Exception as e: return rel, keys, bad + ['%s: %s: %s' % (rel, type(e).__name__, str(e)[:160])], 0


def scan_source(label, spec, dump, jobs, all_files, say):
    src = open_source(spec)
    files, skipped, big = [], 0, []
    for rel, size in src.files():
        low = rel.lower()
        for ext in ('.zs', '.zst', '.szs'):
            if low.endswith(ext): low = low[:-len(ext)]; break
        if not all_files and low.endswith(MEDIA): skipped += 1; continue
        if size > MAX_FILE: big.append((rel, size)); continue
        if size < 0x60: continue
        files.append((rel, size))
    zdicts = {}
    if any(rel == 'Pack/ZsDic.pack.zs' for rel, _ in files):
        try:
            d = unzstd(src.read('Pack/ZsDic.pack.zs'))
            zdicts = {struct.unpack_from('<I', m, 4)[0]: m for _, m in sarc_members(d, 'ZsDic') if m[:4] == bytes.fromhex('37a430ec')}
        except Exception as e: say('shaders: the zstd dictionaries could not be read (%s)' % e)
    del src
    files.sort(key=lambda f: -f[1])   # the largest first: the pool's tail is small files
    total = sum(s for _, s in files) or 1
    keys, problems, done, nfiles, t0, last = {}, [], 0, 0, time.time(), time.time()
    with multiprocessing.Pool(jobs, _worker_init, (spec, dump, zdicts, all_files)) as pool:
        for rel, k, bad, n in pool.imap_unordered(scan_one, [r for r, _ in files], chunksize=1 if len(files) < 2000 else 8):
            keys.update(k); problems += bad; done += n; nfiles += 1
            if time.time() - last >= 5:
                last = time.time()
                say('shaders: reading %s, %d of %d files, %d shaders found' % (label, nfiles, len(files), len(keys)))
    for rel, size in big: problems.append('%s: %.1f GB, not read' % (rel, size / 2**30))
    say('shaders: %s read in %.0f s: %d files (%d sound, movie and texture files left out), %d shaders' % (label, time.time() - t0, len(files), skipped, len(keys)))
    return keys, problems


# ---------- translation ----------
def python_exe(): return sys.executable or 'python'


def chain_of(cache_dir, xlate, dotnet):
    """the chain id of these tools (what a cache entry must carry to be current); None without a translator"""
    r = subprocess.run([python_exe(), os.path.join(HERE, 'nxs_live.py'), '--dump', os.path.join(cache_dir, 'prebuilt'), '--cache', cache_dir,
                        '--xlate', xlate, '--dotnet', dotnet, '--chain'], capture_output=True, text=True)
    c = r.stdout.strip().splitlines()[-1] if r.stdout.strip() else ''
    return c if re.fullmatch(r'[0-9a-f]{16}', c) else None


def is_current(cache_dir, key, chain):
    base = os.path.join(cache_dir, 'vk', key)
    try:
        if os.path.getsize(base + '.spv') < 20: return False
        with open(base + '.json', encoding='utf-8') as f: head = f.read()
        return ('"chain": "%s"' % chain) in head
    except OSError: return False


def translate(cache_dir, dump, keys, jobs, say, xlate, dotnet, batch=96):
    """→ (translated, failed)"""
    parts = [keys[i::jobs] for i in range(jobs)]
    parts = [p for p in parts if p]
    procs, counts, failed = [], [0] * len(parts), [0] * len(parts)
    flags = 0x00004000 if sys.platform == 'win32' else 0
    for i, part in enumerate(parts):
        kf = os.path.join(cache_dir, 'prebuild-keys-%d.txt' % i)
        with open(kf, 'w') as f: f.write('\n'.join(part) + '\n')
        p = subprocess.Popen([python_exe(), '-u', os.path.join(HERE, 'nxs_live.py'), '--once', '--dump', dump, '--cache', cache_dir, '--xlate', xlate,
                              '--dotnet', dotnet, '--keys', kf, '--batch', str(batch), '--progress'],
                             stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, creationflags=flags)
        procs.append((p, kf))
        def pump(i=i, p=p):
            for line in p.stdout:
                m = re.match(r'progress (\d+) (\d+)', line)
                if m: counts[i] = int(m.group(1)); failed[i] = int(m.group(2))
        threading.Thread(target=pump, daemon=True).start()
    t0 = time.time(); last = 0
    while any(p.poll() is None for p, _ in procs):
        time.sleep(0.5)
        if time.time() - last >= 5:
            last = time.time(); n = sum(counts) + sum(failed)
            if n:
                rate = n / max(time.time() - t0, 1)
                say('shaders: translated %d of %d (%.0f a second, about %.0f s left)' % (n, len(keys), rate, (len(keys) - n) / max(rate, 0.1)))
    time.sleep(0.2)
    for p, kf in procs:
        try: os.remove(kf)
        except OSError: pass
    return sum(counts), sum(failed)


def run(outdir, name, jobs=4, say=print, rescan=False, scan_only=False, all_files=False, dotnet='dotnet'):
    """build-time shaders for the game <outdir>/<name>: → a summary dict. Never raises for a game it cannot read: the live path stays"""
    datadir = os.path.join(outdir, name + '_data')
    cache_dir = os.path.join(outdir, name + '_nvn', 'cache')
    dump = os.path.join(cache_dir, 'prebuilt')
    state_path = os.path.join(cache_dir, 'prebuild.json')
    jobs = max(1, min(int(jobs or 1), 8))
    sources = game_sources(datadir)
    if not sources: say('shaders: no game files under %s' % datadir); return {'keys': 0}
    os.makedirs(dump, exist_ok=True); os.makedirs(os.path.join(cache_dir, 'vk'), exist_ok=True)
    identity = {'version': VERSION, 'all': bool(all_files), 'sources': [[label, source_identity(spec)] for label, spec in sources]}
    state = None
    if not rescan and os.path.isfile(state_path):
        try:
            state = json.load(open(state_path, encoding='utf-8'))
            if state.get('identity') != json.loads(json.dumps(identity)): state = None
        except (OSError, ValueError): state = None
    t0 = time.time()
    if state: keys = state['keys']
    else:
        keys, problems = {}, []
        for label, spec in sources:
            try: k, bad = scan_source(label, spec, dump, jobs, all_files, say)
            except Exception as e:
                say('shaders: %s could not be read (%s: %s); its shaders are translated while the game runs' % (label, type(e).__name__, str(e)[:200]))
                problems.append('%s: %s' % (label, e)); continue
            keys.update(k); problems += bad
        state = {'identity': identity, 'keys': keys, 'problems': problems[:200], 'scan_seconds': round(time.time() - t0, 1)}
        tmp = state_path + '.tmp'
        with open(tmp, 'w', encoding='utf-8') as f: json.dump(state, f)
        os.replace(tmp, state_path)
        if problems: say('shaders: %d files could not be followed (listed in %s)' % (len(problems), state_path))
    summary = {'keys': len(keys), 'translated': 0, 'failed': 0, 'current': 0}
    if scan_only or not keys: return summary
    xlate = os.path.join(HERE, 'xlate_bin')
    chain = chain_of(cache_dir, xlate, dotnet)
    if not chain:
        say('shaders: the translator did not start (is the .NET 10 runtime installed?): shaders are translated while the game runs'); return summary
    todo = sorted(k for k in keys if not is_current(cache_dir, k, chain) and os.path.exists(os.path.join(dump, k + '.ctl')))
    summary['current'] = len(keys) - len(todo)
    if not todo: say('shaders: all %d of the game\'s shaders are translated already' % len(keys)); return summary
    say('shaders: translating %d of the game\'s %d shaders, %d at a time...' % (len(todo), len(keys), jobs))
    t1 = time.time()
    ok, failed = translate(cache_dir, dump, todo, jobs, say, xlate, dotnet)
    left = [k for k in todo if not is_current(cache_dir, k, chain)]
    summary.update(translated=len(todo) - len(left), failed=len(left))
    say('shaders: %d translated in %.0f s%s' % (len(todo) - len(left), time.time() - t1,
                                                 ', %d could not be translated (%s)' % (len(left), os.path.join(cache_dir, 'nxs_live.log')) if left else ''))
    return summary


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--game', required=True, help='the build folder (the one with <Game>.exe and <Game>_data)')
    ap.add_argument('--name', help='the game\'s output name (default: the one <name>_data folder there)')
    ap.add_argument('--jobs', type=int, default=4); ap.add_argument('--dotnet', default='dotnet')
    ap.add_argument('--scan-only', action='store_true', help='find the shaders and write the dumps, translate nothing')
    ap.add_argument('--rescan', action='store_true', help='read the game\'s files again even if they have not changed')
    ap.add_argument('--all-files', action='store_true', help='read sound, movie and texture files too')
    a = ap.parse_args()
    name = a.name
    if not name:
        names = [d[:-5] for d in os.listdir(a.game) if d.endswith('_data') and os.path.isdir(os.path.join(a.game, d))]
        if len(names) != 1: ap.error('--name: %d games in %s' % (len(names), a.game))
        name = names[0]
    def say(s): print(s, flush=True)
    s = run(a.game, name, a.jobs, say, a.rescan, a.scan_only, a.all_files, a.dotnet)
    say('shaders: %s' % json.dumps(s))
    return 0


if __name__ == '__main__':
    sys.exit(main())
