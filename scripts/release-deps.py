#!/usr/bin/env python3
# SwitchRecomp - Copyright 2026 MZ Labs
# Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
# Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen.
"""Fetch checksum-pinned redistributable tools into an isolated release dependency directory."""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tarfile
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tool'))
import get_deps as deps

ASSETS = {
    'windows': {
        'python': ('https://api.nuget.org/v3-flatcontainer/python/3.14.9/python.3.14.9.nupkg', 'sha256', '7cd9d07aec975212344dc6f7e45019378cf6775dd564181e269010a1eddaaec7'),
        'dotnet': ('https://builds.dotnet.microsoft.com/dotnet/Runtime/10.0.12/dotnet-runtime-10.0.12-win-x64.zip', 'sha512', '844fa99e16fd6f44e0a7c29def7a82d7846902334d6a955248a9519a4dddb3f5acceb9c9223bef69f8c83b8ae2417537e5b76dddf79fb7117dc85b5039bc1297'),
    },
    'linux': {
        'python': ('https://github.com/astral-sh/python-build-standalone/releases/download/20261110/cpython-3.14.10%2B20261110-x86_64-unknown-linux-gnu-install_only_stripped.tar.gz', 'sha256', 'c2ff2e97422f5d3896a16e0317fd212ba977bbf2688d93a6f7a03d817506bd30'),
        'dotnet': ('https://builds.dotnet.microsoft.com/dotnet/Runtime/10.0.12/dotnet-runtime-10.0.12-linux-x64.tar.gz', 'sha512', '58388fdde4f13bd703c7a6f7defb3300b17e42ba5fe8bca50066f80f64ac7406620dcdfb4acc1eff7992750c8cc5cff8369dd12d09730c8a9e8760d6032f7f6e'),
    },
}

QT_SOURCES = {
    'qtbase': '56001b905601bb9023d399f3ba780d7fa940f3e4861e496a7c490331f49e0b80',
    'qtimageformats': '049bfb99845e4801672aca07c3c4fc4c140f932a3a33faa899419579e33ef1c8',
    'qtsvg': '35eb516460f00f264eb504baa253432384351cf23fb9980a5857190e8deef438',
}
SPIRV = {
    'windows': ('win-64/spirv-tools-2026.4-h49e36cd_2.conda', '933e40fe9ca887a63ef742260f0ed801cda317354806504d864764a4ed9749e1'),
    'linux': ('linux-64/spirv-tools-2026.4-h7148c6a_2.conda', 'e8791349946b2c98ffa5cc67cf1525931e066aef8a01800d6f7c151444a3f052'),
}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--dest', type=Path, required=True)
    args = ap.parse_args()
    dest = args.dest.resolve(); dest.mkdir(parents=True, exist_ok=True)
    platform = 'windows' if os.name == 'nt' else 'linux'
    manifest = {'platform': platform, 'downloads': {}}
    for module, sha in QT_SOURCES.items():
        name = module + '-everywhere-src-6.8.3.tar.xz'
        url = 'https://download.qt.io/archive/qt/6.8/6.8.3/submodules/' + name
        data = deps.fetch(url, sha, str(dest / 'sources' / name))
        with tarfile.open(fileobj=io.BytesIO(data)) as archive:
            for member in archive.getmembers():
                p = Path(member.name)
                if member.isfile() and len(p.parts) > 2 and p.parts[1] == 'LICENSES':
                    out = dest / 'licenses/qt' / module / p.name
                    out.parent.mkdir(parents=True, exist_ok=True)
                    out.write_bytes(archive.extractfile(member).read())
        manifest['downloads'][module] = {'url': url, 'sha256': sha}
    for name, (url, algorithm, digest) in ASSETS[platform].items():
        target = dest / name
        stamp = dest / (name + '.download.json')
        record = {'url': url, algorithm: digest}
        if not stamp.exists() or json.loads(stamp.read_text()) != record:
            if target.exists(): raise SystemExit('Remove the incomplete dependency directory before retrying: ' + str(target))
            print('Downloading ' + url, flush=True)
            with urllib.request.urlopen(url, timeout=180) as response: data = response.read()
            if hashlib.new(algorithm, data).hexdigest() != digest: raise SystemExit('Checksum mismatch: ' + url)
            if platform == 'windows':
                with zipfile.ZipFile(io.BytesIO(data)) as archive:
                    for item in archive.infolist():
                        path = Path(item.filename)
                        if path.is_absolute() or '..' in path.parts: raise ValueError('Unsafe archive path')
                        if name == 'python':
                            if not item.filename.startswith('tools/') or item.is_dir(): continue
                            path = Path(*path.parts[1:])
                        elif item.is_dir(): continue
                        out = target / path; out.parent.mkdir(parents=True, exist_ok=True)
                        out.write_bytes(archive.read(item))
            else:
                with tarfile.open(fileobj=io.BytesIO(data)) as archive:
                    # terminfo contains case-distinct symlinks which loop on NTFS; the toolkit does not use curses
                    members = [m for m in archive.getmembers() if not m.name.startswith('python/share/terminfo/')]
                    archive.extractall(dest if name == 'python' else target, members=members, filter='data')
            stamp.write_text(json.dumps(record, indent=2) + '\n')
        manifest['downloads'][name] = record
    # use the bundled 3.14 interpreter, including its built-in zstd decompressor
    python = dest / ('python/python.exe' if os.name == 'nt' else 'python/bin/python3')
    code = "import sys; sys.path.insert(0, sys.argv[1]); import get_deps as d; d.HERE=sys.argv[2]; d.DEST=d.os.path.join(d.HERE,'gcc'); d.CC=d.os.path.join(d.DEST,'Library','bin','x86_64-w64-mingw32-gcc.exe') if d.WIN else d.os.path.join(d.DEST,'bin','x86_64-conda-linux-gnu-gcc'); d.LIB=d.os.path.join(d.HERE,'lib'); sys.exit(d.install_gcc(force=True) | (0 if d.WIN else d.install_sdl()))"
    subprocess.run([str(python), '-c', code, str(ROOT / 'tool'), str(dest)], check=True)
    package, sha = SPIRV[platform]
    code = "import sys; sys.path.insert(0,sys.argv[1]); import get_deps as d; d.DEST=sys.argv[2]; d.os.makedirs(d.DEST,exist_ok=True); d.relocate(d.unpack(d.get(sys.argv[3],sys.argv[4]),sys.argv[3].split('/')[-1]),d.DEST)"
    subprocess.run([str(python), '-c', code, str(ROOT / 'tool'), str(dest / 'shader-tools'), package, sha], check=True)
    manifest['downloads']['spirv-tools'] = {'url': deps.URL + package, 'sha256': sha}
    manifest['downloads']['gcc'] = {deps.URL + p: {'sha256': sha} for p, sha in deps.PKGS.items()}
    if platform == 'linux': manifest['downloads']['sdl'] = {'url': deps.SDL_WHEEL[0], 'sha256': deps.SDL_WHEEL[1]}
    (dest / 'dependency-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')


if __name__ == '__main__': main()
