# SwitchRecomp - Copyright 2026 MZ Labs
# Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
# Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen.
"""Bounded MKW investigation. Start, inspect PNGs, then issue one action with an observed screen.

python tools/mkw_probe.py start baseline --seconds 180
python tools/mkw_probe.py action RUN lr --observed "title: L+R prompt"
python tools/mkw_probe.py action RUN shot --observed "character selection"
python tools/mkw_probe.py action RUN stop --observed "blocked at loading"
The worker owns its Popen handle and verified same-executable startup relaunches.
"""
import argparse
import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
import struct
import zlib

ROOT = Path(__file__).resolve().parents[1]
GAME = ROOT / 'build/mkw-nsp-1.8'
EXE = GAME / 'Mario Kart World 180.exe'
LOG = EXE.with_suffix('.log')
BUTTONS = dict(a=1, b=2, x=4, y=8, lr=0xc0, plus=0x400,
               up=0x2000, down=0x8000, left=0x1000, right=0x4000)


def atomic(path, text):
    tmp = path.with_suffix('.tmp')
    tmp.write_text(text, encoding='utf-8')
    os.replace(tmp, path)


def sha(path):
    return hashlib.file_digest(path.open('rb'), 'sha256').hexdigest()


def game_pids():
    # query exact executable name, without touching any existing process
    text = subprocess.check_output(['tasklist', '/FI', f'IMAGENAME eq {EXE.name}', '/FO', 'CSV', '/NH'], text=True)
    return text if EXE.name.lower() in text.lower() else ''


def audit(run, cache_root=None):
    files = [EXE, ROOT / 'bin/runtime_win64_win32.exe', Path(__file__).resolve(), ROOT / 'tools/mkw_perf.py']
    for base in (ROOT, GAME):
        files += sorted((base / 'tools/nxshader').glob('*.py'))
        files += sorted((base / 'tools/nxshader/xlate_bin').glob('*.dll'))
    hashes = {str(p): sha(p) for p in files if p.is_file()}
    cache = Path(cache_root) / 'vk' if cache_root else GAME / 'Mario Kart World 180_nvn/cache/vk'
    hashes['effective_cache'] = str(cache)
    hashes['cache'] = {p.name: sha(p) for p in cache.glob('*') if p.is_file()}
    atomic(run / 'provenance.json', json.dumps(hashes, indent=2))
    for p in (LOG, GAME / 'Mario Kart World 180_nvn/cache/nxs_live.log'):
        if p.exists(): shutil.copy2(p, run / ('prior-' + p.name))


def png_complete(path):
    try:
        with path.open('rb') as f:
            if f.read(8) != b'\x89PNG\r\n\x1a\n': return False
            f.seek(-12, 2)
            return f.read() == bytes.fromhex('0000000049454e44ae426082')
    except (OSError, ValueError): return False


def ui_mask(path, box, color):
    data = path.read_bytes(); at = 8; packed = bytearray(); w = h = 0
    while at < len(data):
        n = struct.unpack_from('>I', data, at)[0]; kind = data[at+4:at+8]; part = data[at+8:at+8+n]; at += n+12
        if kind == b'IHDR':
            w,h,depth,ct = struct.unpack_from('>IIBB', part)
            if (depth,ct)!=(8,6): raise ValueError('expected runtime RGBA8 PNG')
        if kind == b'IDAT': packed.extend(part)
    raw = zlib.decompress(packed); stride = w*4+1
    if len(raw)!=stride*h or any(raw[y*stride] for y in range(h)): raise ValueError('expected runtime unfiltered PNG')
    mask = set()
    x0,y0,x1,y1 = box
    for y in range(y0,y1,2):
        for x in range(x0,x1,2):
            r,g,b = raw[y*stride+1+x*4:y*stride+4+x*4]
            hit = (r>160 and g>115 and b<110) if color=='yellow' else (r<90 and g<125 and b>110) if color=='blue' else min(r,g,b)>210 and max(r,g,b)-min(r,g,b)<35
            if hit: mask.add((x,y))
    return mask


