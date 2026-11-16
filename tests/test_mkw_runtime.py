# SwitchRecomp - Copyright 2026 MZ Labs
# Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
# Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen.
"""Compile the production SPIR-V edit and WinMM mapper, then validate real and constructed modules.

python tests/test_mkw_runtime.py [--baseline] [--shader path.spv]
Requires the bundled GCC and SPIRV-Tools. Evidence is kept in build/mkw-investigation/tests-*.
"""
import argparse
import ctypes
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import s2r_paths as P


def function(text, name):
    start = text.index(name)
    start = text.rfind('\n', 0, start) + 1
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


def main():
    ap = argparse.ArgumentParser(); ap.add_argument('--baseline', action='store_true'); ap.add_argument('--shader', type=Path)
    a = ap.parse_args()
    out = ROOT / 'build/mkw-investigation' / (time.strftime('tests-%Y%m%d-%H%M%S-') + str(time.time_ns() % 1000000000))
    out.mkdir(exist_ok=False)
    src = 'runtime/nvn/vk/vk_shader.c'
    text = subprocess.check_output(['git', 'show', 'HEAD:' + src], cwd=ROOT, text=True) if a.baseline else (ROOT / src).read_text()
    enums = '\n'.join(re.findall(r'^enum \{ OP_.*?\};', text, re.S | re.M))
    # compile only the pure production functions; no replacement implementation of the edit
    code = '#include <stdint.h>\n#include <stdlib.h>\n#include <string.h>\n' + enums + '\n'
    code += 'typedef struct { uint32_t *w; size_t n, cap; } wbuf;\n'
    for name in ('spv_ok(', 'wput(', 'spv_outputs(', 'vkspv_int_outputs('): code += function(text, name) + '\n'
    code += '__declspec(dllexport) void release(void *p){free(p);}\n'
    backend = (ROOT / 'runtime/backend_win32.c').read_text()
    code += 'typedef uint32_t DWORD; typedef uint64_t u64;\n'
    code += function(backend, 'joy_buttons(').replace('static u64', '__declspec(dllexport) u64')
    key = re.search(r'uint64_t key = (.*?);', text[text.index('static const vks_shader *fs_variant_l'):]).group(1)
    code += '\n__declspec(dllexport) uint64_t variant_key(int flags){return ' + key + ';}\n'
    code = code.replace('uint32_t *vkspv_int_outputs(', '__declspec(dllexport) uint32_t *vkspv_int_outputs(')
    c = out / 'production.c'; c.write_text(code)
    dll = out / 'production.dll'
    cc = Path(P.gcc_bin()) / 'x86_64-w64-mingw32-gcc.exe'
    os.environ['PATH'] = str(cc.parent) + os.pathsep + os.environ['PATH']
    subprocess.run([str(cc), '-shared', '-O0', str(c), '-o', str(dll)], check=True)
    lib = ctypes.CDLL(str(dll))
    lib.variant_key.argtypes = [ctypes.c_int]; lib.variant_key.restype = ctypes.c_uint64
    if not a.baseline:
        assert lib.variant_key(4) != lib.variant_key(0x104), 'target 3 collides with the key tag'
        print('PASS fragment variant key distinguishes target 3')
    lib.joy_buttons.argtypes = [ctypes.c_uint32, ctypes.c_int]; lib.joy_buttons.restype = ctypes.c_uint64
    for sony in (0, 1):
        for raw, hid in ((4, 6), (5, 7), (10, 4), (11, 5), (6, 8), (7, 9), (3, 2), (0, 3)):
            assert lib.joy_buttons(1 << raw, sony) == 1 << hid
    # by position on every pad (button_layout maps to the guest later): circle (2) = A position, cross (1) = B position
    assert lib.joy_buttons(4, 1) == 1 and lib.joy_buttons(2, 1) == 2
    assert lib.joy_buttons(1 << 9, 1) == 1 << 10
    assert lib.joy_buttons(0, 1) == 0
    print('PASS WinMM shoulders, stick clicks, triggers, face buttons, Options, neutral')
    u32 = ctypes.c_uint32
    lib.vkspv_int_outputs.argtypes = [ctypes.POINTER(u32), ctypes.c_size_t, ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_size_t)]
    lib.vkspv_int_outputs.restype = ctypes.POINTER(u32)
    lib.release.argtypes = [ctypes.c_void_p]
    sdk = Path('C:/VulkanSDK/1.3.268.0/Bin')
    assembler = P.spirv_tool('spirv-as') or str(sdk / 'spirv-as.exe')
    validator = P.spirv_tool('spirv-val') or str(sdk / 'spirv-val.exe')
    assembly = '''OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint Fragment %main "main" %out %out1
OpExecutionMode %main OriginUpperLeft
OpDecorate %out Location 0
OpDecorate %out1 Location 1
%void = OpTypeVoid
%fn = OpTypeFunction %void
%uint = OpTypeInt 32 0
%int = OpTypeInt 32 1
%float = OpTypeFloat 32
%vec = OpTypeVector %float 4
%ptr = OpTypePointer Output %vec
%scalar = OpTypePointer Output %float
%zero = OpConstant %uint 0
%seven = OpConstant %uint 7
%neg = OpConstant %uint 4294967295
%out = OpVariable %ptr Output
%out1 = OpVariable %ptr Output
%main = OpFunction %void None %fn
%label = OpLabel
%bits = OpBitcast %float %seven
%sign = OpBitcast %float %neg
%v = OpCompositeConstruct %vec %bits %sign %bits %sign
OpStore %out %v
%p = OpAccessChain %scalar %out %zero
OpStore %p %bits
%read = OpLoad %float %p
%readv = OpLoad %vec %out
OpStore %out1 %readv
OpStore %p %read
OpReturn
OpFunctionEnd
'''
    asm = out / 'mixed.spvasm'; asm.write_text(assembly)
    inp = out / 'mixed.spv'
    subprocess.run([assembler, '--target-env', 'vulkan1.1', str(asm), '-o', str(inp)], check=True)
    cases = [(inp, (1, 2)), (inp, (2, 1))]
    if a.shader: cases.append((a.shader, (1, 0)))
    failures = 0
    for idx, (path, kinds) in enumerate(cases):
        data = path.read_bytes(); words = (u32 * (len(data)//4)).from_buffer_copy(data)
        n = ctypes.c_size_t(); flags = (ctypes.c_int * 8)(*kinds)
        result = lib.vkspv_int_outputs(words, len(words), flags, ctypes.byref(n))
        assert result, 'edit rejected input'
        raw = ctypes.string_at(result, n.value*4); lib.release(result)
        dest = out / f'case-{idx}.spv'; dest.write_bytes(raw)
        check = subprocess.run([validator, '--target-env', 'vulkan1.3', str(dest)], capture_output=True, text=True)
        (out / f'case-{idx}.validation.txt').write_text(check.stdout + check.stderr)
        if check.returncode:
            failures += 1; print('FAIL', path.name, check.stderr.strip())
        else:
            w = struct.unpack(f'<{len(raw)//4}I', raw); at = 5; ops = []
            while at < len(w): ops.append(w[at] & 0xffff); at += w[at] >> 16
            assert 109 not in ops and 110 not in ops, 'numeric conversion destroys register bits'
            print('PASS', path.name, 'valid SPIR-V, bit-preserving stores and load round-trip')
    print('Evidence:', out)
    return bool(failures)


if __name__ == '__main__': sys.exit(main())
