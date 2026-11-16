# SwitchRecomp - Copyright 2026 MZ Labs
# Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
# Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen.
"""Bounded Pokemon Legends Z-A runs (docs/PLZA_BLACK_SCREEN_INVESTIGATION.md), the mkw_probe.py pattern.

python tools/plza_probe.py start baseline --game "D:/path/to/game folder" --seconds 300 [--verbose] [--stacks]
python tools/plza_probe.py action RUN a --observed "language selection"      (a b x y lr plus up down left right)
python tools/plza_probe.py action RUN shot --observed "after confirming"
python tools/plza_probe.py action RUN stop --observed "done"
python tools/plza_probe.py report RUN

The worker starts the game windowed (--fullscreen=0, so the ini is not touched), with the runtime's own screenshot
request file (NXRT_VK_SHOT_FILE) and scripted input (SWR_INPUT_FILE). It writes, in build/plza-investigation/<run>/:
  NNNN-<name>.png      runtime screenshots: every --shot-every seconds, after each action, at each stall
  progress.jsonl       every 5 s: frames presented, FPS, texture count/MB, stall warnings so far (from the log)
  stall-N-threads.txt  threads.txt as it was at the N-th "s without a new image" warning, with the log's dump block
  stall-N-stacks.txt   with --stacks: native stacks of the game (cdb -pv), runtime frames resolved with nm
  actions.jsonl, provenance.json (hashes), run.log, threads.txt, result.json
It only ever stops processes it started (the startup relaunch child is verified by parent, path and start time).
"""
import argparse
import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
RUNS = ROOT / 'build/plza-investigation'
BUTTONS = dict(a=1, b=2, x=4, y=8, lr=0xc0, plus=0x400, up=0x2000, down=0x8000, left=0x1000, right=0x4000)
CDB = Path(r'C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe')
STALL = re.compile(r'^\[\s*([\d.]+)\] warning: (\d+) s without a new image \(frames=(\d+), GPU submits=(\d+)\)')
TEXTURES = re.compile(r'\[vk\] textures: (\d+) images, (\d+) MB')
PACING = re.compile(r'\[vk\] pacing: ([\d.]+) FPS')
FRAMES = re.compile(r'frame pacing up to frame (\d+)')


def atomic(path, text):
    tmp = path.with_suffix(path.suffix + '.tmp')
    tmp.write_text(text, encoding='utf-8')
    os.replace(tmp, path)


def sha(path):
    with path.open('rb') as f: return hashlib.file_digest(f, 'sha256').hexdigest()


def game_files(game):
    exes = [p for p in Path(game).glob('*.exe') if p.with_name(p.stem + '_code.dll').exists()]
    if len(exes) != 1: raise SystemExit(f'{game}: expected one <Game>.exe next to its <Game>_code.dll, found {len(exes)}')
    exe = exes[0]
    return exe, exe.with_suffix('.log')


def processes(name):
    """[{ProcessId, ParentProcessId, ExecutablePath, created}] of every process with that image name (read only)"""
    q = (f'Get-CimInstance Win32_Process -Filter "Name=\'{name.replace(chr(39), chr(39) * 2)}\'" | '
         'Select-Object ProcessId,ParentProcessId,ExecutablePath,'
         "@{Name='created';Expression={([DateTimeOffset]$_.CreationDate).ToUnixTimeMilliseconds()/1000.0}} | ConvertTo-Json -Compress")
    text = subprocess.check_output(['pwsh', '-NoProfile', '-Command', q], text=True, timeout=20).strip()
    rows = json.loads(text) if text else []
    return rows if isinstance(rows, list) else [rows]


def png_complete(path):
    try:
        with path.open('rb') as f:
            if f.read(8) != b'\x89PNG\r\n\x1a\n': return False
            f.seek(-12, 2)
            return f.read() == bytes.fromhex('0000000049454e44ae426082')
    except (OSError, ValueError): return False


