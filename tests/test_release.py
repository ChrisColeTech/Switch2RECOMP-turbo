#!/usr/bin/env python3
# SwitchRecomp - Copyright 2026 MZ Labs
# Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
# Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen.
"""Packaging contracts with disposable bundles; no downloads, runtime builds or games."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    result = importlib.util.module_from_spec(spec); spec.loader.exec_module(result)
    return result


package = module('package_release', 'scripts/package-release.py')
smoke = module('smoke_release', 'scripts/smoke-release.py')
dependencies = module('release_dependencies', 'scripts/release-deps.py')


class ReleaseChecks(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='release fixtures [spaces] ')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def bundle(self, platform):
        root = self.root / platform; root.mkdir()
        for name in package.required_files(platform):
            path = root / name; path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(b'fixture')
        for name in ('licenses/qt/qtbase', 'licenses/qt/qtimageformats', 'licenses/qt/qtsvg', 'gcc/licenses', 'shader-tools/licenses'):
            path = root / name; path.mkdir(parents=True, exist_ok=True); (path / 'LICENSE').write_text('fixture')
        runtime = root / ('bin/runtime_win64_win32.exe' if platform == 'windows' else 'bin/runtime_linux_x11')
        runtime.write_bytes(b'WebM Project VP9 Decoder\0frame telemetry:\0hid.dll\0setupapi.dll')
        vpx = root / 'runtime/vp9' / ('win' if platform == 'windows' else 'lin') / 'libvpx.a'
        manifest = {'platform': platform, 'downloads': {'libvpx': {'sha256': package.digest(vpx), 'sources': {
            p.name: {'sha256': package.digest(p)} for p in (root / 'sources').glob('libvpx*')}}}}
        (root / 'dependency-manifest.json').write_text(json.dumps(manifest))
        return root

    def sums(self, root):
        (root / 'SHA256SUMS').write_text(''.join(package.digest(p) + '  ' + p.relative_to(root).as_posix() + '\n'
            for p in sorted(root.rglob('*')) if p.is_file() and p.name != 'SHA256SUMS'))

    def test_complete_bundles_and_missing_new_files(self):
        for platform in ('windows', 'linux'):
            root = self.bundle(platform)
            package.validate_contents(root, platform)
            for name in ('runtime/controller_sony_win32.h', 'runtime/nvn/vk/vk_perf.c', 'runtime/nvn/vk/vk_perf.h',
                         'tools/nxshader/s2r_paths.py', 'tools/nxshader/xlate_bin/dmsh_xlate.runtimeconfig.json',
                         'sources/libvpx_1.14.0-1ubuntu2.3.debian.tar.xz'):
                path = root / name; data = path.read_bytes(); path.unlink()
                with self.assertRaises(FileNotFoundError, msg=name): package.validate_contents(root, platform)
                path.write_bytes(data)

    def test_runtime_requires_vp9_nvn_telemetry_and_sony(self):
        root = self.bundle('windows'); runtime = root / 'bin/runtime_win64_win32.exe'; data = runtime.read_bytes()
        for marker in (b'WebM Project VP9 Decoder', b'frame telemetry:', b'hid.dll', b'setupapi.dll'):
            runtime.write_bytes(data.replace(marker, b''))
            with self.assertRaisesRegex(RuntimeError, 'Native runtime feature'): package.validate_contents(root, 'windows')
        runtime.write_bytes(data)
        (root / 'runtime/vp9/win/libvpx.a').write_bytes(b'wrong target library')
        with self.assertRaisesRegex(RuntimeError, 'provenance'): package.validate_contents(root, 'windows')

    def test_private_inputs_rejected(self):
        root = self.bundle('windows')
        for name in ('docs/library.json', 'tools/python.path', 'runtime/prod.keys', 'runtime/cache.NSP',
                     'tools/saves/private.dat', '.venv/Scripts/python.exe', 'logs/session.txt', 'shader_cache/cache.bin'):
            path = root / name; path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(b'private')
            with self.assertRaisesRegex(RuntimeError, 'Personal/generated'): package.validate_contents(root, 'windows')
            path.unlink()
            while path.parent != root and not list(path.parent.iterdir()):
                path = path.parent; path.rmdir()

    def test_manifest_rejects_extra_missing_changed_and_duplicate_files(self):
        root = self.bundle('linux'); self.sums(root); smoke.verify_manifest(root)
        manifest = root / 'SHA256SUMS'; original = manifest.read_text()
        extra = root / 'extra.txt'; extra.write_text('unlisted')
        with self.assertRaisesRegex(RuntimeError, 'exactly'): smoke.verify_manifest(root)
        extra.unlink()
        notice = root / 'NOTICE.txt'; notice.unlink()
        with self.assertRaises(FileNotFoundError): smoke.verify_manifest(root)
        notice.write_text('changed')
        with self.assertRaisesRegex(RuntimeError, 'Checksum'): smoke.verify_manifest(root)
        notice.write_text('fixture')
        manifest.write_text(original + original.splitlines()[0] + '\n')
        with self.assertRaisesRegex(RuntimeError, 'duplicate'): smoke.verify_manifest(root)

    def test_relocated_archives_and_outer_checksum(self):
        for platform in ('windows', 'linux'):
            root = self.bundle(platform); self.sums(root)
            archive = self.root / (platform + ('.zip' if platform == 'windows' else '.tar.gz'))
            if platform == 'windows':
                with zipfile.ZipFile(archive, 'w') as z:
                    for p in root.rglob('*'):
                        if p.is_file(): z.write(p, p.relative_to(root.parent))
            else:
                with tarfile.open(archive, 'w:gz') as tar: tar.add(root, arcname=root.name)
            checksum = Path(str(archive) + '.sha256'); checksum.write_text(package.digest(archive) + '  ' + archive.name + '\n')
            dest = self.root / ('relocated ' + platform); dest.mkdir()
            extracted = smoke.extract_archive(archive, dest)
            smoke.verify_manifest(extracted); package.validate_contents(extracted, platform)
            checksum.write_text('0' * 64 + '  ' + archive.name + '\n')
            with self.assertRaisesRegex(RuntimeError, 'Archive checksum'): smoke.extract_archive(archive, dest)

    def test_unsafe_archive_and_manifest_paths(self):
        for name in ('../escape', '/absolute', 'C:/outside', 'bundle/../../escape', 'bundle\\escape', './alias', 'a//b'):
            with self.assertRaisesRegex(RuntimeError, 'Unsafe'): smoke.safe_name(name)
        archive = self.root / 'bad.zip'
        with zipfile.ZipFile(archive, 'w') as z: z.writestr('../escape', 'bad')
        Path(str(archive) + '.sha256').write_text(package.digest(archive) + '  bad.zip\n')
        dest = self.root / 'extract'; dest.mkdir()
        with self.assertRaisesRegex(RuntimeError, 'Unsafe'): smoke.extract_archive(archive, dest)
        self.assertEqual(list(dest.iterdir()), [])

    @unittest.skipIf(os.name == 'nt', 'Unix symlink archive contract')
    def test_external_and_broken_symlinks_rejected(self):
        root = self.bundle('linux'); link = root / 'outside'
        for target in (self.root / 'missing', self.root, root / 'NOTICE.txt'):
            link.symlink_to(target)
            with self.assertRaisesRegex(RuntimeError, 'Non-relocatable'): package.validate_contents(root, 'linux')
            link.unlink()

    @unittest.skipIf(os.name == 'nt', 'Unix shell contract')
    def test_fix_gcc_script_repoints_packaged_sysroot(self):
        old = self.root / 'packaging-machine-gcc'; (old / 'bin').mkdir(parents=True)
        root = self.bundle('linux')
        cfg = root / 'gcc/bin/x86_64-conda-linux-gnu.cfg'; cfg.parent.mkdir(parents=True, exist_ok=True)
        cfg.write_text('--sysroot=%s/x86_64-conda-linux-gnu/sysroot\n' % old)
        specs = root / 'gcc/lib/gcc/x86_64-conda-linux-gnu/16.2.0/specs'; specs.parent.mkdir(parents=True, exist_ok=True)
        specs.write_text('*link: -rpath %s/lib\n' % old)
        driver = root / 'gcc/bin/x86_64-conda-linux-gnu-gcc'
        driver.write_text('#!/bin/sh\nexit 42\n'); driver.chmod(0o755)
        script = root / 'fix-gcc.sh'; script.write_text(package.GCC_FIXUP); script.chmod(0o755)
        subprocess.run(['sh', script], check=True)
        self.assertEqual(cfg.read_text(), '')                                # never space-split again
        self.assertIn('/tmp/switchrecomp-toolchains/', specs.read_text())    # baked paths now use the alias
        wrapper = driver.read_text()
        self.assertIn('--sysroot="$a/x86_64-conda-linux-gnu/sysroot"', wrapper)
        self.assertTrue((root / 'gcc/bin/x86_64-conda-linux-gnu-gcc.bin').exists())
        subprocess.run(['sh', script], check=True)                           # idempotent
        self.assertEqual(driver.read_text(), wrapper)

    @unittest.skipIf(os.name == 'nt', 'Unix symlink archive contract')
    def test_dereference_replaces_nonrelocatable_links_with_target_copies(self):
        root = self.bundle('linux')
        (root / 'gcc/lib').mkdir(parents=True, exist_ok=True)
        real = root / 'gcc/lib/libgomp.so.1.0.0'; real.write_bytes(b'gomp')
        (root / 'gcc/lib/libgomp.so').symlink_to(real)             # relative, inside the stage
        (root / 'gcc/lib/absolute.so').symlink_to(self.root / 'outside-target')
        (self.root / 'outside-target').write_bytes(b'absolute')
        (root / 'gcc/lib/gcc-ghost.so').symlink_to(Path('/never-existed/lib/gcc-ghost.so.1.0.0'))  # toolchain-build path
        (root / 'gcc/lib/gcc-ghost.so.1.0.0').write_bytes(b'ghost')
        package.dereference_symlinks(root)
        self.assertFalse((root / 'gcc/lib/libgomp.so').is_symlink())
        self.assertEqual((root / 'gcc/lib/libgomp.so').read_bytes(), b'gomp')
        self.assertFalse((root / 'gcc/lib/absolute.so').is_symlink())
        self.assertEqual((root / 'gcc/lib/absolute.so').read_bytes(), b'absolute')
        self.assertFalse((root / 'gcc/lib/gcc-ghost.so').is_symlink())
        self.assertEqual((root / 'gcc/lib/gcc-ghost.so').read_bytes(), b'ghost')
        package.validate_contents(root, 'linux')                   # the fixed tree now validates
        (root / 'gcc/lib/libopenmp.so').symlink_to(Path('/never/libopenmp.so'))  # dev link, library not shipped
        package.dereference_symlinks(root)
        self.assertFalse((root / 'gcc/lib/libopenmp.so').exists())  # dangling dev link removed
        package.validate_contents(root, 'linux')
        (root / 'gcc/lib/suspicious.so').symlink_to(self.root / 'missing-dir/NOTICE.txt')  # basename IS shipped
        with self.assertRaisesRegex(RuntimeError, 'unreadable target'): package.dereference_symlinks(root)

    def test_source_allowlist_includes_new_headers_but_not_personal_data(self):
        names = ['runtime/controller_sony.h', 'runtime/nvn/vk/vk_perf.c', 'tools/nxshader/s2r_paths.py',
                 'tools/library.json', 'runtime/prod.keys', '.venv/Scripts/python.exe', 'games/Game.exe', 'performance.txt']
        for name in names:
            p = self.root / name; p.parent.mkdir(parents=True, exist_ok=True); p.touch()
        with patch.object(package, 'ROOT', self.root), patch.object(package.subprocess, 'check_output', return_value='\0'.join(names).encode()):
            self.assertEqual({p.as_posix() for p in package.source_files()}, set(names[:3]))

    def test_runtime_build_links_native_archive_and_vp9_on_both_platforms(self):
        import switchrecomp as compiler
        for win, backend, subdir, nvn in ((True, 'win32', 'win', 'nvn'), (False, 'x11', 'lin', 'nvn_lin')):
            rt = self.root / 'runtime'; vp9 = rt / 'vp9' / subdir / 'libvpx.a'
            vp9.parent.mkdir(parents=True, exist_ok=True); vp9.touch()
            native = self.root / 'build' / nvn / 'libnvn.a'; native.parent.mkdir(parents=True); native.touch()
            with patch.object(compiler, 'HERE', str(self.root)), patch.object(compiler, 'RT', str(rt)), \
                 patch.object(compiler, 'run_cc', return_value=None) as cc, patch.dict(os.environ, {}, clear=True):
                compiler.build_runtime('gcc', 'unused', win, backend, print)
            command = cc.call_args.args[0]
            for flag in ('-DSWR_HAVE_NVN', '-DSWR_HAVE_VP9', str(native), str(vp9)):
                self.assertIn(flag, command)
            self.assertIn(str(rt / 'audio.c'), command)
            self.assertIn(str(rt / 'svc_hid.c'), command)

    def test_linux_vp9_build_records_provenance_and_preserves_existing_library(self):
        dest = self.root / 'deps'; dest.mkdir()
        def fetch(url, sha, cache):
            path = Path(cache); path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(b'source fixture')
        def build(command, **kwargs):
            if command[0] == 'dpkg-source': Path(command[-1]).mkdir()
            if command[0] == 'make': (Path(kwargs['cwd']) / 'libvpx.a').write_bytes(b'compiled fixture')
        with patch.object(dependencies, 'ROOT', self.root), patch.object(dependencies.deps, 'fetch', side_effect=fetch), \
             patch.object(dependencies.subprocess, 'run', side_effect=build) as run, \
             patch.object(dependencies.subprocess, 'check_output', return_value='gcc fixture\n'):
            record = dependencies.prepare_vp9(dest, 'linux', 4)
            lib = self.root / 'runtime/vp9/lin/libvpx.a'
            self.assertEqual(record['sha256'], package.digest(lib))
            self.assertIn('--enable-pic', record['configure'])
            self.assertIn('--disable-vp9-encoder', record['configure'])
            self.assertEqual(record['compiler'], 'gcc fixture')
            self.assertEqual(run.call_args_list[0].args[0][0], 'dpkg-source')
            lib.write_bytes(b'existing local build')
            with self.assertRaisesRegex(RuntimeError, 'Preserving existing'):
                dependencies.prepare_vp9(dest, 'linux', 4)
            self.assertEqual(lib.read_bytes(), b'existing local build')


if __name__ == '__main__': unittest.main()
