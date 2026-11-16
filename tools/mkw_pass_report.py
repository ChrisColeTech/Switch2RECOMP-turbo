# SwitchRecomp - Copyright 2026 MZ Labs
# Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
# Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen.
"""Read-only analysis of a run's native Vulkan pass dumps. PNG previews and sampled float statistics.

python tools/mkw_pass_report.py RUN
Previews clip to display range; raw dumps are untouched. Nonfinite values are magenta in previews.
"""
import argparse
import json
import math
from pathlib import Path
import re
import struct
import zlib


def png(path, w, h, rgb):
    def chunk(kind, data): return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    raw = b''.join(b'\0' + rgb[y*w*3:(y+1)*w*3] for y in range(h))
    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>2I5B', w, h, 8, 2, 0, 0, 0)) +
                     chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))


def unpack(fmt, data, offset):
    if fmt == 'R11G11B10F':
        v = struct.unpack_from('<I', data, offset*4)[0]
        values = []
        for bits, n in ((v & 2047, 6), ((v >> 11) & 2047, 6), (v >> 22, 5)):
            e, m = bits >> n, bits & ((1 << n)-1)
            values.append((math.nan if m else math.inf) if e == 31 else math.ldexp(1+m/(1 << n), e-15) if e else math.ldexp(m, -14-n))
        return values
    types = dict(RGBA16F=('4e',8), RG16F=('2e',4), R16F=('e',2), R32F=('f',4), RGBA32F=('4f',16))
    if fmt in types:
        t, stride = types[fmt]; return struct.unpack_from('<'+t, data, offset*stride)
    sizes = dict(RGBA8=4, RGBA8_SRGB=4, RGBA8SN=4, RG8=2, R8UI=1, R8=1)
    if fmt in sizes:
        n = sizes[fmt]; b = data[offset*n:(offset+1)*n]
        if fmt == 'RGBA8SN': return [max(-1, (v if v<128 else v-256)/127) for v in b]
        return [v/255 for v in b]
    raise ValueError(fmt)


def main():
    ap = argparse.ArgumentParser(); ap.add_argument('run', type=Path); ap.add_argument('--name', default='pass-report'); a = ap.parse_args()
    dest = a.run / a.name; dest.mkdir(exist_ok=False)
    results = []
    for path in sorted(a.run.glob('*.pass-*.raw')):
        match = re.search(r'pass-(\d+)-(\d+)-c(\d+)-ser(\d+)-(.+)-(\d+)x(\d+)\.raw$', path.name)
        if not match: continue
        frame, num, slot, serial, fmt, w, h = match.groups(); w, h = int(w), int(h)
        data = path.read_bytes(); pw = min(320, w); ph = max(1, round(h*pw/w))
        rgb = bytearray(); low = math.inf; high = -math.inf; nonfinite = zero = count = 0
        try:
            for y in range(ph):
                for x in range(pw):
                    vs = list(unpack(fmt, data, (y*h//ph)*w + x*w//pw))
                    color = vs[:3] if len(vs)>=3 else [vs[0]]*3
                    for v in color:
                        count += 1
                        if not math.isfinite(v): nonfinite += 1
                        else: low=min(low,v); high=max(high,v); zero += v==0
                    rgb.extend((255,0,255) if any(not math.isfinite(v) for v in color) else [round(min(1,max(0,v))*255) for v in color])
        except ValueError: continue
        name = f'{num}-c{slot}-ser{serial}-{fmt}.png'
        png(dest/name, pw, ph, rgb)
        rec = dict(pass_number=int(num), slot=int(slot), serial=serial, format=fmt, size=[w,h],
                   sampled_rgb_components=count, nonfinite=nonfinite, zeros=zero, minimum=low, maximum=high, preview=name)
        results.append(rec)
        if w >= 960: print(json.dumps(rec))
    (dest/'stats.json').write_text(json.dumps(results, indent=2))
    log = a.run / 'run.log'
    if log.exists():
        (dest/'passes.log').write_text('\n'.join(x for x in log.read_text(encoding='utf-8', errors='replace').splitlines() if '[pd]' in x), encoding='utf-8')
    print('Report:', dest)


if __name__ == '__main__': main()
