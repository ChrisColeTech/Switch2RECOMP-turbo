# Copyright 2026 ChrisColeTech
# SPDX-License-Identifier: MIT
"""Reproducible, capture-free race interval using mkw_probe's owned process."""
import argparse
import csv
import json
import math
from pathlib import Path
import re
import subprocess
import sys
import time
from mkw_probe import ui_mask

ROOT = Path(__file__).resolve().parents[1]
PROBE = ROOT / 'tools/mkw_probe.py'


def probe(*args, timeout=60):
    r = subprocess.run([sys.executable, str(PROBE), *map(str, args)], cwd=ROOT,
                       capture_output=True, text=True, timeout=timeout)
    if r.returncode: raise RuntimeError(r.stdout + r.stderr)
    return r.stdout.strip()


def drive(run):
    def action(name, **kw):
        args = ['action', run, name, '--observed', kw.pop('observed', 'deterministic performance protocol')]
        for k, v in kw.items(): args += ['--' + k.replace('_', '-'), str(v)]
        return json.loads(probe(*args))
    try:
        route = ROOT / 'build/mkw-investigation/race-route.json'
        (run / 'race-route.json').write_bytes(route.read_bytes())
        print(probe('route', run, route, timeout=240), flush=True)
        # The verified route ends at the race confirmation. Let the movie, loading,
        # flyover and countdown finish before the sole initial race capture.
        time.sleep(80)
        first = action('shot', observed='initial race visual check after route and 80s settle')
        box = [280,975,375,1030]
        template = ROOT / 'build/mkw-investigation/20261115-185844-race-exclusive/0106-shot-settled.png'
        ref = ui_mask(template, box, 'white'); got = ui_mask(run / first['screenshot'], box, 'white')
        score = len(ref & got)/len(ref | got)
        if score < .80: raise RuntimeError(f'initial lap 1/3 HUD not confirmed: IoU {score:.3f}')
        time.sleep(3)
        started = json.loads((run / 'session.json').read_text())['started']
        markers = dict(start=time.time()-started, initial=first['screenshot'], lap_mask_iou=score, holds=12, hold_ms=10000)
        (run / 'measurement.json').write_text(json.dumps(markers, indent=2))
        for _ in range(12): action('a', hold_ms=10000, settle=0)
        markers['end'] = time.time()-started
        (run / 'measurement.json').write_text(json.dumps(markers, indent=2))
        markers['final'] = action('shot', observed='final visual check after twelve identical 10s A holds')['screenshot']
        (run / 'measurement.json').write_text(json.dumps(markers, indent=2))
    finally:
        if not (run / 'result.json').exists(): print(action('stop'), flush=True)


def distribution(v):
    if not v: return {}
    s = sorted(v)
    return dict(n=len(s), fps=1000*len(s)/sum(s), mean_ms=sum(s)/len(s),
                **{f'p{p}_ms': s[max(0, math.ceil(len(s)*p/100)-1)] for p in (50, 95, 99)},
                max_ms=s[-1], **{f'over_{t}_ms': sum(x > t for x in s) for t in (1000/60, 33, 50, 100)})


