#!/usr/bin/env python3
# Copyright 2026 ChrisColeTech
# SPDX-License-Identifier: MIT
"""Opus decoder test: builds tests/test_opus.c against the committed libopus (runtime/opus/<win|lin>/libopus.a) and runs it.

python tests/test_opus.py        (GCC: gcc/Library/bin from Install.bat, or the PATH; Linux: gcc)

It also checks that the archive is the one runtime/opus/README.md documents (its SHA-256)."""
import hashlib, os, re, subprocess, sys, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import s2r_paths as P


def main():
    win = os.name == 'nt'
    folder = ROOT / 'runtime' / 'opus' / ('win' if win else 'lin')
    lib = folder / 'libopus.a'
    if not lib.is_file(): raise SystemExit('missing: %s' % lib)
    sha = hashlib.sha256(lib.read_bytes()).hexdigest()
    readme = (ROOT / 'runtime' / 'opus' / 'README.md').read_text(encoding='utf-8')
    if sha not in readme: raise SystemExit('%s (SHA-256 %s) is not the archive runtime/opus/README.md documents' % (lib, sha))
    cc = P.gcc_tool('gcc') if win else 'gcc'
    env = P.env_with_gcc() if win else None
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, 'test_opus.exe' if win else 'test_opus')
        cmd = [cc, '-std=gnu11', '-O1', '-Wall', '-Werror=implicit-function-declaration', '-I', str(ROOT / 'runtime' / 'opus' / 'include'),
               str(ROOT / 'tests' / 'test_opus.c'), str(lib), '-o', exe] + (['-static'] if win else ['-lm'])
        r = subprocess.run(cmd, cwd=ROOT, env=env, capture_output=True, text=True)
        if r.returncode: raise SystemExit('compile failed:\n' + r.stdout + r.stderr)
        r = subprocess.run([exe], capture_output=True, text=True, timeout=600)
        print(r.stdout + r.stderr, end='')
        if r.returncode: raise SystemExit('tests/test_opus.c failed (%d)' % r.returncode)


if __name__ == '__main__': main()