class OwnedHandle:
    """A verified process, held by handle so a reused PID can never be closed by mistake (mkw_probe.py)."""
    def __init__(self, pid):
        self.pid = pid; self.returncode = None
        self.k = ctypes.WinDLL('kernel32', use_last_error=True)
        self.k.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]; self.k.OpenProcess.restype = wintypes.HANDLE
        self.k.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
        self.k.GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
        self.k.TerminateProcess.argtypes = [wintypes.HANDLE, wintypes.UINT]
        self.k.CloseHandle.argtypes = [wintypes.HANDLE]
        self.handle = self.k.OpenProcess(0x100000 | 0x1000 | 1, False, pid)
        if not self.handle: raise ctypes.WinError(ctypes.get_last_error())
    def poll(self):
        if self.k.WaitForSingleObject(self.handle, 0) == 0:
            code = wintypes.DWORD(); self.k.GetExitCodeProcess(self.handle, ctypes.byref(code)); self.returncode = code.value
        return self.returncode
    def wait(self, timeout):
        if self.k.WaitForSingleObject(self.handle, int(timeout * 1000)) != 0: raise subprocess.TimeoutExpired(str(self.pid), timeout)
        return self.poll()
    def terminate(self): self.k.TerminateProcess(self.handle, 1)
    def close(self): self.k.CloseHandle(self.handle)


def close_owned(proc):
    """WM_CLOSE to the process's windows, then terminate after 10 s"""
    user = ctypes.WinDLL('user32')
    callback = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    user.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    user.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
    def each(hwnd, unused):
        pid = wintypes.DWORD(); user.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        if pid.value == proc.pid: user.PostMessageW(hwnd, 0x10, 0, 0)
        return True
    user.EnumWindows(callback(each), 0)
    try: proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        proc.terminate(); proc.wait(timeout=10)
        return 'terminated after WM_CLOSE timeout'
    return 'WM_CLOSE'


def owned_children(exe, root_pid, started):
    """the startup relaunch (plat_identity_early) and anything it relaunched: same executable, our descendants,
    created within 10 s of our launch"""
    rows = processes(exe.name); known = {root_pid}; found = []
    for _ in range(16):
        added = False
        for r in rows:
            if (r['ProcessId'] in known or r['ParentProcessId'] not in known or not started - 1 <= r['created'] <= started + 10
                    or Path(r.get('ExecutablePath') or '').resolve() != exe.resolve()): continue
            known.add(r['ProcessId']); found.append(r); added = True
        if not added: break
    return found


def game_pid(exe, proc, started):
    """the process that runs the game: the deepest owned relaunch, or the one we started"""
    kids = owned_children(exe, proc.pid, started)
    return kids[-1]['ProcessId'] if kids else proc.pid


def native_stacks(pid, out):
    """cdb -pv (non-invasive: the game keeps running) stacks of every thread, runtime offsets resolved with nm"""
    if not CDB.exists(): atomic(out, 'cdb.exe not installed (Windows SDK debugging tools)\n'); return
    raw = subprocess.run([str(CDB), '-pv', '-p', str(pid), '-c', '~*kn 16; qd'], capture_output=True, text=True, timeout=120).stdout
    nm = ROOT / 'build/release-deps/gcc/bin/x86_64-w64-mingw32-nm.exe'
    for cand in (nm, Path(os.environ.get('SWR_NM', '')), Path(r'D:\Projects\RecompLauncher\third_party\gcc\Library\bin\x86_64-w64-mingw32-nm.exe')):
        if cand and cand.is_file(): nm = cand; break
    syms = []
    if nm.is_file():
        for line in subprocess.run([str(nm), '-n', str(ROOT / 'bin/runtime_win64_win32.exe')], capture_output=True, text=True).stdout.splitlines():
            p = line.split()
            if len(p) == 3 and p[1] in 'tT': syms.append((int(p[0], 16), p[2]))
    def sym(off):
        a = 0x140000000 + off; lo, hi = 0, len(syms) - 1
        if not syms or a < syms[0][0]: return f'+{off:#x}'
        while lo < hi:
            m = (lo + hi + 1) // 2
            if syms[m][0] <= a: lo = m
            else: hi = m - 1
        return f'{syms[lo][1]}+{a - syms[lo][0]:#x}'
    exe_frame = re.compile(r'(\S+?)(?<!_code)\+0x([0-9a-f]+)$')
    lines = []
    for line in raw.splitlines():
        m = re.match(r'^([0-9a-f]{2}) \S+ \S+ (.*)$', line)
        if m:
            site = m.group(2)
            e = exe_frame.match(site)
            if e and not e.group(1).endswith('_code') and '!' not in e.group(1): site = sym(int(e.group(2), 16))
            elif '_code!' in site: site = 'game code'
            line = f'{m.group(1)} {site}'
        lines.append(line)
    atomic(out, '\n'.join(lines) + '\n')


