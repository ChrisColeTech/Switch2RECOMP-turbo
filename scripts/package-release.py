#!/usr/bin/env python3
# SwitchRecomp - Copyright 2026 MZ Labs
# Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
# Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen.
"""Stage an allowlisted, relocatable toolkit and archive it with a SHA-256 manifest."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE_DIRS = {'runtime', 'launcher', 'scripts', 'tools', 'tool', 'tests', 'docs', '.github'}
SOURCE_FILES = {'a64.py', 'brt.py', 'codegen.py', 'nup.py', 'nsp.py', 'nxloader.py', 'switchrecomp.py', 'tools_sym.py',
                'LICENSE.txt', 'NOTICE.txt', 'CHANGELOG.txt', 'README.md', 'README.txt', 'DEVELOPER_GUIDE.md',
                'REGRESSION.md', 'Install.bat', 'Install.sh', '.gitattributes', '.gitignore'}
JUNK = {'__pycache__', '_downloads', '.git', '.venv', 'venv', '.pytest_cache', 'shader_cache',
        'captures', 'logs', 'games', 'saves', 'savedata', 'firmware'}
PRIVATE_NAMES = {'library.json', 'python.path', 'performance.txt', 'threads.txt', 'prod.keys', 'title.keys'}
PRIVATE_SUFFIXES = {'.log', '.pyc', '.nup', '.nsp', '.xci', '.keys', '.nca', '.nso', '.nro'}


def excluded(path):
    return (any(part.lower() in JUNK for part in path.parts) or path.name.lower() in PRIVATE_NAMES
            or path.suffix.lower() in PRIVATE_SUFFIXES)


def required_files(platform):
    common = ['switchrecomp.py', 'nup.py', 'nsp.py', 'runtime/rt.h', 'runtime/audio.h',
              'runtime/audio_limit.h', 'runtime/audio_queue.h', 'runtime/controller_sony.h',
              'runtime/controller_sony_win32.h', 'runtime/nvn/gfx/dmsh_abi.h',
              'runtime/nvn/vk/vk_perf.c', 'runtime/nvn/vk/vk_perf.h', 'tools/build_nvn.py',
              'tools/s2r_paths.py', 'tools/nxshader/s2r_paths.py', 'tools/nxshader/nxs_live.py',
              'tools/nxshader/nxs_cache.py', 'tools/nxshader/nxs_front.py', 'tools/nxshader/nxshader.py',
              'tools/nxshader/nxs_refl.py', 'tools/nxshader/nxs_spv.py', 'tools/nxshader/dmsh_abi.py',
              'tools/nxshader/dmsh_ampere.py', 'tools/nxshader/dmsh_meshstage.py',
              'tools/nxshader/xlate_bin/dmsh_xlate.dll', 'tools/nxshader/xlate_bin/dmsh_xlate.deps.json',
              'tools/nxshader/xlate_bin/dmsh_xlate.runtimeconfig.json',
              'tools/nxshader/xlate_bin/LICENSE-Ryujinx.txt', 'tools/nxshader/xlate_bin/LICENSE-nxnative.txt',
              'LICENSE.txt', 'NOTICE.txt', 'docs/THIRD_PARTY.md', 'docs/RELEASES.md',
              'dependency-manifest.json', 'dotnet/LICENSE.txt', 'dotnet/ThirdPartyNotices.txt',
              'runtime/vp9/LICENSE-libvpx.txt', 'runtime/vp9/PATENTS-libvpx.txt',
              'sources/libvpx_1.14.0-1ubuntu2.3.dsc', 'sources/libvpx_1.14.0.orig.tar.xz',
              'sources/libvpx_1.14.0-1ubuntu2.3.debian.tar.xz']
    common += ['sources/' + module + '-everywhere-src-6.8.3.tar.xz' for module in ('qtbase', 'qtimageformats', 'qtsvg')]
    if platform == 'windows':
        return common + ['Switch2RECOMP-turbo.cmd', 'python/python.exe', 'python/LICENSE.txt',
                         'gcc/Library/bin/x86_64-w64-mingw32-gcc.exe', 'dotnet/dotnet.exe',
                         'tools/hactool/hactool.exe', 'tools/hactool/LICENSE', 'scripts/_python.bat',
                         'runtime/vp9/win/libvpx.a', 'bin/runtime_win64_win32.exe',
                         'bin/launcher/SwitchRecompLauncher.exe', 'bin/launcher/Qt6Core.dll',
                         'bin/launcher/Qt6Gui.dll', 'bin/launcher/Qt6Widgets.dll',
                         'bin/launcher/platforms/qwindows.dll', 'bin/launcher/platforms/qoffscreen.dll',
                         'bin/launcher/imageformats/qwebp.dll', 'bin/launcher/msvcp140.dll',
                         'bin/launcher/vcruntime140.dll', 'bin/launcher/vcruntime140_1.dll'] + [
                         'shader-tools/Library/bin/' + tool + '.exe' for tool in ('spirv-as', 'spirv-val', 'spirv-opt')]
    return common + ['Switch2RECOMP-turbo.sh', 'python/bin/python3', 'dotnet/dotnet', 'scripts/_env.sh',
                     'gcc/bin/x86_64-conda-linux-gnu-gcc', 'runtime/vp9/lin/libvpx.a', 'bin/runtime_linux_x11',
                     'bin/launcher/SwitchRecompLauncher', 'bin/launcher/plugins/platforms/libqxcb.so',
                     'bin/launcher/plugins/platforms/libqoffscreen.so', 'bin/launcher/plugins/imageformats/libqwebp.so',
                     'lib/libSDL2-2.0.so.0', 'lib/libQt6Core.so.6', 'linux-libraries.json',
                     'licenses/python/PYTHON.json', 'licenses/python/licenses/LICENSE.cpython.txt'] + [
                     'shader-tools/bin/' + tool for tool in ('spirv-as', 'spirv-val', 'spirv-opt')]


def validate_contents(stage, platform):
    for path in stage.rglob('*'):
        relative = path.relative_to(stage)
        if excluded(relative): raise RuntimeError('Personal/generated release input: ' + str(relative))
        if path.is_symlink() and (os.path.isabs(os.readlink(path)) or not path.exists()
                                  or not path.resolve().is_relative_to(stage.resolve())):
            raise RuntimeError('Non-relocatable symlink: ' + str(relative))
    for name in required_files(platform):
        if not (stage / name).is_file() or not (stage / name).stat().st_size: raise FileNotFoundError(name)
    for name in ('licenses/qt/qtbase', 'licenses/qt/qtimageformats', 'licenses/qt/qtsvg', 'gcc/licenses', 'shader-tools/licenses'):
        if not any(p.is_file() for p in (stage / name).rglob('*')): raise FileNotFoundError(name)
    manifest = json.loads((stage / 'dependency-manifest.json').read_text())
    if manifest['platform'] != platform:
        raise RuntimeError('Dependency manifest platform mismatch')
    vpx = manifest['downloads']['libvpx']
    lib = stage / 'runtime/vp9' / ('win' if platform == 'windows' else 'lin') / 'libvpx.a'
    if digest(lib) != vpx['sha256']: raise RuntimeError('VP9 archive does not match dependency provenance')
    for name, record in vpx['sources'].items():
        if digest(stage / 'sources' / name) != record['sha256']: raise RuntimeError('VP9 source checksum: ' + name)
    runtime = stage / ('bin/runtime_win64_win32.exe' if platform == 'windows' else 'bin/runtime_linux_x11')
    data = runtime.read_bytes()
    # these strings live in the linked decoder, telemetry and controller implementations, not build scripts
    markers = [b'WebM Project VP9 Decoder', b'frame telemetry:']
    if platform == 'windows': markers += [b'hid.dll', b'setupapi.dll']
    for marker in markers:
        if marker not in data: raise RuntimeError('Native runtime feature missing: ' + marker.decode())


def copy(src, dst):
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)


def tree(src, dst):
    if not src.is_dir(): raise FileNotFoundError(src)
    shutil.copytree(src, dst, dirs_exist_ok=True, symlinks=True,
                    ignore=shutil.ignore_patterns(*JUNK, '*.pyc', '*.pdb', '*.log'))


def source_files():
    paths = subprocess.check_output(['git', 'ls-files', '-z', '--cached', '--others', '--exclude-standard'], cwd=ROOT).decode().split('\0')
    for name in sorted(set(paths)):
        p = Path(name)
        if not name or not (p.parts[0] in SOURCE_DIRS or name in SOURCE_FILES): continue
        if excluded(p): continue
        if (ROOT / p).is_file(): yield p


def linux_libraries(stage, qt):
    plugins = stage / 'bin/launcher/plugins'
    for name in ('platforms/libqxcb.so', 'platforms/libqoffscreen.so', 'imageformats/libqwebp.so',
                 'imageformats/libqjpeg.so', 'imageformats/libqsvg.so', 'xcbglintegrations/libqxcb-glx-integration.so'):
        copy(qt / 'plugins' / name, plugins / name)
    env = dict(os.environ, LD_LIBRARY_PATH=os.pathsep.join(map(str, [qt / 'lib', stage / 'lib', stage / 'gcc/lib'])))
    queue = [stage / 'bin/launcher/SwitchRecompLauncher', stage / 'bin/runtime_linux_x11',
             stage / 'python/bin/python3', stage / 'dotnet/dotnet', *plugins.rglob('*.so'),
             *stage.glob('lib/*.so*'), *[stage / 'shader-tools/bin' / tool for tool in ('spirv-as', 'spirv-val', 'spirv-opt')]]
    # .NET loads ICU/OpenSSL dynamically; SDL loads audio/display backends from the host desktop
    for pattern in ('libicu*.so.*', 'libssl.so.*', 'libcrypto.so.*'):
        queue.extend(Path('/usr/lib/x86_64-linux-gnu').glob(pattern))
    seen = set()
    system = re.compile(r'^(ld-linux|lib(c|m|pthread|dl|rt|resolv|util|nss_[^.]+)\.so)')
    records = {}
    while queue:
        binary = queue.pop()
        if str(binary) in seen: continue
        seen.add(str(binary))
        if binary.parent == Path('/usr/lib/x86_64-linux-gnu') and not system.match(binary.name):
            copy(binary, stage / 'lib' / binary.name)
            records.setdefault(binary.name, str(binary))
        output = subprocess.run(['ldd', str(binary)], env=env, capture_output=True, text=True, check=True).stdout
        if 'not found' in output: raise RuntimeError('Missing Linux dependency: ' + output)
        for name, path in re.findall(r'^\s*(\S+) => (/\S+)', output, re.M):
            if system.match(name): continue
            dst = stage / 'lib' / name
            if not dst.exists(): copy(Path(path), dst)
            records.setdefault(name, path)
            queue.append(Path(path))
    (stage / 'linux-libraries.json').write_text(json.dumps(records, indent=2) + '\n')
    # preserve the distribution copyright notices for copied system libraries
    for path in set(records.values()):
        result = subprocess.run(['dpkg-query', '-S', path], capture_output=True, text=True)
        if result.returncode: continue
        package = result.stdout.split(':', 1)[0]
        notice = Path('/usr/share/doc') / package / 'copyright'
        if notice.is_file(): copy(notice, stage / 'licenses/linux' / (package + '.copyright'))


def digest(path):
    with path.open('rb') as stream: return hashlib.file_digest(stream, 'sha256').hexdigest()


def dereference_symlinks(stage):
    """Toolchain archives carry absolute symlinks (e.g. GCC's libgomp.so -> libgomp.so.1.0.0 recorded against
    the extraction directory); a relocated bundle cannot follow them, so replace every link with a copy of its
    target wherever that target resolves. Only unreadable targets fail; validate_contents still runs afterwards."""
    for path in [p for p in stage.rglob('*') if p.is_symlink()]:
        target = Path(os.readlink(path))
        resolved = target if target.is_absolute() else Path(os.path.normpath(path.parent / target))
        if not resolved.is_file():
            raise RuntimeError('Non-relocatable symlink with unreadable target: ' + str(path.relative_to(stage)))
        path.unlink()
        shutil.copy2(resolved, path)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--deps', type=Path, required=True)
    ap.add_argument('--launcher', type=Path, required=True)
    ap.add_argument('--qt', type=Path, required=True)
    ap.add_argument('--output', type=Path, default=ROOT / 'dist')
    ap.add_argument('--work', type=Path, help='Optional staging directory on a faster filesystem')
    ap.add_argument('--name', default='Switch2RECOMP-turbo')
    ap.add_argument('--stage-only', action='store_true')
    args = ap.parse_args()
    if not re.fullmatch(r'[A-Za-z0-9._-]+', args.name): ap.error('Invalid archive name')
    platform = 'windows' if os.name == 'nt' else 'linux'
    args.output = args.output.resolve(); args.deps = args.deps.resolve(); args.qt = args.qt.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    stage = (args.work.resolve() if args.work else args.output) / (args.name + '-' + platform + '-x64')
    # never merge a release with stale or unrelated files
    stage.mkdir(parents=True, exist_ok=False)
    for path in source_files(): copy(ROOT / path, stage / path)
    for name in ('python', 'gcc', 'dotnet', 'shader-tools'): tree(args.deps / name, stage / name)
    if platform == 'windows': tree(args.deps / 'tools/hactool', stage / 'tools/hactool')
    copy(args.deps / 'dependency-manifest.json', stage / 'dependency-manifest.json')
    tree(args.launcher, stage / 'bin/launcher')
    runtime = 'runtime_win64_win32.exe' if platform == 'windows' else 'runtime_linux_x11'
    copy(ROOT / 'bin' / runtime, stage / 'bin' / runtime)
    # Linux VP9 is generated by release-deps and ignored by Git; never silently drop it.
    vp9 = Path('runtime/vp9') / ('win' if platform == 'windows' else 'lin') / 'libvpx.a'
    copy(ROOT / vp9, stage / vp9)
    tree(args.deps / 'licenses', stage / 'licenses')
    tree(args.deps / 'sources', stage / 'sources')
    if platform == 'windows':
        (stage / 'Switch2RECOMP-turbo.cmd').write_text('@echo off\n"%~dp0bin\\launcher\\SwitchRecompLauncher.exe" --root "%~dp0." %*\n', newline='\r\n')
    else:
        tree(args.deps / 'lib', stage / 'lib')
        linux_libraries(stage, args.qt)
        (stage / 'bin/launcher/qt.conf').write_text('[Paths]\nPlugins=plugins\n')
        wrapper = stage / 'Switch2RECOMP-turbo.sh'
        wrapper.write_text('#!/bin/sh\nset -eu\nroot=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)\nexport LD_LIBRARY_PATH="$root/lib:$root/gcc/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"\nexport QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-xcb}"\nexec "$root/bin/launcher/SwitchRecompLauncher" --root "$root" "$@"\n')
        wrapper.chmod(0o755)
    dereference_symlinks(stage)
    validate_contents(stage, platform)
    commit = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
    dirty = bool(subprocess.check_output(['git', 'status', '--porcelain', '--untracked-files=normal'], cwd=ROOT))
    (stage / 'build-info.json').write_text(json.dumps({'commit': commit, 'platform': platform, 'local_changes': dirty,
        'source': 'https://github.com/ChrisColeTech/Switch2RECOMP-turbo/tree/' + commit}, indent=2) + '\n')
    files = sorted(p for p in stage.rglob('*') if p.is_file())
    (stage / 'SHA256SUMS').write_text(''.join(digest(p) + '  ' + p.relative_to(stage).as_posix() + '\n' for p in files))
    if args.stage_only:
        print(stage); return
    if platform == 'windows':
        archive = args.output / (stage.name + '.zip')
        with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as z:
            for p in sorted(stage.rglob('*')):
                if p.is_file(): z.write(p, p.relative_to(stage.parent))
    else:
        archive = args.output / (stage.name + '.tar.gz')
        with tarfile.open(archive, 'w:gz') as tar: tar.add(stage, arcname=stage.name)
    Path(str(archive) + '.sha256').write_text(digest(archive) + '  ' + archive.name + '\n')
    print(archive)


if __name__ == '__main__': main()
