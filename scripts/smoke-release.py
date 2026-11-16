#!/usr/bin/env python3
# SwitchRecomp - Copyright 2026 MZ Labs
# Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
# Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen.
"""Extract an archive away from the checkout, build a synthetic NUP, and probe its bundled tools."""
import argparse
import importlib.util
import json
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys
import tarfile
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
from test_nup import minimal_nso, write_nup
spec = importlib.util.spec_from_file_location('release_package', ROOT / 'scripts/package-release.py')
package = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package)


def safe_name(name):
    p = PurePosixPath(name)
    if not name or p.is_absolute() or '..' in p.parts or '\\' in name or ':' in name or p.as_posix() != name.rstrip('/'):
        raise RuntimeError('Unsafe archive/manifest path: ' + name)
    return p


def verify_manifest(root):
    listed = set()
    for line in (root / 'SHA256SUMS').read_text().splitlines():
        sha, name = line.split('  ', 1)
        safe_name(name)
        if not re.fullmatch('[0-9a-f]{64}', sha) or name in listed or name == 'SHA256SUMS':
            raise RuntimeError('Invalid or duplicate manifest entry: ' + name)
        listed.add(name)
        if package.digest(root / name) != sha: raise RuntimeError('Checksum mismatch: ' + name)
    actual = {p.relative_to(root).as_posix() for p in root.rglob('*') if p.is_file()} - {'SHA256SUMS'}
    if listed != actual: raise RuntimeError('Manifest does not cover exactly the archive contents: ' + str(actual ^ listed))


