# SwitchRecomp - Copyright 2026 MZ Labs
# Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
# Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen.
"""Where the NVN translation thread spends its time: a table by function from a SWR_HOST_PROF sample file.

python tools/host_prof.py <game folder>/<Game>.log.hostprof.txt [--exe bin/runtime_win64_win32.exe] [--top 40]

The runtime writes the file when started with SWR_HOST_PROF=<start s>,<seconds>[,<file>] (runtime/nvn/host/nvn_host.c): one line a
sample, "<address in the runtime or absolute> <return address into the runtime or 0> <module or ->". Samples inside the runtime
count for the function they are in; samples in another module (the Vulkan driver, ntdll) count for that module and, in the second
table, for the runtime function that called into it. The exe must be the one that ran (same build): its symbols are read with nm."""
import argparse
import bisect
import collections
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import s2r_paths as P


def symbols(exe):
    nm = P.gcc_tool('nm')
    out = subprocess.run([nm, '-n', exe], capture_output=True, text=True, env=P.env_with_gcc()).stdout
    syms = []
    base = None
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] in 'tT':
            a = int(parts[0], 16)
            if parts[2] == '__image_base__': base = a
            syms.append((a, parts[2]))
    if base is None:   # the image base: the lowest text symbol's page, as the PE header has it
        base = min(a for a, _ in syms) & ~0xffff if syms else 0
        for line in out.splitlines():
            parts = line.split()
            if len(parts) == 3 and parts[2] == '__image_base__': base = int(parts[0], 16)
    syms.sort()
    return base, syms


def exports(path):
    """A DLL's exported functions as sorted (rva, name): its export directory, read from the file"""
    import struct
    try: d = open(path, 'rb').read()
    except OSError: return []
    pe = struct.unpack_from('<I', d, 0x3c)[0]
    nsec = struct.unpack_from('<H', d, pe + 6)[0]; opt = pe + 24; optsz = struct.unpack_from('<H', d, pe + 20)[0]
    if struct.unpack_from('<H', d, opt)[0] != 0x20b: return []
    exp_rva = struct.unpack_from('<I', d, opt + 112)[0]
    secs = [struct.unpack_from('<IIII', d, opt + optsz + 40 * i + 8) for i in range(nsec)]   # vsize, vaddr, rawsize, rawptr

    def off(rva):
        for vs, va, rs, rp in secs:
            if va <= rva < va + max(vs, rs): return rva - va + rp
        return None
    e = off(exp_rva)
    if e is None: return []
    nfun, nname, afun, aname, aord = struct.unpack_from('<IIIII', d, e + 20)
    funs = struct.unpack_from('<%dI' % nfun, d, off(afun)); names = struct.unpack_from('<%dI' % nname, d, off(aname)); ords = struct.unpack_from('<%dH' % nname, d, off(aord))
    out = []
    for i in range(nname):
        o = off(names[i]); end = d.index(b'\0', o)
        out.append((funs[ords[i]], d[o:end].decode('ascii', 'replace')))
    return sorted(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('file'); ap.add_argument('--exe', default=os.path.join(P.ROOT, 'bin', 'runtime_win64_win32.exe')); ap.add_argument('--top', type=int, default=40)
    a = ap.parse_args()
    base, syms = symbols(a.exe)
    addrs = [x for x, _ in syms]

    def name(rva):
        i = bisect.bisect_right(addrs, base + rva) - 1
        return syms[i][1] if i >= 0 else '?'

    exps = {}

    def outside_name(mod, rva):   # the export at or below the address (an approximation: internal functions count for the export before them)
        if mod not in exps: e = exports(mod); exps[mod] = ([x for x, _ in e], [y for _, y in e])
        a_, n_ = exps[mod]
        i = bisect.bisect_right(a_, rva) - 1
        return os.path.basename(mod) + '!' + (n_[i] if i >= 0 else '?')

    own, outside, via, outfn = collections.Counter(), collections.Counter(), collections.Counter(), collections.Counter()
    n = 0
    head = ''
    for line in open(a.file):
        if line.startswith('#'): head = line.strip(); continue
        p = line.split(None, 2)
        if len(p) < 3: continue
        ip, ret, mod = int(p[0], 16), int(p[1], 16), p[2].strip()
        n += 1
        if mod == '-': own[name(ip)] += 1
        else:
            outside[os.path.basename(mod)] += 1
            fn = outside_name(mod, ip) if mod != '?' else '?'
            outfn[fn] += 1
            via[(name(ret) if ret else '(no caller found)') + ' -> ' + fn] += 1
    print(head)
    print('%d samples; in the runtime %d (%.1f %%), in other modules %d' % (n, sum(own.values()), 100.0 * sum(own.values()) / max(1, n), sum(outside.values())))
    print('\n-- by function (self time; other modules by name)')
    allc = collections.Counter(own)
    for m, c in outside.items(): allc['[' + m + ']'] += c
    for k, c in allc.most_common(a.top): print('%6.2f %%  %6d  %s' % (100.0 * c / max(1, n), c, k))
    print('\n-- time in other modules, by the nearest export below the address')
    for k, c in outfn.most_common(a.top // 2): print('%6.2f %%  %6d  %s' % (100.0 * c / max(1, n), c, k))
    print('\n-- time in other modules, by the runtime function that called')
    for k, c in via.most_common(a.top // 2): print('%6.2f %%  %6d  %s' % (100.0 * c / max(1, n), c, k))


if __name__ == '__main__':
    main()
