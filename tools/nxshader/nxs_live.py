#!/usr/bin/env python3
# SwitchRecomp - Copyright 2026 MZ Labs
# Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
# Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen.
"""nxs_live: shaders translated while the game runs (renderer=vulkan), with no build step before.

The runtime starts this once. The NVN layer (gfx/nxs_rt.c) writes each shader it has no translation for into the dump
folder (<key>.ctl, <key>.code) and looks for <key> in the cache twice a second. This loop picks up new dumps, translates
every one found in a batch with one run of nxnative's translator (dmsh_xlate: Ryujinx's shader translator, which reads
Switch 2 Ampere code itself), applies the same edits and reflection as nxshader.py and publishes into the cache. The game
never waits: a draw whose shader is not there yet is skipped until it arrives.

  nxs_live.py --dump DIR --cache DIR [--xlate DIR] [--dotnet dotnet] [--facts shader_facts.tsv] [--parent PID]"""
import argparse, json, os, shutil, struct, sys, tempfile, time, traceback

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import nxs_front as F, nxs_cache as C, nxshader as N

SPH_STAGE = {1: 'vs', 2: 'hs', 3: 'ds', 4: 'gs', 5: 'ps'}


class ParentProcess:
    """Hold the Windows process handle so PID reuse cannot extend a helper's lifetime."""
    def __init__(self, pid):
        self.pid, self.handle = pid, None
        if pid and os.name == 'nt':
            import ctypes
            from ctypes import wintypes
            self.kernel = ctypes.WinDLL('kernel32', use_last_error=True)
            self.kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
            self.kernel.OpenProcess.restype = wintypes.HANDLE
            self.kernel.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
            self.kernel.WaitForSingleObject.restype = wintypes.DWORD
            self.kernel.CloseHandle.argtypes = [wintypes.HANDLE]
            self.kernel.CloseHandle.restype = wintypes.BOOL
            self.handle = self.kernel.OpenProcess(0x00100000, False, pid)  # SYNCHRONIZE only

    def alive(self):
        if not self.pid: return True
        if os.name == 'nt':
            return bool(self.handle) and self.kernel.WaitForSingleObject(self.handle, 0) == 258
        try: os.kill(self.pid, 0)
        except ProcessLookupError: return False
        except PermissionError: return True
        return True

    def close(self):
        if self.handle:
            self.kernel.CloseHandle(self.handle)
            self.handle = None


def stage_of(ctl, code):
    """nxnative shader_split.py nvn2_stage / sph_stage (MIT): compute, task and mesh by the control's stage byte (+0x56),
    else the graphics SPH word 0 of the code area"""
    if F.dmsh_ampere.is_nvn2(ctl) and len(ctl) > 0x56 and ctl[0x56] in (5, 6, 7): return {5: 'cs', 6: 'ts', 7: 'ms'}[ctl[0x56]]
    if len(code) < 4: return None
    w = struct.unpack_from('<I', code, 0)[0] if F.dmsh_ampere.is_nvn2(ctl) else (struct.unpack_from('<I', code, 0x30)[0] if len(code) >= 0x34 else 0)
    if (w >> 10) & 0xf == 0 and w & 0x1f == 1: return 'vs'
    return SPH_STAGE.get((w >> 10) & 0xf) or ('cs' if not F.dmsh_ampere.is_nvn2(ctl) and code[:4] == bytes.fromhex('79563412') else None)


class DumpCorpus:
    """nxs_front.Corpus over the runtime's dumps: a record is 0x38 header bytes, the code area, the control section"""
    def __init__(self, dump, facts_path=None):
        self.dump = dump
        self.facts_raw, self.facts, self.source = {}, {}, {}
        if facts_path and os.path.isfile(facts_path):
            for l in open(facts_path, encoding='utf-8'):
                if l.strip() and not l.startswith('#'):
                    k, v = l.rstrip('\r\n').split('\t', 1)
                    self.facts_raw[k] = v; self.facts[k] = json.loads(v)
        self.stage = {}

    def record(self, key):
        ctl = open(os.path.join(self.dump, key + '.ctl'), 'rb').read()
        code = open(os.path.join(self.dump, key + '.code'), 'rb').read()
        if F.dmsh_ampere.is_nvn2(ctl): ctl = ctl[:0xe0]
        st = stage_of(ctl, code)
        if not st: raise ValueError('a shader of no known stage')
        self.stage[key] = st
        return st + '_live.bin', bytes(0x38) + code + ctl


