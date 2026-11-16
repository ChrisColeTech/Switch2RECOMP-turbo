# SwitchRecomp - Copyright 2026 MZ Labs
# Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
# Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen.
"""Compare cached compute execution modes with the preserved NVN2 control header."""
import collections
import json
from pathlib import Path
import struct
import argparse
import shutil
import subprocess
import sys
import time

root = Path(__file__).resolve().parents[1] / 'build/mkw-nsp-1.8/Mario Kart World 180_nvn/cache'
ap = argparse.ArgumentParser(); ap.add_argument('--prepare', action='store_true'); ap.add_argument('--cache', type=Path); ap.add_argument('--validate', action='store_true'); args = ap.parse_args()
if args.cache: root = args.cache
counts = collections.Counter()
examples = []
failures = []
for p in sorted((root/'vk').glob('*.json')):
    meta = json.loads(p.read_text())
    if meta.get('stage') != 'cs': continue
    ctl = root/'dumps'/f'{p.stem}.ctl'
    if not ctl.exists(): continue
    data = ctl.read_bytes()
    dims = struct.unpack_from('<3I', data, 0x58)
    if args.validate:
        result = subprocess.run(['C:/VulkanSDK/1.3.268.0/Bin/spirv-val.exe', '--target-env', 'vulkan1.3', str(p.with_suffix('.spv'))], capture_output=True, text=True)
        if result.returncode: failures.append(dict(key=p.stem, error=result.stderr))
    counts[(tuple(meta.get('local_size', [])), dims)] += 1
    if len(examples)<12 or p.stem=='96591f5d42fa8104':
        examples.append(dict(key=p.stem, local=meta.get('local_size'), ctl58=dims, header=data[0x48:0x80].hex(' ')))
print(json.dumps(examples, indent=2))
for (local,dims), n in counts.most_common(): print(n, 'cached', local, 'control58', dims)
if args.validate:
    print('Validation failures:', json.dumps(failures, indent=2))
    (root.parent / ('compute-validation-'+str(time.time_ns())+'.json')).write_text(json.dumps(failures, indent=2))
    if failures: sys.exit(1)
if args.prepare:
    repo = Path(__file__).resolve().parents[1]
    dest = repo / 'build/mkw-investigation' / time.strftime('%Y%m%d-%H%M%S-compute-cache')
    dest.mkdir(exist_ok=False)
    shutil.copytree(root, dest/'cache')
    print('Preserved source cache; isolated copy:', dest, flush=True)
    with (dest/'translation.log').open('w', encoding='utf-8') as log:
        result = subprocess.run([sys.executable, str(repo/'tools/nxshader/nxs_live.py'), '--once',
                  '--dump', str(dest/'cache/dumps'), '--cache', str(dest/'cache'),
                  '--dotnet', shutil.which('dotnet') or 'dotnet'], stdout=log, stderr=subprocess.STDOUT, timeout=600)
    print('Translation exit', result.returncode, 'cache', dest/'cache')
    sys.exit(result.returncode)