def close_owned(proc):
    user = ctypes.WinDLL('user32')
    callback = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    user.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    user.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
    def each(hwnd, unused):
        pid = wintypes.DWORD()
        user.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        if pid.value == proc.pid: user.PostMessageW(hwnd, 0x10, 0, 0)
        return True
    user.EnumWindows(callback(each), 0)
    try: proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        proc.terminate()
        proc.wait(timeout=10)
        return 'terminated after WM_CLOSE timeout'
    return 'WM_CLOSE'


class OwnedHandle:
    """An already verified descendant, held by handle so PID reuse cannot redirect a stop."""
    def __init__(self, pid):
        self.pid = pid; self.returncode = None
        self.k = ctypes.WinDLL('kernel32', use_last_error=True)
        self.k.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        self.k.OpenProcess.restype = wintypes.HANDLE
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
        if self.k.WaitForSingleObject(self.handle, int(timeout*1000)) != 0: raise subprocess.TimeoutExpired(str(self.pid), timeout)
        return self.poll()
    def terminate(self): self.k.TerminateProcess(self.handle, 1)
    def close(self): self.k.CloseHandle(self.handle)
    def created(self):
        times = [wintypes.FILETIME() for _ in range(4)]
        self.k.GetProcessTimes.argtypes = [wintypes.HANDLE] + [ctypes.POINTER(wintypes.FILETIME)]*4
        if not self.k.GetProcessTimes(self.handle, *[ctypes.byref(t) for t in times]): raise ctypes.WinError(ctypes.get_last_error())
        return ((times[0].dwHighDateTime << 32) | times[0].dwLowDateTime)/1e7-11644473600


def recover_child(run, pid):
    session = json.loads((run/'session.json').read_text())
    query = (f'$p=Get-CimInstance Win32_Process -Filter "ProcessId={pid}"; '
             '$p | Select-Object ProcessId,ParentProcessId,ExecutablePath,CommandLine | ConvertTo-Json -Compress')
    info = json.loads(subprocess.check_output(['pwsh', '-NoProfile', '-Command', query], text=True))
    if info['ParentProcessId'] != session['pid'] or Path(info['ExecutablePath']).resolve() != EXE.resolve():
        raise RuntimeError('process is not a direct same-executable child of this owned launch')
    proc = OwnedHandle(pid)
    try:
        created = proc.created()
        if not session['started']-1 <= created <= session['started']+10:
            raise RuntimeError('child creation does not match the owned startup relaunch')
        info.update(created=created, shutdown=close_owned(proc), exit_code=proc.returncode)
        atomic(run/f'owned-child-{pid}.json', json.dumps(info, indent=2))
        print(json.dumps(info, indent=2))
    finally: proc.close()


def startup_children(rows, root_pid, started):
    known = {root_pid}; found = []
    for _ in range(16):
        added = False
        for row in rows:
            if (row['ProcessId'] in known or row['ParentProcessId'] not in known or
                    not started-1 <= row['created'] <= started+10 or
                    Path(row.get('ExecutablePath') or '').resolve() != EXE.resolve()): continue
            known.add(row['ProcessId']); found.append(row); added = True
        if not added: break
    return found


def close_family(proc, started, run):
    # plat_identity_early may restart the same executable and wait for its child.
    # Closing only the Popen PID then leaves the actual game window alive.
    name = EXE.name.replace("'", "''")
    query = (f'Get-CimInstance Win32_Process -Filter "Name=\'{name}\'" | '
             'Select-Object ProcessId,ParentProcessId,ExecutablePath,'
             "@{Name='created';Expression={([DateTimeOffset]$_.CreationDate).ToUnixTimeMilliseconds()/1000.0}} | ConvertTo-Json -Compress")
    records = []
    try:
        text = subprocess.check_output(['pwsh', '-NoProfile', '-Command', query], text=True, timeout=10).strip()
        rows = json.loads(text) if text else []
        for row in reversed(startup_children(rows if isinstance(rows,list) else [rows], proc.pid, started)):
            child = OwnedHandle(row['ProcessId'])
            try:
                # Recheck creation through the held handle before any window message.
                if abs(child.created()-row['created']) > .01: raise RuntimeError('child PID was reused')
                row['shutdown'] = close_owned(child) if child.poll() is None else 'already exited'
                row['exit_code'] = child.returncode; records.append(row)
            finally: child.close()
    except Exception as e:
        records.append(dict(error=f'could not verify startup descendants: {e}'))
    shutdown = close_owned(proc) if proc.poll() is None else 'already exited'
    records.append(dict(ProcessId=proc.pid, shutdown=shutdown, exit_code=proc.returncode))
    atomic(run/'owned-processes.json', json.dumps(records, indent=2))
    return shutdown