def report(run):
    markers = json.loads((run / 'measurement.json').read_text())
    session = json.loads((run / 'session.json').read_text())
    lo, hi = markers['start'], markers.get('end', time.time()-session['started'])
    log = run / 'run.log'
    if not log.exists(): log = ROOT / 'build/mkw-nsp-1.8/Mario Kart World 180.log'
    lines = log.read_text(errors='replace').splitlines()
    active = []
    for line in lines:
        m = re.match(r'\[\s*([\d.]+)\]', line)
        if m and lo <= float(m[1]) <= hi: active.append(line)
    result = dict(run=str(run), measurement=markers, session=session,
                  capture_free=True, present_definition='host vkQueuePresentKHR completion, not physical scanout')
    result['fatal_diagnostics'] = [x for x in lines if 'svcBreak' in x or 'FATAL:' in x or 'VK_ERROR_DEVICE_LOST' in x]
    if (run/'result.json').exists(): result['shutdown'] = json.loads((run/'result.json').read_text())
    result['log_present_windows'] = [x for x in active if '[vk] present:' in x]
    result['slow_frames'] = [x for x in active if 'slow frame' in x]
    result['phase_samples'] = [x for x in active if 'replay phases per frame:' in x]
    result['gpu_samples'] = [x for x in active if 'GPU time up to frame' in x]
    result['capture_free'] = not any('screenshot' in r and lo <= r['elapsed'] <= hi
        for r in (json.loads(x) for x in (run/'actions.jsonl').read_text().splitlines()))
    result['minimized_in_measurement'] = any('window minimized' in x for x in active)
    # Whole log windows give the same coarse measurement when CSV is disabled.
    windows = []
    for line in result['log_present_windows']:
        m = re.search(r'\[\s*([\d.]+)\].* ([\d.]+) per s; interval p50 ([\d.]+) / p95 ([\d.]+) / p99 ([\d.]+) / max ([\d.]+)', line)
        if m and float(m[1])-300/float(m[2]) >= lo+1:
            windows.append([float(x) for x in m.groups()])
    if windows:
        result['whole_log_windows'] = dict(frames=300*len(windows), fps=300*len(windows)/sum(300/x[1] for x in windows),
            max_ms=max(x[5] for x in windows), worst_window_p95_ms=max(x[3] for x in windows), worst_window_p99_ms=max(x[4] for x in windows))
    cpu = [float(m[1]) for line in active if (m := re.search(r'translation thread .*\(CPU ([\d.]+) ms\)', line))]
    if cpu: result['translation_thread_cpu_mean_ms'] = sum(cpu)/len(cpu)
    path = run / 'frames.csv'
    if path.exists():
        rows = []
        with path.open(newline='') as f:
            for r in csv.DictReader(f):
                try: rows.append({k: v if k == 'kind' else int(v) for k, v in r.items()})
                except (ValueError, TypeError): continue  # writer may be in its last row
        selected = {kind: [r for r in rows if r['kind']==kind and lo <= r['unix_ns']/1e9-session['started'] <= hi]
                    for kind in ('present', 'replay')}
        for kind, rr in selected.items():
            result[kind] = distribution([r['interval_ns']/1e6 for r in rr if r['interval_ns']])
        pp = selected['present']; rp = [r for r in rows if r['kind']=='replay']
        result['dropped_delta'] = pp[-1]['dropped']-pp[0]['dropped'] if pp else None
        for key in ('cpu_ns','acquire_ns','fence_ns','queue_ns','enqueue_ns'):
            costs = distribution([r[key]/1e6 for r in pp if r[key]])
            result['present'][key] = {k:v for k,v in costs.items() if k != 'fps' and not k.startswith('over_')}
        counters = ('cpu_ns','sparse_ns','wait_ns','texture_ns','pipeline_ns','pool_ns','gpu_ns','writeback_ns','astc_ns','cache_ns')
        deltas = []
        for prev, r in zip(rp, rp[1:]):
            if not lo <= r['unix_ns']/1e9-session['started'] <= hi: continue
            d = dict(frame=r['frame'], elapsed=r['unix_ns']/1e9-session['started'], interval_ms=r['interval_ns']/1e6)
            d.update({key: max(0,r[key]-prev[key])/1e6 for key in counters})
            deltas.append(d)
        result['replay_counter_ms_per_frame'] = {key: sum(r[key] for r in deltas)/len(deltas) for key in counters} if deltas else {}
        rr = selected['replay']
        if len(rr)>1: result['writeback_mib_per_frame'] = (rr[-1]['writeback_bytes']-rr[0]['writeback_bytes'])/(len(rr)-1)/1048576
        result['worst_replay'] = sorted(deltas, key=lambda r:r['interval_ms'], reverse=True)[:20]
        result['worst_present'] = sorted(pp, key=lambda r:r['interval_ns'], reverse=True)[:20]
        result['telemetry_loss'] = [x for x in lines if 'records lost' in x or 'telemetry write failed' in x]
        # Small standalone chart, generated offline; the runtime never renders an overlay.
        points = ' '.join(f'{(r["unix_ns"]/1e9-session["started"]-lo)/(hi-lo)*1200:.1f},{300-min(300,r["interval_ns"]/1e6*3):.1f}' for r in pp)
        (run / 'frametimes.html').write_text('<!doctype html><meta charset="utf-8"><title>MKW frametimes</title>'
            '<style>body{background:#101820;color:#eee;font:16px monospace}svg{width:100%;background:#182530}pre{white-space:pre-wrap}</style>'
            f'<h1>{run.name}</h1><p>Present intervals, 0 to 100 ms. Red line: 16.667 ms. Capture-free race interval.</p>'
            f'<svg viewBox="0 0 1200 300"><line x1="0" y1="250" x2="1200" y2="250" stroke="tomato"/>'
            f'<polyline points="{points}" fill="none" stroke="#50d5ff" stroke-width="1"/></svg>'
            '<pre>'+json.dumps(result['present'], indent=2)+'</pre>', encoding='utf-8')
    (run / ('summary.json' if 'end' in markers else 'summary-partial.json')).write_text(json.dumps(result, indent=2))
    print(json.dumps({k:v for k,v in result.items() if k not in ('slow_frames','phase_samples','gpu_samples','worst_replay','worst_present','log_present_windows')}, indent=2))
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('label'); ap.add_argument('--env', action='append', default=[])
    ap.add_argument('--worker', type=Path)
    ap.add_argument('--report', type=Path)
    ap.add_argument('--series-after', type=Path, help='after an owned run ends, run old-path/on, fixed/on, fixed/off sequentially')
    ap.add_argument('--series-worker', type=Path, help=argparse.SUPPRESS)
    ap.add_argument('--normal', action='store_true', help='with --series-after: CSV on/off with detailed phase tracing disabled')
    a = ap.parse_args()
    if a.series_after:
        run = a.series_after.resolve()
        with (run / 'series.log').open('xb') as log:
            args = [sys.executable, str(Path(__file__).resolve()), 'series', '--series-worker', str(run)]
            if a.normal: args += ['--normal']
            p = subprocess.Popen(args,
                                 cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW)
        print('series supervisor PID', p.pid, 'log', run / 'series.log')
        return
    if a.series_worker:
        previous = a.series_worker.resolve(); runs = [str(previous)]
        cases = [('normal-telemetry-on', ['SWR_FRAME_CSV={run}/frames.csv']), ('normal-telemetry-off', [])] if a.normal else [
            ('wb-shadow-reverse', ['NXRT_VK_WB_SHADOW_SKIP=0', 'SWR_FRAME_CSV={run}/frames.csv']),
            ('wb-shadow-repeat', ['SWR_FRAME_CSV={run}/frames.csv']), ('telemetry-off', [])]
        for label, env in cases:
            deadline = time.monotonic()+500
            while not (previous/'result.json').exists():
                if time.monotonic()>deadline: raise TimeoutError(f'run did not finish: {previous}')
                time.sleep(1)
            # An interrupted or mismatched route must not silently start another game.
            markers = json.loads((previous/'measurement.json').read_text())
            if 'final' not in markers: raise RuntimeError(f'incomplete run: {previous}')
            if report(previous)['fatal_diagnostics']: raise RuntimeError(f'fatal diagnostics in {previous}; not a clean comparison')
            args = [sys.executable, str(Path(__file__).resolve()), label, '--env', 'NXRT_VK_PHASES='+('0' if a.normal else '1')]
            for e in env: args += ['--env', e]
            r = subprocess.run(args, capture_output=True, text=True, timeout=30, check=True)
            previous = Path(r.stdout.strip()); runs.append(str(previous))
            (a.series_worker/'comparison.json').write_text(json.dumps(runs, indent=2))
            print('started', previous, flush=True)
        return
    if a.report: return report(a.report)
    if a.worker: return drive(a.worker)
    args = ['start', a.label, '--seconds', '480', '--perf']
    for e in a.env: args += ['--env', e]
    run = Path(probe(*args))
    with (run / 'driver.log').open('wb') as log:
        subprocess.Popen([sys.executable, str(Path(__file__).resolve()), a.label, '--worker', str(run)],
                         cwd=ROOT, stdout=log, stderr=subprocess.STDOUT,
                         creationflags=subprocess.CREATE_NO_WINDOW)
    print(run)


if __name__ == '__main__': main()
