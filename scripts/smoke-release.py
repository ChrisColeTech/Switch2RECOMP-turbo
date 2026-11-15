#!/usr/bin/env python3
# SwitchRecomp - Copyright 2026 MZ Labs
# Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
# Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen.
"""Extract an archive away from the checkout, build a synthetic NUP, and probe its bundled tools."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tarfile
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
from test_nup import minimal_nso, write_nup


def run(args, cwd, env, timeout=180):
    result = subprocess.run(list(map(str, args)), cwd=cwd, env=env, capture_output=True, text=True, timeout=timeout)
    if result.returncode: raise RuntimeError(f'{args[0]} exited {result.returncode}\n{result.stdout}\n{result.stderr}')
    return result.stdout


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('archive', type=Path)
    args = ap.parse_args()
    with tempfile.TemporaryDirectory(prefix='SwitchRecomp relocated [test] ') as temp:
        temp = Path(temp)
        if args.archive.suffix == '.zip':
            with zipfile.ZipFile(args.archive) as z: z.extractall(temp)
        else:
            with tarfile.open(args.archive) as tar: tar.extractall(temp, filter='data')
        root, = [p for p in temp.iterdir() if p.is_dir()]
        for line in (root / 'SHA256SUMS').read_text().splitlines():
            sha, name = line.split('  ', 1)
            with (root / name).open('rb') as stream:
                if hashlib.file_digest(stream, 'sha256').hexdigest() != sha: raise RuntimeError('Checksum mismatch: ' + name)
        env = dict(os.environ)
        for key in list(env):
            if key.startswith(('PYTHON', 'QT_', 'SWR_', 'S2R_', 'DOTNET_', 'CONDA')) or key in {'LD_LIBRARY_PATH', 'VIRTUAL_ENV', 'QTDIR', 'GCC_EXEC_PREFIX', 'COMPILER_PATH', 'LIBRARY_PATH'}: env.pop(key)
        win = os.name == 'nt'
        env['PATH'] = str(Path(os.environ['SystemRoot']) / 'System32') if win else '/usr/bin:/bin'
        env['QT_QPA_PLATFORM'] = 'offscreen'
        env['DOTNET_ROOT'] = str(root / 'dotnet')
        if not win: env['LD_LIBRARY_PATH'] = str(root / 'lib') + ':' + str(root / 'gcc/lib')
        python = root / ('python/python.exe' if win else 'python/bin/python3')
        dotnet = root / ('dotnet/dotnet.exe' if win else 'dotnet/dotnet')
        cc = root / ('gcc/Library/bin/x86_64-w64-mingw32-gcc.exe' if win else 'gcc/bin/x86_64-conda-linux-gnu-gcc')
        shader_tools = root / ('shader-tools/Library/bin' if win else 'shader-tools/bin')
        env['NXSHADER_TOOLS'] = str(shader_tools)
        env['PATH'] = os.pathsep.join([str(cc.parent), str(root / 'bin/launcher'), str(shader_tools), env['PATH']])
        for tool in ('spirv-val', 'spirv-opt'):
            run([shader_tools / (tool + ('.exe' if win else '')), '--version'], temp, env)
        run([python, '-I', '-c', 'from compression import zstd; import ctypes, ssl, multiprocessing'], temp, env)
        sha = run([dotnet, root / 'tools/nxshader/xlate_bin/dmsh_xlate.dll', '--source-sha'], temp, env).strip()
        if not re.fullmatch(r'[0-9a-f]{16}', sha): raise RuntimeError('Shader translator did not identify its source')
        cpp = temp / 'probe.cpp'; cpp.write_text('#include <vector>\nint main(){std::vector<int> v{42};return v[0]!=42;}\n')
        probe = temp / ('probe.exe' if win else 'probe')
        run([cc, cpp, '-lstdc++', '-o', probe], temp, env)
        run([probe], temp, env)
        source = temp / 'Synthetic [v0] & input.nup'
        write_nup(source, {'Data/raw.bin': b'release fixture'}, {'main': minimal_nso()})
        output = temp / 'Build output'; library = temp / 'library.json'
        launcher = root / ('bin/launcher/SwitchRecompLauncher.exe' if win else 'Switch2RECOMP-turbo.sh')
        options = ['--root', root] if win else []
        run([launcher, *options, '--library', library, '--smoke-build', source, '--build-output', output], temp, env)
        game, = json.loads(library.read_text())['games']
        if game['buildIncomplete']: raise RuntimeError('Launcher left the build incomplete')
        for suffix in ('.exe' if win else '', '_code.dll' if win else '_code.so', '_data/romfs.nup'):
            if not (output / (game['outputName'] + suffix)).is_file(): raise RuntimeError('Missing build artifact: ' + suffix)
        if (output / (game['outputName'] + '_data/romfs.nup')).read_text().strip() != str(source): raise RuntimeError('Incorrect NUP pointer')
        run([python, '-I', output / 'tools/nxshader/nxs_live.py', '--dump', temp / 'dump', '--cache', temp / 'cache', '--dotnet', dotnet, '--once'], temp, env)
        print('PASS: relocated archive checksums, Python/zstd, GCC/G++, .NET translator, Qt build, and staged shader helper')


if __name__ == '__main__': main()
