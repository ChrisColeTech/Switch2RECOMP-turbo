#!/usr/bin/env python3
"""Builds runtime/opus/<win|lin>/libopus.a and runtime/opus/include from Opus's release archive (see README.md).

Windows:  python runtime/opus/build_opus.py --gcc-bin D:/path/to/gcc/Library/bin
Linux:    python3 runtime/opus/build_opus.py [--cc gcc-13]

The archive is checked against the SHA-256 below. The sources Opus's own makefiles list (celt_sources.mk,
silk_sources.mk, opus_sources.mk: the float build) are compiled one by one with the flags below, in <work>/src, so the
archive does not depend on where the repository is, and put into the archive in a fixed order without time stamps."""
import argparse, hashlib, os, re, shutil, subprocess, sys, tarfile, urllib.request
from concurrent.futures import ThreadPoolExecutor

VERSION = '1.5.2'
SOURCE = ('https://downloads.xiph.org/releases/opus/opus-%s.tar.gz' % VERSION,
          '65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1')   # as xiph.org's SHA256SUMS.txt
# the float build in plain C: no run-time CPU detection and no intrinsics (a decoder of 48 kHz movie and game sound
# takes a fraction of a millisecond a packet without them), no deep PLC / DRED / OSCE (off by default in Opus)
FLAGS = ['-O2', '-g0', '-w', '-fno-math-errno', '-DOPUS_BUILD', '-DUSE_ALLOCA', '-DHAVE_LRINTF', '-DHAVE_LRINT',
         '-DPACKAGE_VERSION="%s"' % VERSION, '-ffile-prefix-map=.=opus', '-Iinclude', '-Icelt', '-Isilk', '-Isilk/float', '-I.']
LISTS = [('celt_sources.mk', ['CELT_SOURCES']), ('silk_sources.mk', ['SILK_SOURCES', 'SILK_SOURCES_FLOAT']),
         ('opus_sources.mk', ['OPUS_SOURCES', 'OPUS_SOURCES_FLOAT'])]
HEADERS = ['opus.h', 'opus_defines.h', 'opus_multistream.h', 'opus_types.h']
HERE = os.path.dirname(os.path.abspath(__file__))


def fetch(url, sha, folder):
    path = os.path.join(folder, url.rsplit('/', 1)[1])
    if not os.path.isfile(path):
        with urllib.request.urlopen(url) as r, open(path + '.part', 'wb') as f: shutil.copyfileobj(r, f)
        os.replace(path + '.part', path)
    got = hashlib.sha256(open(path, 'rb').read()).hexdigest()
    if got != sha: raise SystemExit('%s: SHA-256 %s, expected %s' % (path, got, sha))
    return path


def sources(src):
    out = []
    for name, variables in LISTS:
        text = open(os.path.join(src, name), encoding='utf-8').read().replace('\\\n', ' ')
        for v in variables:
            m = re.search(r'^%s\s*=(.*)$' % v, text, re.M)
            if not m: raise SystemExit('%s: no %s' % (name, v))
            out += m.group(1).split()
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--work', default=os.path.join(HERE, '..', '..', 'build', 'opus'))
    ap.add_argument('--gcc-bin', help='Windows: the folder with x86_64-w64-mingw32-gcc.exe')
    ap.add_argument('--cc', default='gcc', help='Linux: the C compiler (the committed archive: gcc 13.3.0, Ubuntu 24.04)')
    ap.add_argument('--jobs', type=int, default=4)
    a = ap.parse_args()
    win = os.name == 'nt'
    work = os.path.abspath(a.work); os.makedirs(work, exist_ok=True)
    src = os.path.join(work, 'src'); shutil.rmtree(src, ignore_errors=True)
    with tarfile.open(fetch(*SOURCE, work)) as t: t.extractall(work, filter='data')
    os.rename(os.path.join(work, 'opus-' + VERSION), src)
    if win:
        if not a.gcc_bin or not os.path.isfile(os.path.join(a.gcc_bin, 'x86_64-w64-mingw32-gcc.exe')):
            raise SystemExit('--gcc-bin: the folder with x86_64-w64-mingw32-gcc.exe (Install.bat puts it in gcc/Library/bin)')
        g = os.path.abspath(a.gcc_bin)
        cc, ar = os.path.join(g, 'x86_64-w64-mingw32-gcc.exe'), os.path.join(g, 'x86_64-w64-mingw32-ar.exe')
        env = dict(os.environ, PATH=g + os.pathsep + os.environ['PATH'])
    else:
        cc, ar, env = a.cc, 'ar', dict(os.environ)
        if not shutil.which(cc): raise SystemExit(cc + ' is not installed (Ubuntu: sudo apt-get install build-essential)')
    flags = FLAGS + ([] if win else ['-fPIC'])
    files = sources(src)
    objs = [f[:-2].replace('/', '_') + '.o' for f in files]
    if len(set(objs)) != len(objs): raise SystemExit('two sources give the same object name')

    def one(pair):
        f, o = pair
        subprocess.check_call([cc] + flags + ['-c', f, '-o', o], cwd=src, env=env)
    with ThreadPoolExecutor(a.jobs) as ex: list(ex.map(one, zip(files, objs)))
    lib = os.path.join(src, 'libopus.a')
    subprocess.check_call([ar, 'rcsD', 'libopus.a'] + objs, cwd=src, env=env)   # D: no time stamps, owners or modes

    dest = os.path.join(HERE, 'win' if win else 'lin'); os.makedirs(dest, exist_ok=True)
    shutil.copyfile(lib, os.path.join(dest, 'libopus.a'))
    inc = os.path.join(HERE, 'include'); os.makedirs(inc, exist_ok=True)
    for h in HEADERS: shutil.copyfile(os.path.join(src, 'include', h), os.path.join(inc, h))
    shutil.copyfile(os.path.join(src, 'COPYING'), os.path.join(HERE, 'LICENSE'))
    data = open(os.path.join(dest, 'libopus.a'), 'rb').read()
    print('%s: %d objects, %d bytes, SHA-256 %s' % (os.path.join(dest, 'libopus.a'), len(objs), len(data), hashlib.sha256(data).hexdigest()))


if __name__ == '__main__': main()
