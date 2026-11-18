#!/usr/bin/env python3
"""Writes tests/av1/vectors.h: two tiny AV1 streams made from ffmpeg's synthetic test pattern (no game data), and, for
each picture, the hash of the 8-bit NV12 the runtime has to make of it.

python tests/av1/make_vectors.py          (needs an ffmpeg with libaom: both its AV1 encoder and its AV1 decoder)

The streams are encoded with libaom. The expected pictures come from libaom's decoder, not from dav1d, read as raw
planar video, and are turned into NV12 here by the rule of runtime/av1video.inc: samples of more than 8 bits shifted
down, chroma of 4:4:4 taken from the even rows and columns. tests/test_av1.c decodes the streams with dav1d through
av1video.inc and compares."""
import os, struct, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
#           name   size      frames  pixel format     bits  4:4:4
STREAMS = [('a420', (64, 48), 12, 'yuv420p', 8, False),
           ('b444', (50, 38), 5, 'yuv444p10le', 10, True)]


def fnv(data):
    h = 0xcbf29ce484222325
    for b in data: h = ((h ^ b) * 0x100000001b3) & 0xffffffffffffffff
    return h


def run(*cmd): subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def stream(tmp, name, size, frames, pix, bits, full):
    w, h = size; ivf = os.path.join(tmp, name + '.ivf'); raw = os.path.join(tmp, name + '.yuv')
    run('ffmpeg', '-y', '-f', 'lavfi', '-i', 'testsrc2=size=%dx%d:rate=1' % size, '-frames:v', str(frames), '-pix_fmt', pix,
        '-c:v', 'libaom-av1', '-strict', 'experimental', '-cpu-used', '4', '-crf', '12', '-b:v', '0', '-g', '30', '-f', 'ivf', ivf)
    run('ffmpeg', '-y', '-c:v', 'libaom-av1', '-i', ivf, '-f', 'rawvideo', '-pix_fmt', pix, raw)
    data = open(ivf, 'rb').read(); at = struct.unpack_from('<H', data, 6)[0]; units = []
    while at < len(data):
        n, pts = struct.unpack_from('<IQ', data, at); units.append((pts, data[at + 12:at + 12 + n])); at += 12 + n
    yuv = open(raw, 'rb').read(); bps = 2 if bits > 8 else 1
    cw, ch = (w, h) if full else ((w + 1) // 2, (h + 1) // 2); one = (w * h + 2 * cw * ch) * bps
    if len(yuv) % one or not yuv: raise SystemExit('%s: unexpected raw size %d' % (name, len(yuv)))
    def sample(plane, i): return yuv[plane + i] if bps == 1 else (yuv[plane + 2 * i] | yuv[plane + 2 * i + 1] << 8) >> (bits - 8)
    hashes = []
    for f in range(len(yuv) // one):
        base = f * one; u0 = base + w * h * bps; v0 = u0 + cw * ch * bps; out = bytearray()
        for i in range(w * h): out.append(sample(base, i))
        for r in range((h + 1) // 2):
            for x in range((w + 1) // 2):
                i = (2 * r) * cw + 2 * x if full else r * cw + x
                out.append(sample(u0, i)); out.append(sample(v0, i))
        hashes.append(fnv(out))
    if len(set(hashes)) != len(hashes): raise SystemExit(name + ': two pictures are the same, the order could not be checked')
    return units, hashes


def main():
    out = ['/* written by tests/av1/make_vectors.py: AV1 streams of ffmpeg\'s testsrc2 pattern (libaom), a temporal unit each,',
           '   and the FNV-1a hash of each picture as 8-bit NV12 (from libaom\'s decoder). No game data. */']
    with tempfile.TemporaryDirectory() as tmp:
        for name, size, frames, pix, bits, full in STREAMS:
            units, hashes = stream(tmp, name, size, frames, pix, bits, full)
            blob = b''.join(u for _, u in units)
            out.append('static const unsigned char %s_data[%d] = {' % (name, len(blob)))
            for i in range(0, len(blob), 32): out.append('  ' + ','.join(str(b) for b in blob[i:i + 32]) + ',')
            out.append('};')
            out.append('static const unsigned %s_sizes[%d] = {%s};' % (name, len(units), ','.join(str(len(u)) for _, u in units)))
            out.append('static const unsigned long long %s_hashes[%d] = {%s};' % (name, len(hashes), ','.join('0x%016xull' % x for x in hashes)))
            out.append('enum { %s_w = %d, %s_h = %d };' % (name, size[0], name, size[1]))
            print('%s: %d units, %d bytes, %d pictures' % (name, len(units), len(blob), len(hashes)))
    open(os.path.join(HERE, 'vectors.h'), 'w', newline='\n').write('\n'.join(out) + '\n')


if __name__ == '__main__': main()