def worker(run):
    config = json.loads((run / 'config.json').read_text())
    if game_pids(): raise RuntimeError('an existing game process is running; refused launch')
    audit(run, config['env'].get('NXRT_SHADER_CACHE'))
    env = dict(os.environ, SWR_PYTHON=sys.executable, NXRT_VK_SHOT_FILE=str(run / 'shot-request.txt'),
               NXRT_VK_SHOT_EVERY='0', NXRT_VK_SHOT_WINDOW='0', SWR_INPUT_FILE=str(run / 'input.txt'),
               SWR_NVN_DIAGNOSTIC_PREFIX=str(run / 'diagnostic'), SWR_INPUT_EXCLUSIVE='1')
    env['PATH'] = str(ROOT / 'dotnet') + os.pathsep + env.get('PATH', '')
    env.update(config['env'])
    proc = subprocess.Popen([str(EXE), '--fullscreen=0'], cwd=GAME, env=env,
                            stdout=(run / 'stdout.log').open('wb'), stderr=subprocess.STDOUT)
    start = time.monotonic()
    deadline = start + config['seconds']
    atomic(run / 'session.json', json.dumps(dict(pid=proc.pid, started=time.time())))
    count = 0
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
        path = run / f'{count:04d}-{name}.png'
        req = run / 'shot-request.txt'
        if req.exists(): raise RuntimeError('pending screenshot request; refusing overwrite')
        atomic(req, str(path))
        wait(lambda: png_complete(path))
        record(screenshot=path.name, observed='unreviewed')
        return path.name
    reason = 'natural exit'
    next_shot = start + 5
    try:
        while proc.poll() is None and time.monotonic() < deadline:
            cmd = run / 'command.json'
            if cmd.exists():
                request = json.loads(cmd.read_text())
                cmd.unlink()
                action, ident = request['action'], request['id']
                record(**request)
                if action == 'stop':
                    reason = 'requested stop'
                    atomic(run / f'{ident}.done.json', json.dumps(dict(stopping=True)))
                    break
                if action in BUTTONS:
                    inp = run / 'input.txt'
                    if inp.exists(): raise RuntimeError('pending input request; refusing overwrite')
                    atomic(inp, f'{ident} {BUTTONS[action]:x} {request["hold_ms"]}')
                    wait(lambda: (run / 'input.txt.ack').exists() and
                         (run / 'input.txt.ack').read_text().strip() == ident)
                    if not config.get('perf'): shot(f'{action}-press')
                    end = time.monotonic() + request['hold_ms'] / 1000 + request['settle']
                    while time.monotonic() < end:
                        time.sleep(max(0, min(5, end-time.monotonic())))
                        if not config.get('perf') and time.monotonic()+.1 < end: shot(f'{action}-held')
                image = shot(f'{action}-settled') if action == 'shot' or not config.get('perf') else None
                atomic(run / f'{ident}.done.json', json.dumps(dict(screenshot=image)))
                next_shot = time.monotonic() + 5
            elif not config.get('perf') and time.monotonic() >= next_shot:
                shot('periodic')
                next_shot = time.monotonic() + 5
            time.sleep(.05)
        if proc.poll() is None and time.monotonic() >= deadline: reason = 'time limit'
    except Exception as e:
        reason = f'{type(e).__name__}: {e}'
        record(error=reason)
    finally:
        shutdown = close_family(proc, json.loads((run/'session.json').read_text())['started'], run)
        if LOG.exists(): shutil.copy2(LOG, run / 'run.log')
        for name in ('threads.txt', 'performance.txt'):
            if (GAME / name).exists(): shutil.copy2(GAME / name, run / name)
        atomic(run / 'result.json', json.dumps(dict(reason=reason, shutdown=shutdown,
               exit_code=proc.returncode, elapsed=time.monotonic()-start), indent=2))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('start'); p.add_argument('label'); p.add_argument('--seconds', type=int, default=600)
    p.add_argument('--env', action='append', default=[])
    p.add_argument('--perf', action='store_true', help='capture only explicit shots; no periodic or input readbacks')
    p = sub.add_parser('worker'); p.add_argument('run', type=Path)
    p = sub.add_parser('deploy'); p.add_argument('label'); p.add_argument('--translator', action='store_true')
    p = sub.add_parser('report'); p.add_argument('run', type=Path)
    sub.add_parser('verify', help='compare built/deployed runtime hashes without changing either')
    p = sub.add_parser('recover-child', help='close a verified direct startup-relaunch child of an owned run')
    p.add_argument('run', type=Path); p.add_argument('pid', type=int)
    p = sub.add_parser('install-cache'); p.add_argument('cache', type=Path)
    p = sub.add_parser('route'); p.add_argument('run', type=Path); p.add_argument('steps', type=Path)
    p = sub.add_parser('action'); p.add_argument('run', type=Path); p.add_argument('action', choices=[*BUTTONS, 'shot', 'stop'])
    p.add_argument('--observed', required=True); p.add_argument('--hold-ms', type=int, default=250)
    p.add_argument('--settle', type=float, default=2)
    a = ap.parse_args()
    if a.cmd == 'recover-child': return recover_child(a.run.resolve(), a.pid)
    if a.cmd == 'verify':
        source = sha(ROOT / 'bin/runtime_win64_win32.exe'); deployed = sha(EXE)
        print(json.dumps(dict(interpreter=sys.executable, source=source, deployed=deployed, match=source==deployed), indent=2))
        return 0 if source == deployed else sys.exit(1)
    if a.cmd == 'worker': return worker(a.run.resolve())
    if a.cmd == 'route':
        run = a.run.resolve()
        def act(action, observation):
            cmd = [sys.executable, str(Path(__file__).resolve()), 'action', str(run), action, '--observed', observation]
            if action == 'lr': cmd += ['--hold-ms', '1000']
            result = subprocess.run(cmd, capture_output=True, text=True, timeout=50)
            if result.returncode: raise RuntimeError(result.stderr)
            image = json.loads(result.stdout)['screenshot']
            return run / image if image else None
        previous = None
        for step in json.loads(a.steps.read_text()):
            reference = ui_mask(ROOT/step['template'], step['box'], step['color'])
            if len(reference)<30: raise ValueError('template has too little label detail')
            began = time.monotonic(); end = began+120; retried = False
            while True:
                image = act('shot', 'checking expected UI label: '+step['state'])
                mask = ui_mask(image, step['box'], step['color'])
                score = len(mask & reference)/len(mask | reference)
                if score >= .72:
                    print(step['state'], image.name, 'mask IoU', round(score,3), flush=True)
                    act(step['action'], 'template-matched '+step['state']+'; IoU '+str(round(score,3)))
                    previous = step
                    break
                # Startup may acknowledge L+R while the title is still entering.
                # Retry once only when the same title label is visibly still there.
                if previous and previous['action']=='lr' and not retried and time.monotonic()-began>5:
                    old = ui_mask(ROOT/previous['template'], previous['box'], previous['color'])
                    current = ui_mask(image, previous['box'], previous['color'])
                    if len(old & current)/len(old | current) >= .72:
                        act('lr', 'title L+R still template-matched after 5s; one bounded retry')
                        retried = True
                if time.monotonic()>end: raise TimeoutError(f'expected {step["state"]}; IoU {score:.3f}, {image}')
                time.sleep(1)
        return
    if a.cmd == 'install-cache':
        if game_pids(): ap.error('an existing game process is running; refused cache installation')
        source = a.cache.resolve()
        if not (source/'vk/CHAIN').is_file(): ap.error('source is not a published shader cache')
        target = GAME / 'Mario Kart World 180_nvn/cache'
        backup = ROOT / 'build/mkw-investigation' / time.strftime('%Y%m%d-%H%M%S-original-cache')
        if backup.exists(): ap.error('backup path exists')
        target.rename(backup)
        try: shutil.copytree(source, target)
        except Exception:
            print('Cache copy failed; original retained at', backup)
            raise
        print('Installed', source, 'original preserved at', backup)
        return
    if a.cmd == 'report':
        run = a.run.resolve()
        path = run / 'run.log' if (run / 'run.log').exists() else LOG
        terms = ('input probe:', 'switch 2 video:', 'close type', '[RED]', 'FATAL:', 'svcBreak',
                 '3D slice', 'sampled 3D', 'GPU event waits so far', '[vk] present:', '[vk] pacing:')
        lines = [l for l in path.read_text(errors='replace').splitlines() if any(t in l for t in terms)]
        print('\n'.join(lines))
        print('Latest screenshot:', max(run.glob('*.png'), key=lambda p: p.stat().st_mtime, default='none'))
        return
    if a.cmd == 'deploy':
        if game_pids(): ap.error('an existing game process is running; refused deployment')
        backup = ROOT / 'build/mkw-investigation' / (time.strftime('%Y%m%d-%H%M%S-deploy-') + a.label)
        backup.mkdir(exist_ok=False)
        shutil.copy2(EXE, backup / EXE.name)
        shutil.copy2(ROOT / 'bin/runtime_win64_win32.exe', EXE)
        if a.translator:
            for name in ('nxs_live.py', 'nxs_cache.py', 'nxs_front.py', 'nxshader.py'):
                target = GAME / 'tools/nxshader' / name
                shutil.copy2(target, backup / name)
                shutil.copy2(ROOT / 'tools/nxshader' / name, target)
        assert sha(EXE) == sha(ROOT / 'bin/runtime_win64_win32.exe')
        atomic(backup / 'deployment.json', json.dumps(dict(old=sha(backup / EXE.name), new=sha(EXE)), indent=2))
        print(backup, sha(EXE))
        return
    if a.cmd == 'start':
        if game_pids(): ap.error('an existing game process is running')
        if not 10 <= a.seconds <= 3600: ap.error('seconds must be 10..3600')
        run = ROOT / 'build/mkw-investigation' / (time.strftime('%Y%m%d-%H%M%S-') + a.label)
        run.mkdir(exist_ok=False)
        env = dict(x.split('=', 1) for x in a.env)
        env = {k: v.replace('{run}', str(run)) for k, v in env.items()}
        atomic(run / 'config.json', json.dumps(dict(seconds=a.seconds, env=env, perf=a.perf)))
        subprocess.Popen([sys.executable, str(Path(__file__).resolve()), 'worker', str(run)],
                         stdout=(run / 'harness.log').open('wb'), stderr=subprocess.STDOUT,
                         creationflags=subprocess.CREATE_NO_WINDOW)
        print(run)
    else:
        run = a.run.resolve()
        if (run / 'result.json').exists(): ap.error('run has ended')
        if (run / 'command.json').exists(): ap.error('a command is pending')
        if not 1 <= a.hold_ms <= 10000 or not 0 <= a.settle <= 10: ap.error('invalid hold/settle')
        ident = str(time.time_ns())
        atomic(run / 'command.json', json.dumps(dict(id=ident, action=a.action,
               observed_before=a.observed, hold_ms=a.hold_ms, settle=a.settle)))
        end = time.monotonic() + 45
        done = run / f'{ident}.done.json'
        while not done.exists() and time.monotonic() < end:
            if (run / 'result.json').exists(): raise RuntimeError((run / 'result.json').read_text())
            time.sleep(.1)
        if not done.exists(): raise TimeoutError('action not acknowledged; do not repeat blindly')
        print(done.read_text())


if __name__ == '__main__': main()