def ctl(command):
    """one command on the runtime's control pipe (runtime/nvn/vk/vk_ctl.c), its reply; errors come back as text"""
    k = ctypes.WinDLL('kernel32', use_last_error=True)
    k.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
    k.CreateFileW.restype = wintypes.HANDLE
    k.WriteFile.argtypes = k.ReadFile.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
    k.CloseHandle.argtypes = [wintypes.HANDLE]
    end = time.monotonic() + 5
    while True:
        h = k.CreateFileW(r'\\.\pipe\swr_ctl', 0xC0000000, 0, None, 3, 0, None)   # GENERIC_READ|WRITE, OPEN_EXISTING
        if h and h != wintypes.HANDLE(-1).value: break
        if time.monotonic() > end: return f'control pipe not available (error {ctypes.get_last_error()})'
        time.sleep(.2)
    try:
        data = (command + '\n').encode(); n = wintypes.DWORD()
        if not k.WriteFile(h, data, len(data), ctypes.byref(n), None): return f'write failed (error {ctypes.get_last_error()})'
        out = b''; buf = ctypes.create_string_buffer(65536)
        while k.ReadFile(h, buf, len(buf), ctypes.byref(n), None) and n.value: out += buf.raw[:n.value]
        return out.decode(errors='replace')
    finally: k.CloseHandle(h)


