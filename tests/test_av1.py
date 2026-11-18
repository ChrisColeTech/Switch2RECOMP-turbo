#!/usr/bin/env python3
# SwitchRecomp - Copyright 2026 MZ Labs
# Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
# Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen.
"""AV1 decoder test: builds tests/test_av1.c against the committed dav1d (runtime/av1/<win|lin>/libdav1d.a) and runs it.

python tests/test_av1.py        (GCC: gcc/Library/bin from Install.bat, or the PATH; Linux: gcc)

It also checks that the archive is the one runtime/av1/README.md documents (its SHA-256)."""
import hashlib, os, re, subprocess, sys, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import s2r_paths as P


def main():
    win = os.name == 'nt'
    folder = ROOT / 'runtime' / 'av1' / ('win' if win else 'lin')
    lib = folder / 'libdav1d.a'
    if not lib.is_file(): raise SystemExit('missing: %s' % lib)
    sha = hashlib.sha256(lib.read_bytes()).hexdigest()
    readme = (ROOT / 'runtime' / 'av1' / 'README.md').read_text(encoding='utf-8')
    if sha not in readme: raise SystemExit('%s (SHA-256 %s) is not the archive runtime/av1/README.md documents' % (lib, sha))
    version = re.search(r'#define DAV1D_API_VERSION_MAJOR (\d+)', (ROOT / 'runtime/av1/include/dav1d/version.h').read_text())
    if not version: raise SystemExit('runtime/av1/include/dav1d/version.h: no API version')
    cc = P.gcc_tool('gcc') if win else 'gcc'
    env = P.env_with_gcc() if win else None
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, 'test_av1.exe' if win else 'test_av1')
        cmd = [cc, '-std=gnu11', '-O1', '-Wall', '-Werror=implicit-function-declaration', '-I', str(ROOT / 'runtime' / 'av1' / 'include'),
               str(ROOT / 'tests' / 'test_av1.c'), str(lib), '-o', exe] + (['-static'] if win else ['-lpthread', '-ldl', '-lm'])
        r = subprocess.run(cmd, cwd=ROOT, env=env, capture_output=True, text=True)
        if r.returncode: raise SystemExit('compile failed:\n' + r.stdout + r.stderr)
        r = subprocess.run([exe], capture_output=True, text=True, timeout=600)
        print(r.stdout + r.stderr, end='')
        if r.returncode: raise SystemExit('tests/test_av1.c failed (%d)' % r.returncode)


if __name__ == '__main__': main()
