#!/usr/bin/env python3
"""Builds runtime/av1/<win|lin>/libdav1d.a and runtime/av1/include/dav1d from dav1d's release archive (see README.md).

Windows:  python runtime/av1/build_dav1d.py --gcc-bin D:/path/to/gcc/Library/bin
Linux:    python3 runtime/av1/build_dav1d.py [--cc gcc-13]   (gcc and nasm from the distribution)

Everything it downloads is checked against the SHA-256 below. meson and ninja are installed, at the versions below,
into a virtual environment inside the work folder (default build/dav1d); nothing is installed system-wide.
The build runs in <work>/src and <work>/out, so the archive does not depend on where the repository is."""
import argparse, hashlib, os, shutil, subprocess, sys, tarfile, urllib.request, venv, zipfile

VERSION = '1.5.5'
COMMIT = '6c7bee685677fd997a431302be4fea8ea4017606'   # the tag 1.5.5 on code.videolan.org/videolan/dav1d
SOURCE = ('https://downloads.videolan.org/pub/videolan/dav1d/%s/dav1d-%s.tar.xz' % (VERSION, VERSION),
          'ca96b1d0ba3f01f55de7231d775892b64994ba061aaeeaaa4f662a390b3abf9e')
NASM_WIN = ('https://www.nasm.us/pub/nasm/releasebuilds/2.16.03/win64/nasm-2.16.03-win64.zip',
            '3ee4782247bcb874378d02f7eab4e294a84d3d15f3f6ee2de2f47a46aa7226e6')
MESON, NINJA = '1.12.1', '1.13.2'
# both bit depths (the Switch 2 movies seen so far are 8-bit; 10-bit ones are shifted to 8 by the runtime), the x86
# assembly, no tools, tests or examples. -ffile-prefix-map keeps the work folder out of the objects
OPTIONS = ['--buildtype=release', '--default-library=static', '-Dbitdepths=8,16', '-Denable_asm=true',
           '-Denable_tools=false', '-Denable_tests=false', '-Denable_examples=false', '-Dlogging=true',
           '-Db_lto=false', '-Db_staticpic=true', '-Dc_args=-ffile-prefix-map=../src=dav1d']
HEADERS = ['common.h', 'data.h', 'dav1d.h', 'headers.h', 'picture.h', 'version.h']
HERE = os.path.dirname(os.path.abspath(__file__))


def fetch(url, sha, folder):
    path = os.path.join(folder, url.rsplit('/', 1)[1])
    if not os.path.isfile(path):
        with urllib.request.urlopen(url) as r, open(path + '.part', 'wb') as f: shutil.copyfileobj(r, f)
        os.replace(path + '.part', path)
    got = hashlib.sha256(open(path, 'rb').read()).hexdigest()
    if got != sha: raise SystemExit('%s: SHA-256 %s, expected %s' % (path, got, sha))
    return path


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--work', default=os.path.join(HERE, '..', '..', 'build', 'dav1d'))
    ap.add_argument('--gcc-bin', help='Windows: the folder with x86_64-w64-mingw32-gcc.exe')
    ap.add_argument('--cc', default='gcc', help='Linux: the C compiler (the committed archive: gcc 13.3.0, Ubuntu 24.04)')
    ap.add_argument('--jobs', type=int, default=4)
    a = ap.parse_args()
    win = os.name == 'nt'
    work = os.path.abspath(a.work); os.makedirs(work, exist_ok=True)
    src, out = os.path.join(work, 'src'), os.path.join(work, 'out')
    for d in (src, out): shutil.rmtree(d, ignore_errors=True)
    with tarfile.open(fetch(*SOURCE, work)) as t: t.extractall(work, filter='data')
    os.rename(os.path.join(work, 'dav1d-' + VERSION), src)

    env = dict(os.environ)
    env['NASMENV'] = '--reproducible'   # no time stamp in the assembly's objects (COFF)
    if win:
        if not a.gcc_bin or not os.path.isfile(os.path.join(a.gcc_bin, 'x86_64-w64-mingw32-gcc.exe')):
            raise SystemExit('--gcc-bin: the folder with x86_64-w64-mingw32-gcc.exe (Install.bat puts it in gcc/Library/bin)')
        nasm = os.path.join(work, 'nasm')
        if not os.path.isdir(nasm):
            with zipfile.ZipFile(fetch(*NASM_WIN, work)) as z: z.extractall(nasm)
        g = os.path.abspath(a.gcc_bin)
        env['PATH'] = os.pathsep.join([os.path.join(nasm, 'nasm-2.16.03'), g, env['PATH']])
        env['CC'] = os.path.join(g, 'x86_64-w64-mingw32-gcc.exe'); env['AR'] = os.path.join(g, 'x86_64-w64-mingw32-ar.exe')
        env['STRIP'] = os.path.join(g, 'x86_64-w64-mingw32-strip.exe')
    else:
        for tool in (a.cc, 'nasm'):
            if not shutil.which(tool): raise SystemExit(tool + ' is not installed (Ubuntu: sudo apt-get install build-essential nasm)')
        env['CC'] = a.cc
    vdir = os.path.join(work, 'venv')
    vbin = os.path.join(vdir, 'Scripts' if win else 'bin')
    if not os.path.isdir(vbin): venv.EnvBuilder(with_pip=True).create(vdir)
    py = os.path.join(vbin, 'python.exe' if win else 'python')
    subprocess.check_call([py, '-m', 'pip', 'install', '-q', 'meson==' + MESON, 'ninja==' + NINJA], env=env)
    env['PATH'] = vbin + os.pathsep + env['PATH']
    meson = [py, '-m', 'mesonbuild.mesonmain']
    subprocess.check_call(meson + ['setup', out, src] + OPTIONS, env=env, cwd=work)
    subprocess.check_call([os.path.join(vbin, 'ninja'), '-C', out, '-j', str(a.jobs)], env=env, cwd=work)

    lib = os.path.join(out, 'src', 'libdav1d.a')
    dest = os.path.join(HERE, 'win' if win else 'lin'); os.makedirs(dest, exist_ok=True)
    shutil.copyfile(lib, os.path.join(dest, 'libdav1d.a'))
    inc = os.path.join(HERE, 'include', 'dav1d'); os.makedirs(inc, exist_ok=True)
    for h in HEADERS: shutil.copyfile(os.path.join(src, 'include', 'dav1d', h), os.path.join(inc, h))
    shutil.copyfile(os.path.join(src, 'COPYING'), os.path.join(HERE, 'LICENSE-dav1d.txt'))
    data = open(os.path.join(dest, 'libdav1d.a'), 'rb').read()
    print('%s: %d bytes, SHA-256 %s' % (os.path.join(dest, 'libdav1d.a'), len(data), hashlib.sha256(data).hexdigest()))


if __name__ == '__main__': main()