def worker(run):
    config = json.loads((run / 'config.json').read_text())
    exe, log = game_files(config['game'])
    if processes(exe.name): raise RuntimeError(f'{exe.name} is already running; refused launch')
    files = [exe, exe.with_name(exe.stem + '_code.dll'), ROOT / 'bin/runtime_win64_win32.exe', Path(__file__).resolve()]
    atomic(run / 'provenance.json', json.dumps({str(p): sha(p) for p in files if p.is_file()}, indent=2))
    if log.exists(): shutil.copy2(log, run / 'prior-run.log')
    env = dict(os.environ, NXRT_VK_SHOT_FILE=str(run / 'shot-request.txt'), NXRT_VK_SHOT_EVERY='0', NXRT_VK_SHOT_WINDOW='0',
               SWR_INPUT_FILE=str(run / 'input.txt'), SWR_INPUT_EXCLUSIVE='1')
    env.update(config['env'])
    args = [str(exe), '--fullscreen=0'] + (['-v'] if config['verbose'] else [])
    proc = subprocess.Popen(args, cwd=exe.parent, env=env, stdout=(run / 'stdout.log').open('wb'), stderr=subprocess.STDOUT)
    started = time.time(); start = time.monotonic(); deadline = start + config['seconds']
    atomic(run / 'session.json', json.dumps(dict(pid=proc.pid, started=started, args=args)))
    count = 0; seen = 0; stalls = 0; latest = {}
    def record(**fields):
        fields['elapsed'] = round(time.monotonic() - start, 3)
        with (run / 'actions.jsonl').open('a', encoding='utf-8') as f: f.write(json.dumps(fields) + '\n')
    def wait(test, seconds=15):
        end = min(deadline, time.monotonic() + seconds)
        while not test():
            if proc.poll() is not None: raise RuntimeError(f'game exited {proc.returncode:#x}')
            if time.monotonic() >= end: raise TimeoutError('request did not complete')
            time.sleep(.025)
    def shot(name):
        nonlocal count
        count += 1
        path = run / f'{count:04d}-{name}.png'; req = run / 'shot-request.txt'
        if req.exists(): raise RuntimeError('pending screenshot request; refusing overwrite')
        atomic(req, str(path))
        try: wait(lambda: png_complete(path), 10)
        except TimeoutError:   # a stalled game presents nothing, so the request never completes
            if req.exists(): req.unlink()
            record(screenshot=None, name=name, note='no present within 10 s')
            return None
        record(screenshot=path.name, name=name)
        return path.name
    def scan_log():
        """new log lines: progress counters and stall warnings"""
        nonlocal seen, stalls
        if not log.exists(): return
        with log.open('r', encoding='utf-8', errors='replace') as f:
            f.seek(seen); chunk = f.read(); seen = f.tell()
        for line in chunk.splitlines():
            if (m := TEXTURES.search(line)): latest.update(textures=int(m.group(1)), texture_mb=int(m.group(2)))
            if (m := PACING.search(line)): latest['fps'] = float(m.group(1))
            if (m := FRAMES.search(line)): latest['frames'] = int(m.group(1))
            if (m := STALL.match(line)):
                stalls += 1; t = m.group(1)
                latest.update(stalled_at=float(t), stalled_frames=int(m.group(3)), stalls=stalls)
                block = [l for l in chunk.splitlines() if l.startswith(f'[{t.rjust(7)}]') or l.startswith(f'[ {t}]')]
                tt = exe.parent / 'threads.txt'
                text = (tt.read_text(errors='replace') if tt.exists() else '(no threads.txt)\n') + '\n---- log dump block ----\n' + '\n'.join(block) + '\n'
                atomic(run / f'stall-{stalls}-threads.txt', text)
                record(stall=stalls, at=float(t), frames=int(m.group(3)))
                if stalls <= 2:
                    shot(f'stall{stalls}')
                    if config['stacks']: native_stacks(game_pid(exe, proc, started), run / f'stall-{stalls}-stacks.txt')
    reason = 'natural exit'; next_shot = start + 5; next_progress = start + 5
    try:
        while proc.poll() is None and time.monotonic() < deadline:
            cmd = run / 'command.json'
            if cmd.exists():
                request = json.loads(cmd.read_text()); cmd.unlink()
                action, ident = request['action'], request['id']
                record(**request)
                if action == 'stop':
                    reason = 'requested stop'; atomic(run / f'{ident}.done.json', json.dumps(dict(stopping=True))); break
                if action == 'ctl':
                    reply = ctl(request['ctl'])
                    record(ctl=request['ctl'], reply=reply.strip()[:2000])
                    time.sleep(request['settle'])
                    image = shot('ctl-' + re.sub(r'[^A-Za-z0-9]+', '_', request['ctl'])[:40])
                    atomic(run / f'{ident}.done.json', json.dumps(dict(screenshot=image, reply=reply.strip()[:2000])))
                    next_shot = time.monotonic() + config['shot_every']
                    continue
                if action in BUTTONS:
                    inp = run / 'input.txt'
                    if inp.exists(): raise RuntimeError('pending input request; refusing overwrite')
                    atomic(inp, f'{ident} {BUTTONS[action]:x} {request["hold_ms"]}')
                    wait(lambda: (run / 'input.txt.ack').exists() and (run / 'input.txt.ack').read_text().strip() == ident)
                    time.sleep(request['hold_ms'] / 1000 + request['settle'])
                image = shot(f'{action}-settled')
                atomic(run / f'{ident}.done.json', json.dumps(dict(screenshot=image)))
                next_shot = time.monotonic() + config['shot_every']
            elif config['shot_every'] and time.monotonic() >= next_shot:
                shot('periodic'); next_shot = time.monotonic() + config['shot_every']
            if time.monotonic() >= next_progress:
                scan_log()
                with (run / 'progress.jsonl').open('a', encoding='utf-8') as f:
                    f.write(json.dumps(dict(elapsed=round(time.monotonic() - start, 1), **latest)) + '\n')
                next_progress = time.monotonic() + 5
            time.sleep(.05)
        if proc.poll() is None and time.monotonic() >= deadline: reason = 'time limit'
    except Exception as e:
        reason = f'{type(e).__name__}: {e}'; record(error=reason)
    finally:
        records = []
        try:
            for row in reversed(owned_children(exe, proc.pid, started)):
                child = OwnedHandle(row['ProcessId'])
                try:
                    row['shutdown'] = close_owned(child) if child.poll() is None else 'already exited'
                    row['exit_code'] = child.returncode; records.append(row)
                finally: child.close()
        except Exception as e: records.append(dict(error=f'could not verify startup children: {e}'))
        parent = OwnedHandle(proc.pid)
        try: records.append(dict(ProcessId=proc.pid, shutdown=close_owned(parent) if parent.poll() is None else 'already exited'))
        finally: parent.close()
        atomic(run / 'owned-processes.json', json.dumps(records, indent=2))
        scan_log()
        if log.exists(): shutil.copy2(log, run / 'run.log')
        for name in ('threads.txt', 'performance.txt'):
            if (exe.parent / name).exists(): shutil.copy2(exe.parent / name, run / name)
        atomic(run / 'result.json', json.dumps(dict(reason=reason, elapsed=round(time.monotonic() - start, 1), **latest), indent=2))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('start'); p.add_argument('label'); p.add_argument('--game', required=True, help='folder with <Game>.exe and <Game>_code.dll')
    p.add_argument('--seconds', type=int, default=300); p.add_argument('--env', action='append', default=[], help='NAME=VALUE for the game')
    p.add_argument('--shot-every', type=float, default=10, help='seconds between periodic screenshots, 0 = none')
    p.add_argument('--verbose', action='store_true', help='runtime -v (log=2: IPC, thread starts, core masks)')
    p.add_argument('--stacks', action='store_true', help='native stacks with cdb at the first two stalls')
    p.add_argument('--wait', action='store_true', help='block until the run ends, then print the report')
    p = sub.add_parser('worker'); p.add_argument('run', type=Path)
    p = sub.add_parser('report'); p.add_argument('run', type=Path)
    p = sub.add_parser('action'); p.add_argument('run', type=Path); p.add_argument('action', choices=[*BUTTONS, 'shot', 'ctl', 'stop'])
    p.add_argument('--observed', required=True); p.add_argument('--hold-ms', type=int, default=200); p.add_argument('--settle', type=float, default=1.5)
    p.add_argument('--ctl', default='', help='with action ctl: a control pipe command, e.g. "set a2c 0", "texat 34a000000", "flight"')
    a = ap.parse_args()
    if a.cmd == 'worker': return worker(a.run.resolve())
    if a.cmd == 'report': return report(a.run.resolve())
    if a.cmd == 'start':
        exe, _ = game_files(a.game)
        if processes(exe.name): ap.error(f'{exe.name} is already running')
        if not 10 <= a.seconds <= 3600: ap.error('seconds must be 10..3600')
        RUNS.mkdir(parents=True, exist_ok=True)
        run = RUNS / (time.strftime('%Y%m%d-%H%M%S-') + a.label); run.mkdir()
        env = dict(x.split('=', 1) for x in a.env)
        atomic(run / 'config.json', json.dumps(dict(game=str(Path(a.game).resolve()), seconds=a.seconds, env=env,
               shot_every=a.shot_every, verbose=a.verbose, stacks=a.stacks), indent=2))
        w = subprocess.Popen([sys.executable, str(Path(__file__).resolve()), 'worker', str(run)],
                             stdout=(run / 'harness.log').open('wb'), stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW)
        print(run, flush=True)
        if a.wait:
            w.wait(); report(run)
        return
    run = a.run.resolve()
    if (run / 'result.json').exists(): ap.error('run has ended')
    if (run / 'command.json').exists(): ap.error('a command is pending')
    ident = str(time.time_ns())
    if a.action == 'ctl' and not a.ctl: ap.error('action ctl needs --ctl "<command>"')
    atomic(run / 'command.json', json.dumps(dict(id=ident, action=a.action, observed_before=a.observed, hold_ms=a.hold_ms, settle=a.settle, ctl=a.ctl)))
    end = time.monotonic() + 45; done = run / f'{ident}.done.json'
    while not done.exists() and time.monotonic() < end:
        if (run / 'result.json').exists(): raise RuntimeError((run / 'result.json').read_text())
        time.sleep(.1)
    if not done.exists(): raise TimeoutError('action not acknowledged; do not repeat blindly')
    print(done.read_text())


def report(run):
    result = json.loads((run / 'result.json').read_text()) if (run / 'result.json').exists() else {'reason': 'still running'}
    print(json.dumps(result, indent=2))
    prog = (run / 'progress.jsonl').read_text().splitlines() if (run / 'progress.jsonl').exists() else []
    for line in prog[-3:]: print('progress', line)
    for f in sorted(run.glob('stall-*')): print('stall file', f.name)
    shots = sorted(run.glob('*.png'))
    print(f'{len(shots)} screenshots; latest:', shots[-1] if shots else 'none')


if __name__ == '__main__': main()