def extract_archive(archive, dest):
    sha, name = Path(str(archive) + '.sha256').read_text().strip().split('  ', 1)
    if name != archive.name or sha != package.digest(archive): raise RuntimeError('Archive checksum mismatch')
    if archive.suffix == '.zip':
        with zipfile.ZipFile(archive) as z:
            names = [item.filename for item in z.infolist()]
            for name in names: safe_name(name)
            if len(names) != len(set(names)): raise RuntimeError('Duplicate ZIP entry')
            z.extractall(dest)
    else:
        with tarfile.open(archive) as tar:
            names = [member.name for member in tar.getmembers()]
            for name in names: safe_name(name)
            if len(names) != len(set(names)): raise RuntimeError('Duplicate tar entry')
            tar.extractall(dest, filter='data')
    children = list(dest.iterdir())
    if len(children) != 1 or not children[0].is_dir(): raise RuntimeError('Expected one toolkit directory')
    return children[0]


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
        root = extract_archive(args.archive, temp)
        win = os.name == 'nt'
        package.validate_contents(root, 'windows' if win else 'linux')
        verify_manifest(root)
        info = json.loads((root / 'build-info.json').read_text())
        if os.environ.get('GITHUB_SHA') and (info['commit'] != os.environ['GITHUB_SHA'] or info['local_changes']):
            raise RuntimeError('Release provenance differs from the clean CI commit')
        env = dict(os.environ)
        for key in list(env):
            if key.startswith(('PYTHON', 'QT_', 'SWR_', 'S2R_', 'DOTNET_', 'CONDA')) or key in {'LD_LIBRARY_PATH', 'VIRTUAL_ENV', 'QTDIR', 'GCC_EXEC_PREFIX', 'COMPILER_PATH', 'LIBRARY_PATH'}: env.pop(key)
        env['PATH'] = str(Path(os.environ['SystemRoot']) / 'System32') if win else '/usr/bin:/bin'
        env['QT_QPA_PLATFORM'] = 'offscreen'
        env['DOTNET_ROOT'] = str(root / 'dotnet')
        env['PYTHONDONTWRITEBYTECODE'] = '1'
        # keep all settings and caches disposable, even if the launcher changes its defaults
        home = temp / 'profile'; home.mkdir()
        for key in ('HOME', 'USERPROFILE', 'APPDATA', 'LOCALAPPDATA', 'XDG_CONFIG_HOME', 'XDG_CACHE_HOME', 'XDG_DATA_HOME'):
            env[key] = str(home)
        if not win: env['LD_LIBRARY_PATH'] = str(root / 'lib') + ':' + str(root / 'gcc/lib')
        python = root / ('python/python.exe' if win else 'python/bin/python3')
        dotnet = root / ('dotnet/dotnet.exe' if win else 'dotnet/dotnet')
        cc = root / ('gcc/Library/bin/x86_64-w64-mingw32-gcc.exe' if win else 'gcc/bin/x86_64-conda-linux-gnu-gcc')
        shader_tools = root / ('shader-tools/Library/bin' if win else 'shader-tools/bin')
        env['NXSHADER_TOOLS'] = str(shader_tools)
        env['PATH'] = os.pathsep.join([str(cc.parent), str(root / 'bin/launcher'), str(shader_tools), env['PATH']])
        if not win:
            trace = subprocess.run(['sh', '-x', root / 'fix-gcc.sh'], cwd=temp, env=env, capture_output=True,
                                   text=True, timeout=120)
            print(trace.stdout + trace.stderr)
            if trace.returncode: raise RuntimeError(f'fix-gcc.sh exited {trace.returncode}\n{trace.stdout}\n{trace.stderr}')
            probe = subprocess.run([cc, '-print-sysroot', '-dumpmachine'], cwd=temp, env=env, capture_output=True,
                                   text=True, timeout=60)
            print('gcc sysroot probe:', probe.stdout, probe.stderr)
            if probe.returncode or 'x86_64-conda-linux-gnu' not in probe.stdout:
                raise RuntimeError(f'Relocated GCC is unusable: {probe.stdout}{probe.stderr}')
        for tool in ('spirv-as', 'spirv-val', 'spirv-opt'):
            run([shader_tools / (tool + ('.exe' if win else '')), '--version'], temp, env)
        if win:
            probe = subprocess.run([root / 'tools/hactool/hactool.exe', '--help'], cwd=temp, env=env,
                                   capture_output=True, text=True, timeout=30)
            if probe.returncode not in (0, 1) or 'hactool' not in (probe.stdout + probe.stderr).lower():
                raise RuntimeError('Bundled hactool did not start')
        run([python, '-I', '-c', 'from compression import zstd; import ctypes, ssl, multiprocessing'], temp, env)
        selected = run([python, '-B', root / 'tools/s2r_paths.py'], temp, env)
        if str(python) not in selected: raise RuntimeError('Relocated helpers did not select bundled Python')
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
        runtime = root / ('bin/runtime_win64_win32.exe' if win else 'bin/runtime_linux_x11')
        if not win and not os.access(runtime, os.X_OK): raise RuntimeError('Runtime lost its executable permission')
        built_runtime = output / (game['outputName'] + ('.exe' if win else ''))
        if package.digest(runtime) != package.digest(built_runtime): raise RuntimeError('Synthetic build used a different runtime')
        if 'renderer=vulkan' not in (output / (game['outputName'] + '.ini')).read_text():
            raise RuntimeError('Synthetic build did not select the native Vulkan renderer')
        if (output / (game['outputName'] + '_data/romfs')).exists(): raise RuntimeError('Synthetic NUP was unnecessarily extracted')
        for _ in range(2):
            run([launcher, *options, '--library', library, '--import-build', built_runtime,
                 '--screenshot', temp / 'import.png'], temp, env)
            imported = json.loads(library.read_text())['games']
            if len(imported) != 1 or imported[0]['buildIncomplete'] or imported[0]['renderer'] != 'vulkan':
                raise RuntimeError('Importing the built fixture lost its settings or duplicated the library entry')
            if package.digest(built_runtime) != package.digest(runtime): raise RuntimeError('Import modified the executable')
        for name in ('nxs_live.py', 'nxs_front.py', 'nxs_cache.py', 'nxs_refl.py', 'nxs_spv.py', 'dmsh_abi.py',
                     'dmsh_ampere.py', 'dmsh_meshstage.py', 'nxshader.py', 's2r_paths.py', 'xlate_bin/dmsh_xlate.dll'):
            if not (output / 'tools/nxshader' / name).is_file(): raise FileNotFoundError(name)
        run([python, '-I', '-B', '-c', 'import sys; sys.path.insert(0, sys.argv[1]); import nxshader, nxs_live, dmsh_ampere, dmsh_meshstage',
             output / 'tools/nxshader'], temp, env)
        run([python, '-I', output / 'tools/nxshader/nxs_live.py', '--dump', temp / 'dump', '--cache', temp / 'cache', '--dotnet', dotnet, '--once'], temp, env)
        print('PASS: archive/file checksums, contents/provenance, native feature markers, relocated Python/zstd, GCC/G++, .NET/SPIRV, Qt synthetic build/import, and staged shader helper')


if __name__ == '__main__': main()
