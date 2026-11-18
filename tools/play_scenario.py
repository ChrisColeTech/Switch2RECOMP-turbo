# SwitchRecomp - Copyright 2026 MZ Labs
# Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
# Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen.
"""The scripted gameplay runs of docs/GAMEPLAY_INVESTIGATION.md, one command each, on top of tools/plza_probe.py.

python tools/play_scenario.py dkb  --game "<buildDir>" [--label dkb-after] [--env NAME=VALUE ...] [--window 100,150]
python tools/play_scenario.py stats <run folder> [--window 100,150]

A scenario starts the game through the harness (metrics on), sends its steps, stops the game and prints the run's
numbers: FPS from metrics.csv and the present interval's p50 / p95 / p99 / longest from frames.csv, over the window
(seconds since the game's start), with each screenshot's black audit. Scenarios: dkb (title, Continue, walk, punch,
walk), mkw (to a race, two drives), plza (title, the station, two walks), zelda (title, menu, A, A), mk8d (from a
save without a Mii: Select a Mii, then to a Grand Prix race, drive). The game's save is not touched here: copy <Game>_data/save aside first.
"""
import argparse
import csv
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
PROBE = ROOT / 'tools/plza_probe.py'
INPUTS = ROOT / 'tools/plza_inputs'
# (action, settle seconds, observed, replay file)
SCENARIOS = {
    'dkb': dict(seconds=200, window=(100, 150), steps=[
        ('shot', 75, 'boot'), ('a', 6, 'title'), ('a', 25, 'Continue'), ('replay', 14, 'gameplay', 'dkb_walk.txt'),
        ('replay', 6, 'walked', 'dkb_punch.txt'), ('replay', 14, 'punched', 'dkb_walk.txt'), ('shot', 10, 'end')]),
    'mkw': dict(seconds=230, window=(150, 185), steps=[
        ('shot', 44, 'boot'), ('lr', 5, 'title')] + [('a', 6, 'menu')] * 8 + [
        ('replay', 42, 'race', 'mkw_drive.txt'), ('replay', 42, 'race', 'mkw_drive.txt')]),
    'plza': dict(seconds=150, window=(45, 84), steps=[
        ('shot', 12, 'boot'), ('a', 30, 'title'), ('shot', 8, 'station'), ('replay', 10, 'walk', 'plza_walk.txt'),
        ('shot', 5, 'walked'), ('replay', 10, 'walk', 'plza_walk.txt'), ('shot', 5, 'walked')]),
    'zelda': dict(seconds=140, window=(45, 100), steps=[
        ('shot', 40, 'boot'), ('shot', 25, 'title'), ('a', 15, 'menu'), ('a', 15, 'menu'), ('shot', 10, 'end')]),
    'mk8d': dict(seconds=240, window=(100, 140), steps=[
        ('shot', 9, 'boot'), ('lr', 4, 'title'), ('right', 1.5, 'Select a Mii: off Create a Mii'), ('a', 3, 'first Mii'), ('a', 3, 'menu1'), ('a', 3, 'menu2'), ('a', 5, 'menu3'), ('a', 4, 'char'),
        ('a', 2, 'k1'), ('a', 2, 'k2'), ('a', 4, 'k3'), ('a', 4, 'cup'), ('a', 30, 'gp-ok'),
        ('replay', 20, 'driving', 'mkw_drive.txt'), ('shot', 18, 'driving +20s')]),
}


def pct(v, q):
    return v[min(len(v) - 1, int(len(v) * q))] if v else 0.0


