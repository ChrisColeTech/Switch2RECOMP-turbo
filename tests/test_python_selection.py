#!/usr/bin/env python3
# Copyright 2026 ChrisColeTech
# SPDX-License-Identifier: MIT
"""Exercise interpreter selection with disposable paths, without installing tools or running games."""
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]


def paths_module(relative):
    spec = importlib.util.spec_from_file_location('paths_under_test', ROOT / relative)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class PythonSelection(unittest.TestCase):
    def test_helpers_precedence_and_invalid_override(self):
        for relative in ('tools/s2r_paths.py', 'tools/nxshader/s2r_paths.py'):
            module = paths_module(relative)
            nested = 'nxshader' in relative
            self.assertEqual(Path(module.ROOT), ROOT / 'tools' if nested else ROOT)
            with tempfile.TemporaryDirectory(prefix='python paths with spaces ') as tmp:
                module.ROOT = str(Path(tmp) / 'tools') if nested else tmp
                suffix = 'Scripts/python.exe' if os.name == 'nt' else 'bin/python3'
                choices = [str(Path(tmp) / v / suffix) for v in ('.venv', 'venv')]
                choices += [str(Path(tmp) / ('python/python.exe' if os.name == 'nt' else 'python/bin/python3')), sys.executable]
                available = set(choices)
                with patch.dict(os.environ, {}, clear=True), patch.object(module.shutil, 'which', side_effect=lambda p: p if p in available else None), patch.object(module.subprocess, 'run', return_value=subprocess.CompletedProcess([], 0)) as run:
                    for expected in choices:
                        self.assertEqual(module.python(), os.path.abspath(expected))
                        self.assertEqual(run.call_args.args[0][0], expected)
                        available.remove(expected)
                    os.environ['SWR_PYTHON'] = choices[-1]
                    available.update(choices)
                    self.assertEqual(module.python(), os.path.abspath(choices[-1]))
                    os.environ['SWR_PYTHON'] = 'missing python'
                    with self.assertRaisesRegex(RuntimeError, 'SWR_PYTHON'): module.python()

    @unittest.skipUnless(os.name == 'nt', 'Windows batch selection')
    def test_batch_runs_selected_python_in_paths_with_spaces(self):
        with tempfile.TemporaryDirectory(prefix='swr python spaces ') as tmp:
            root = Path(tmp)
            (root / 'scripts').mkdir()
            shutil.copyfile(ROOT / 'scripts/_python.bat', root / 'scripts/_python.bat')
            # setup-python is a base install on CI; create redirectors without pip or network access.
            bootstrap = root / 'bootstrap'
            subprocess.run([sys.executable, '-m', 'venv', '--without-pip', str(bootstrap)], check=True)
            # disposable redirectors use the already installed base interpreter
            choices = [root / v / 'Scripts/python.exe' for v in ('.venv', 'venv')]
            choices += [root / 'python/python.exe', root / 'path/python.exe']
            for executable in choices:
                executable.parent.mkdir(parents=True)
                shutil.copyfile(bootstrap / 'Scripts/python.exe', executable)
                (executable.parent.parent / 'pyvenv.cfg').write_text('home = ' + str(Path(sys._base_executable).parent) + '\n', encoding='utf-8')
            env = dict(os.environ)
            env.pop('SWR_PYTHON', None)
            env['PATH'] = str(root / 'path') + os.pathsep + env['PATH']
            def resolve():
                return subprocess.run(['cmd.exe', '/d', '/c', 'call scripts\\_python.bat && call echo %SWR_PYTHON%'], cwd=root, env=env, capture_output=True, text=True, timeout=30)
            for expected in choices:
                result = resolve()
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertTrue(expected.samefile(result.stdout.strip()), result.stdout)
                self.assertEqual(subprocess.run([str(expected), '-c', 'import sys; assert sys.version_info >= (3, 8)']).returncode, 0)
                if expected == choices[0]:
                    env['SWR_PYTHON'] = str(choices[2])
                    self.assertTrue(choices[2].samefile(resolve().stdout.strip()))
                    env['SWR_PYTHON'] = str(root / 'missing.exe')
                    self.assertNotEqual(resolve().returncode, 0)
                    env.pop('SWR_PYTHON')
                expected.unlink()

    @unittest.skipIf(os.name == 'nt', 'POSIX shell selection')
    def test_shell_selection_in_paths_with_spaces(self):
        with tempfile.TemporaryDirectory(prefix='swr shell spaces ') as tmp:
            root = Path(tmp)
            choices = [root / v / 'bin/python3' for v in ('.venv', 'venv', 'python')]
            for executable in choices:
                executable.parent.mkdir(parents=True)
                executable.symlink_to(sys.executable)
            env = dict(os.environ); env.pop('SWR_PYTHON', None)
            def resolve():
                return subprocess.run(['sh', '-c', 'root="$1"; . "$2" || exit $?; printf "%s\\n" "$python"',
                                       'selection', str(root), str(ROOT / 'scripts/_env.sh')],
                                      env=env, capture_output=True, text=True, timeout=30)
            for expected in choices:
                result = resolve()
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout.strip(), str(expected))
                env['SWR_PYTHON'] = sys.executable
                self.assertEqual(resolve().stdout.strip(), sys.executable)
                env['SWR_PYTHON'] = str(root / 'missing')
                self.assertNotEqual(resolve().returncode, 0)
                env.pop('SWR_PYTHON'); expected.unlink()
            result = resolve()
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout.strip(), 'python3')

    @unittest.skipUnless(os.name == 'nt', 'Windows runtime selection')
    def test_runtime_translator_selection_without_runtime_build(self):
        cc = ROOT / 'gcc/Library/bin/x86_64-w64-mingw32-gcc.exe'
        if not cc.is_file(): cc = ROOT / 'build/release-deps/gcc/Library/bin/x86_64-w64-mingw32-gcc.exe'
        if not cc.is_file(): self.skipTest('GCC not installed')
        source = (ROOT / 'runtime/nvn_hook.c').read_text(encoding='utf-8')
        start = source.index('    { const char* override=getenv("SWR_PYTHON")')
        end = source.index('    if(!sc[0] || getenv("SWR_NO_NXS_LIVE"))', start)
        selection = source[start:end]
        with tempfile.TemporaryDirectory(prefix='translator paths with spaces ') as tmp:
            root = Path(tmp)
            game = root / 'build/Game with spaces'; game.mkdir(parents=True)
            script = game / 'tools/nxshader/nxs_live.py'; script.parent.mkdir(parents=True); script.touch()
            harness = root / 'selection.c'
            harness.write_text('#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n'
                               'int main(int argc,char** argv){const char* g_exedir=argv[1];'
                               'const char* up[]={"","../","../../","../../../"};'
                               'char py[1400]="",sc[1400],x[1400];snprintf(sc,sizeof sc,"%s",argv[2]);\n'
                               + selection + '\nputs(py);return 0;}\n', encoding='utf-8')
            binary = root / 'selection.exe'
            compile_env = dict(os.environ, PATH=str(cc.parent) + os.pathsep + os.environ['PATH'])
            result = subprocess.run([str(cc), str(harness), '-o', str(binary)], env=compile_env, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            env = dict(os.environ); env.pop('SWR_PYTHON', None)
            def resolve():
                return subprocess.check_output([str(binary), str(game) + '/', str(script)], env=env, text=True).strip()
            venv = root / '.venv/Scripts/python.exe'; venv.parent.mkdir(parents=True); venv.touch()
            bundle = game / 'python/python.exe'; bundle.parent.mkdir(); bundle.touch()
            self.assertEqual(Path(resolve()).resolve(), venv.resolve())
            env['SWR_PYTHON'] = 'C:/explicit path/python.exe'; self.assertEqual(resolve(), env['SWR_PYTHON']); env.pop('SWR_PYTHON')
            venv.unlink(); self.assertEqual(Path(resolve()).resolve(), bundle.resolve())
            bundle.unlink()
            marker = script.with_name('python.path'); marker.write_text(sys.executable + '\n', encoding='utf-8')
            self.assertEqual(resolve(), sys.executable)
            marker.write_text('C:/missing/python.exe\n', encoding='utf-8'); self.assertEqual(resolve(), 'python')


if __name__ == '__main__': unittest.main()
