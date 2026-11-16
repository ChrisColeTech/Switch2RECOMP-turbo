#!/usr/bin/env python3
# SwitchRecomp - Copyright 2026 MZ Labs
# Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
# Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen.
"""ZBIC: the zstd variant Switch 2 uses for some NSO segments (NSO flag bit 7, segment data starts with "ZBIC").

ZBIC is zstd 1.5.7 with two changes, both in framing, none in the entropy/LZ decoding itself:
  - the frame magic is 0x4349425A ("ZBIC") instead of 0xFD2FB528;
  - every FSE table description (sequence LL/OF/ML tables and Huffman weight tables) is a binary interpolative
    coded header instead of the standard FSE_readNCount format.
This module rewrites those headers into standard ones and hands the result to a stock zstd decoder (Python 3.14
compression.zstd, backports.zstd or zstandard), so no patched native zstd is needed. Written for this engine from
the format as implemented in Atmosphere-NX (libstratosphere util/zstd, ZSTD_ZBIC_SUPPORT) and kinnay/zbic; no code
or tables are copied from them (the BIC traversal table is generated below).
"""
import io

ZBIC_MAGIC = b'ZBIC'
ZSTD_MAGIC = b'\x28\xb5\x2f\xfd'
BLOCK_MAX = 128 << 10


class Error(ValueError):
    """ZBIC data that cannot be decoded."""


class Unsupported(Error):
    """No zstd decoder is available in this Python."""


def is_zbic(data):
    return bytes(data[:4]) == ZBIC_MAGIC


def _bic_table():
    # (middle, first, last) visiting order of the interpolative coder: midpoint, then upper half, then lower half,
    # over [0, 256]. A table for 2^n symbols is the tail of this one, which ends with (0, 0, 1)
    out = []

    def visit(lo, hi):
        if hi - lo < 2:
            return
        mid = (lo + hi) // 2
        out.append((mid, lo, hi))
        visit(mid, hi)
        visit(lo, mid)
    visit(0, 256)
    out.append((0, 0, 1))
    return out


BIC = _bic_table()