def is_current(cache, chain, key):
    """the cache has this shader, made by the current chain. Asked per dumped key: a build-time cache (nxs_prebuild.py) holds tens of thousands of
    entries, and reading every one of them at each start of the game cost its first seconds"""
    try:
        hit = cache.lookup(key, chain)
        return bool(hit and len(hit[0]) >= 20 and len(hit[0]) % 4 == 0)
    except (OSError, ValueError): return False   # interrupted publication is a miss, not a completed shader


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dump', required=True); ap.add_argument('--cache', required=True)
    ap.add_argument('--xlate', default=os.path.join(HERE, 'xlate_bin')); ap.add_argument('--dotnet', default='dotnet')
    ap.add_argument('--facts'); ap.add_argument('--parent', type=int, default=0); ap.add_argument('--batch', type=int, default=48)
    ap.add_argument('--once', action='store_true', help='translate the current dump set and exit')
    ap.add_argument('--keys', help='a file of keys, one a line: only these dumps (nxs_prebuild.py runs several of these side by side)')
    ap.add_argument('--progress', action='store_true', help='print "progress <translated> <failed>" after each batch')
    ap.add_argument('--chain', action='store_true', help='print the chain id of these tools and exit')
    ap.add_argument('--part', default='0/1', help='I/N: this is translator I of N running side by side on one dump folder (each takes the keys that are I modulo N)')
    a = ap.parse_args()
    if a.batch < 1: ap.error('--batch must be positive')
    try: part, parts = [int(x) for x in a.part.split('/')]
    except ValueError: ap.error('--part is I/N')
    if not 0 <= part < parts: ap.error('--part is I/N with I < N')
    os.makedirs(a.dump, exist_ok=True); os.makedirs(os.path.join(a.cache, 'vk'), exist_ok=True)
    log = open(os.path.join(a.cache, 'nxs_live.log'), 'a', buffering=1)
    say = lambda s: log.write(time.strftime('%H:%M:%S ') + s + '\n')
    try: xl = F.Translator(a.xlate, a.dotnet)
    except Exception as e: say('no translator: %s (.NET 10 runtime installed?)' % e); return 1
    cache = C.Cache(a.cache)
    w = tempfile.mkdtemp(prefix='nxs_live_')
    b = N.Builder(DumpCorpus(a.dump, a.facts), cache, xl, w, validate=False)
    cache.set_chain(b.chain)
    if a.chain:
        print(b.chain); shutil.rmtree(w, ignore_errors=True); log.close(); return 0
    say('ready: translator %s, chain %s, dumps %s' % (xl.id, b.chain, a.dump))
    done, failed, seen = set(), set(), set()
    only = None
    if a.keys:
        with open(a.keys) as f: only = [l.strip() for l in f if l.strip()]
    nok = 0
    parent = ParentProcess(a.parent)
    try:
        while True:
            if not parent.alive(): break
            names = os.listdir(a.dump) if only is None else []
            keys = only if only is not None else sorted(f[:-4] for f in names if f.endswith('.ctl') and len(f) == 20 and int(f[:8], 16) % parts == part)
            # <key>.now: a draw is waiting for this one (nxs_rt.c); the others were dumped when the game loaded them and may never be drawn
            urgent = {f[:-4] for f in names if f.endswith('.now')}
            for k in keys:
                if k not in seen:
                    seen.add(k)
                    if is_current(cache, b.chain, k): done.add(k)
            todo = [k for k in keys if k not in done and k not in failed and os.path.exists(os.path.join(a.dump, k + '.code'))]
            if not todo:
                if a.once: break
                time.sleep(0.05); continue
            time.sleep(0.03)   # the shaders of one pipeline arrive together: one translator run for them
            if urgent: todo.sort(key=lambda k: k not in urgent)
            if only is None and not a.once: todo = todo[:a.batch]   # one batch, then look again: a shader a draw waits for goes first
            for i in range(0, len(todo), a.batch):
                if not parent.alive(): break
                chunk = todo[i:i + a.batch]; t0 = time.time()
                try: out = b.make(chunk)
                except Exception: say(traceback.format_exc()); out = {k: {'status': 'failed'} for k in chunk}
                ok = [k for k, r in out.items() if r.get('status') == 'ok']
                for k, r in out.items():
                    if r.get('status') == 'ok': done.add(k)
                    else: failed.add(k); say('%s: %s %s' % (k, r.get('status'), r.get('error', '')))
                say('%d translated in %.2f s (%d failed)' % (len(ok), time.time() - t0, len(chunk) - len(ok)))
                nok += len(ok)
                if a.progress: print('progress %d %d' % (nok, len(failed)), flush=True)
            if a.once: break
    finally:
        parent.close()
        shutil.rmtree(w, ignore_errors=True)
        log.close()
    return 1 if a.once and failed else 0


if __name__ == '__main__':
    sys.exit(main())
