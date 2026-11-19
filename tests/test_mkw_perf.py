# Copyright 2026 ChrisColeTech
# SPDX-License-Identifier: MIT
"""Compile the real writeback shadow update and compare both paths, including null/partial pages."""
import ctypes as C
import contextlib
import os
from pathlib import Path
import random
import subprocess
import sys
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'tools'))
from mkw_probe import EXE, startup_children


class ProcessOwnership(unittest.TestCase):
    def test_only_owned_startup_descendants(self):
        def row(pid, parent, created=100, path=EXE):
            return dict(ProcessId=pid, ParentProcessId=parent, created=created, ExecutablePath=str(path))
        records = [row(3,2), row(2,1), row(4,99), row(5,1,120), row(6,1,80), row(7,1,path=ROOT/'other.exe')]
        self.assertEqual([r['ProcessId'] for r in startup_children(records,1,100)], [2,3])
        self.assertEqual(startup_children(records,88,100), [])


class ShadowUpdate(unittest.TestCase):
    def test_equivalence_and_cost(self):
        source = (ROOT / 'runtime/nvn/vk/vk_mem.c').read_text()
        fn = source[source.index('static void wr_shadow('):source.index('// The words that differ between base and after')]
        d = ROOT / 'build/mkw-investigation' / ('shadow-test-' + str(time.time_ns()))
        d.mkdir(parents=True)
        with contextlib.nullcontext(d):
            src = Path(d) / 'shadow.c'; dll = Path(d) / 'shadow.dll'
            src.write_text('#include <stdint.h>\n#include <stddef.h>\n#include <string.h>\nenum { PAGE=4096 };\n' + fn +
                           '\n__declspec(dllexport) void run(uint32_t **s, uint64_t o, uint32_t *a, uint32_t *b, uint64_t n, int skip) { wr_shadow(s,o,a,b,n,skip); }\n')
            import s2r_paths   # GCC where this checkout has it: gcc/ here, beside the checkout, or on the PATH (a worktree has none of its own)
            subprocess.run([s2r_paths.gcc_tool('gcc'), '-O2', '-shared', str(src), '-o', str(dll)], check=True, env=s2r_paths.env_with_gcc())
            lib = C.CDLL(str(dll)); f = lib.run
            ptr = C.POINTER(C.c_uint32)
            f.argtypes = [C.POINTER(ptr), C.c_uint64, ptr, ptr, C.c_uint64, C.c_int]
            rng = random.Random(60)
            for case in range(200):
                pages = 9; total = pages*1024
                before = (C.c_uint32*total)(*[rng.getrandbits(32) for _ in range(total)])
                after = (C.c_uint32*total).from_buffer_copy(before)
                for _ in range(case*13): after[rng.randrange(total)] = rng.getrandbits(32)
                # Shadows deliberately differ from both input snapshots: unchanged words must stay untouched.
                old = (C.c_uint32*total)(*[rng.getrandbits(32) for _ in range(total)])
                new = (C.c_uint32*total).from_buffer_copy(old)
                oo = (ptr*pages)(); nn = (ptr*pages)()
                for p in range(pages):
                    if rng.randrange(4):
                        oo[p] = C.cast(C.byref(old,p*4096),ptr); nn[p] = C.cast(C.byref(new,p*4096),ptr)
                offset = rng.randrange(1024)*4; n = rng.randrange(total-offset//4+1)
                f(oo, offset, after, before, n, 0); f(nn, offset, after, before, n, 1)
                self.assertEqual(bytes(old), bytes(new), f'case {case}, offset {offset}, words {n}')
            n = 4*1024*1024; pages = n//1024
            before = (C.c_uint32*n)(); after = (C.c_uint32*n)(); shadow = (C.c_uint32*n)()
            table = (ptr*pages)(*[C.cast(C.byref(shadow,p*4096),ptr) for p in range(pages)])
            for i in range(0,n,97): after[i] = 1
            for skip in (0,1):
                t = time.perf_counter()
                for _ in range(40): f(table,0,after,before,n,skip)
                print(f'shadow 16 MiB, ~1% changed, skip={skip}: {(time.perf_counter()-t)*1000/40:.3f} ms', flush=True)
            # Windows keeps loaded DLLs open until explicitly unloaded.
            import _ctypes
            _ctypes.FreeLibrary(lib._handle)


if __name__ == '__main__': unittest.main()