def read_bic_ncount(buf, pos, end, max_symbol):
    """BIC table header at buf[pos:end] -> (normalized counts, max symbol, table log, bytes used)."""
    size = end - pos
    if size <= 0:
        raise Error('empty table header')
    raw = buf[pos] & 0x7F
    low = buf[pos] >> 7
    if raw + 1 >= size:
        raise Error('table header is truncated')
    left = [raw]                # the header is a mixed-radix number read from its last byte backwards

    def refill(v):
        n = left[0]
        while n and v < (1 << 40):
            v = buf[pos + n] | (v << 8)
            n -= 1
        left[0] = n
        return v

    v = refill(0)
    nsym = v % 0x34 + 1
    v = refill(v // 0x34)
    if nsym > max_symbol:
        raise Error('table header has too many symbols')
    log = (v & 7) + 5
    v = refill(v >> 3)
    total = 1 << log
    last = v % total + 1 + ((nsym + 1) if low else 0)
    v = refill(v // total)
    span = 1
    while span <= nsym:
        span <<= 1
    if span > 0xFF:
        raise Error('table header has too many symbols')
    cum = [0] * 257
    cum[span] = last
    for mid, lo, hi in BIC[256 - span:]:
        a, b = cum[lo], cum[hi]
        if a == b:
            for k in range(lo + 1, hi):
                cum[k] = a
        else:
            if b < a:
                raise Error('table header is damaged')
            v, entry = divmod(v, b - a + 1)
            v = refill(v)
            cum[mid] = a + entry
    counts = []
    acc = 0
    for k in range(1, nsym + 2):
        count = cum[k] - acc - low
        acc = cum[k]
        if count < -1:
            raise Error('table header is damaged')
        counts.append(count)
        total -= abs(count)
    if total or left[0]:
        raise Error('table header is damaged')
    return counts, nsym, log, raw + 1


def write_ncount(counts, log):
    """standard zstd FSE table description (RFC 8878 4.1.1)"""
    bits = log - 5
    nbits = 4
    remaining = (1 << log) + 1
    threshold = 1 << log
    width = log + 1
    symbol = 0
    previous0 = False
    while symbol < len(counts) and remaining > 1:
        if previous0:
            start = symbol
            while symbol < len(counts) and not counts[symbol]:
                symbol += 1
            if symbol == len(counts):
                raise Error('table has no remaining probability')
            while symbol >= start + 3:
                start += 3
                bits |= 3 << nbits
                nbits += 2
            bits |= (symbol - start) << nbits
            nbits += 2
        count = counts[symbol]
        symbol += 1
        top = 2 * threshold - 1 - remaining
        remaining -= abs(count)
        count += 1
        if count >= threshold:
            count += top
        bits |= count << nbits
        nbits += width - (count < top)
        previous0 = count == 1
        if remaining < 1:
            raise Error('table probabilities overflow')
        while remaining < threshold:
            width -= 1
            threshold >>= 1
    if remaining != 1:
        raise Error('table probabilities do not add up')
    return bits.to_bytes((nbits + 7) // 8, 'little')


def _table(buf, pos, end, max_symbol):
    counts, _, log, used = read_bic_ncount(buf, pos, end, max_symbol)
    return write_ncount(counts, log), used


LITERAL_SIZES = {0: (3, 10), 1: (3, 10), 2: (4, 14), 3: (5, 18)}


def _literals(buf, p, end, out):
    b0 = buf[p]
    kind, fmt = b0 & 3, (b0 >> 2) & 3
    if kind < 2:                # raw or RLE
        if fmt in (0, 2):
            head, size = 1, b0 >> 3
        elif fmt == 1:
            head, size = 2, (b0 >> 4) | (buf[p + 1] << 4)
        else:
            head, size = 3, (b0 >> 4) | (buf[p + 1] << 4) | (buf[p + 2] << 12)
        n = head + (size if kind == 0 else 1)
        if p + n > end:
            raise Error('literals are truncated')
        out += buf[p:p + n]
        return p + n
    head, width = LITERAL_SIZES[fmt]
    if p + head > end:
        raise Error('literals are truncated')
    v = int.from_bytes(buf[p:p + head], 'little')
    mask = (1 << width) - 1
    regen, comp = (v >> 4) & mask, (v >> (4 + width)) & mask
    q = p + head
    end_literals = q + comp
    if end_literals > end:
        raise Error('literals are truncated')
    body = buf[q:q + comp]
    if kind == 2 and body and body[0] < 128:      # Huffman weights compressed with FSE
        h = body[0]
        if 1 + h > len(body):
            raise Error('Huffman table is truncated')
        table, used = _table(body, 1, 1 + h, 255)
        weights = table + body[1 + used:1 + h]
        if len(weights) >= 128:
            raise Error('Huffman table does not fit a standard header')
        body = bytes([len(weights)]) + weights + body[1 + h:]
    comp = len(body)
    if fmt:
        while fmt < 3 and max(regen, comp) > (1 << LITERAL_SIZES[fmt][1]) - 1:
            fmt += 1
    head, width = LITERAL_SIZES[fmt]
    if max(regen, comp) >> width:
        raise Error('literals do not fit a standard header')
    out += (kind | (fmt << 2) | (regen << 4) | (comp << (4 + width))).to_bytes(head, 'little')
    out += body
    return end_literals


def _block(buf, p, end):
    out = bytearray()
    p = _literals(buf, p, end, out)
    if p >= end:
        raise Error('sequences are truncated')
    b = buf[p]
    n = 1 if b < 128 else 2 if b < 255 else 3
    out += buf[p:p + n]
    p += n
    if b:
        if p >= end:
            raise Error('sequences are truncated')
        modes = buf[p]
        out.append(modes)
        p += 1
        for shift, max_symbol in ((6, 35), (4, 31), (2, 52)):     # literal length, offset, match length
            mode = (modes >> shift) & 3
            if mode == 1:
                out += buf[p:p + 1]
                p += 1
            elif mode == 2:
                table, used = _table(buf, p, end, max_symbol)
                out += table
                p += used
    if p > end:
        raise Error('sequences are truncated')
    out += buf[p:end]
    return out


def to_zstd(data):
    """ZBIC frames -> the same frames as standard zstd."""
    data = memoryview(data).tobytes() if not isinstance(data, (bytes, bytearray)) else data
    out = bytearray()
    p = 0
    while p < len(data):
        magic = data[p:p + 4]
        if len(magic) == 4 and (int.from_bytes(magic, 'little') & 0xFFFFFFF0) == 0x184D2A50:
            n = 8 + int.from_bytes(data[p + 4:p + 8], 'little')
            out += data[p:p + n]
            p += n
            continue
        if magic != ZBIC_MAGIC:
            raise Error('no ZBIC frame at byte %d' % p)
        out += ZSTD_MAGIC
        p += 4
        if p >= len(data):
            raise Error('frame header is truncated')
        fhd = data[p]
        single = (fhd >> 5) & 1
        head = 1 + (0 if single else 1) + (0, 1, 2, 4)[fhd & 3] + ((1 if single else 0), 2, 4, 8)[fhd >> 6]
        out += data[p:p + head]
        p += head
        while True:
            if p + 3 > len(data):
                raise Error('frame is truncated')
            bh = int.from_bytes(data[p:p + 3], 'little')
            last, kind, size = bh & 1, (bh >> 1) & 3, bh >> 3
            p += 3
            n = 1 if kind == 1 else size
            if kind == 3 or p + n > len(data):
                raise Error('frame is damaged or truncated')
            if kind == 2:
                block = _block(data, p, p + size)
                if len(block) > BLOCK_MAX:
                    raise Error('block does not fit a standard zstd block')
                out += ((len(block) << 3) | (2 << 1) | last).to_bytes(3, 'little')
                out += block
            else:
                out += data[p - 3:p + n]
            p += n
            if last:
                break
        if fhd & 4:
            out += data[p:p + 4]
            p += 4
    return bytes(out)


def _decode(frames, size):
    try:
        from backports import zstd
    except ImportError:
        try:
            from compression import zstd
        except ImportError:
            zstd = None
    if zstd is not None:
        option = getattr(getattr(zstd, 'DecompressionParameter', None), 'window_log_max', None)
        out = bytearray()
        while frames:
            dec = zstd.ZstdDecompressor(options={option: 31}) if option is not None else zstd.ZstdDecompressor()
            out += dec.decompress(frames, max_length=size + 1 - len(out))
            if not dec.eof:
                raise Error('zstd frame is truncated or expands past %d bytes' % size)
            frames = dec.unused_data
        return bytes(out)
    try:
        import zstandard
    except ImportError:
        raise Unsupported('decoding it needs Python 3.14 or newer, or: pip install zstandard')
    reader = zstandard.ZstdDecompressor(max_window_size=1 << 31).stream_reader(io.BytesIO(frames),
                                                                              read_across_frames=True)
    return reader.read(size + 1)


def decompress(data, size):
    """ZBIC data -> at most size + 1 bytes (the caller checks the exact size)."""
    frames = to_zstd(data)
    try:
        return _decode(frames, size)
    except Unsupported:
        raise
    except Exception as e:
        raise Error(str(e))
