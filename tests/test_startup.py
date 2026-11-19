# Copyright 2026 ChrisColeTech
# SPDX-License-Identifier: MIT
"""Run the Win32 startup painter on a memory DC. No game or visible window."""
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import s2r_paths as P


def main():
    cc, env = P.gcc_tool('gcc'), P.env_with_gcc()
    subprocess.run([cc, '-std=gnu11', '-fsyntax-only', '-Werror=implicit-function-declaration',
                    ROOT / 'runtime/backend_win32.c'], env=env, check=True, timeout=60)
    # Like the controller tests, keep the production functions together without linking the rest of the runtime.
    backend = (ROOT / 'runtime/backend_win32.c').read_text()
    start = backend.index('static u8* g_startup_px;')
    end = backend.index('static LRESULT CALLBACK wndproc(', start)
    with tempfile.TemporaryDirectory(prefix='startup-', dir=ROOT / 'build') as tmp:
        out = Path(tmp)
        (out / 'startup_production.h').write_text('#include "rt.h"\n#include "splash_img.h"\n' + backend[start:end])
        exe = out / 'startup-test.exe'
        subprocess.run([cc, '-std=gnu11', '-O1', '-I', out, '-I', ROOT / 'runtime',
                        ROOT / 'tests/startup_win32.c', '-lgdi32', '-luser32', '-o', exe],
                       env=env, check=True, timeout=60)
        subprocess.run([exe], env=env, check=True, timeout=15)


if __name__ == '__main__':
    main()