def stats(run, window):
    run = Path(run); lo, hi = window
    out = dict(run=str(run), window=[lo, hi])
    fr = run / 'frames.csv'
    if fr.exists():
        t0 = None; iv = []
        with fr.open(newline='') as f:
            for row in csv.DictReader(f):
                if row['kind'] != 'present': continue
                t = int(row['unix_ns'])
                if t0 is None: t0 = t
                s = (t - t0) / 1e9
                if lo <= s <= hi and int(row['interval_ns']) > 0: iv.append(int(row['interval_ns']) / 1e6)
        iv.sort()
        if iv:
            out.update(frames=len(iv), fps_frames=round(1000 * len(iv) / sum(iv), 2), p50=round(pct(iv, .50), 2), p95=round(pct(iv, .95), 2),
                       p99=round(pct(iv, .99), 2), longest=round(iv[-1], 1), over_20ms=sum(1 for x in iv if x > 20), over_33ms=sum(1 for x in iv if x > 33.4),
                       over_50ms=sum(1 for x in iv if x > 50))
    mt = run / 'metrics.csv'
    if mt.exists():
        fps = []; lazy = []; cols = {}
        with mt.open(newline='') as f:
            for row in csv.DictReader(f):
                try: t = float(row['t_s'])
                except (KeyError, ValueError): continue
                if lo <= t <= hi:
                    fps.append(float(row['fps']))
                    for c in ('draws', 'draws_skipped', 'wb_lazy_pages', 'wb_lazy_in', 'wb_lazy_skip_kb', 'tex_new', 'shader_missing'):
                        if c in row and row[c] not in ('', None): cols.setdefault(c, []).append(float(row[c]))
        if fps: out.update(fps_metrics=round(sum(fps) / len(fps), 2), seconds=len(fps), seconds_58=sum(1 for x in fps if x >= 58), fps_min=min(fps))
        for c, v in cols.items(): out[c + '_mean'] = round(sum(v) / len(v), 1)
    shots = []
    ac = run / 'actions.jsonl'
    if ac.exists():
        for line in ac.read_text(encoding='utf-8').splitlines():
            j = json.loads(line)
            if 'screenshot' in j: shots.append((j['screenshot'], j.get('black_fraction'), j.get('black_cells')))
    out['screenshots'] = shots
    rs = run / 'result.json'
    if rs.exists(): out['result'] = json.loads(rs.read_text())
    return out


def probe(*args):
    r = subprocess.run([sys.executable, str(PROBE), *map(str, args)], capture_output=True, text=True)
    if r.returncode: raise SystemExit('plza_probe %s failed:\n%s' % (' '.join(map(str, args[:3])), (r.stdout + r.stderr)[-2000:]))
    return r.stdout.strip()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('scenario', choices=[*SCENARIOS, 'stats'])
    ap.add_argument('run', nargs='?', help='with stats: the run folder')
    ap.add_argument('--game'); ap.add_argument('--label'); ap.add_argument('--env', action='append', default=[]); ap.add_argument('--option', action='append', default=[], help='KEY=VALUE ini option for this run, e.g. docked=1')
    ap.add_argument('--window', help='seconds from,to for the numbers'); ap.add_argument('--seconds', type=int)
    a = ap.parse_args()
    win = tuple(float(x) for x in a.window.split(',')) if a.window else None
    if a.scenario == 'stats':
        print(json.dumps(stats(a.run, win or (0, 1e9)), indent=1)); return
    if not a.game: ap.error('--game <buildDir> is required')
    sc = SCENARIOS[a.scenario]
    args = ['start', a.label or a.scenario, '--game', a.game, '--seconds', a.seconds or sc['seconds'], '--metrics']
    for e in a.env: args += ['--env', e]
    for o in a.option: args += ['--option', o]
    run = probe(*args).splitlines()[-1]
    print(run, flush=True)
    try:
        for step in sc['steps']:
            extra = ['--file', INPUTS / step[3]] if step[0] == 'replay' else []
            probe('action', run, step[0], '--observed', step[2], '--settle', step[1], *extra)
    finally:
        try: probe('action', run, 'stop', '--observed', 'done')
        except SystemExit as e: print(e)
    import time
    for _ in range(300):
        if (Path(run) / 'result.json').exists(): break
        time.sleep(.2)
    print(json.dumps(stats(run, win or sc['window']), indent=1))


if __name__ == '__main__':
    main()
