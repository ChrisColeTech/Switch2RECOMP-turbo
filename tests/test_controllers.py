# Copyright 2026 ChrisColeTech
# SPDX-License-Identifier: MIT
"""Isolated production controller tests and optional HID probe. Never builds/launches the runtime.

.venv/Scripts/python tests/test_controllers.py [--device | --pulse | --rumble | --buttons [SECONDS]]
Evidence stays in a unique build/controllers-* directory. --device is shared read-only.
--rumble drives the connected pad's lights and both motors for about 5 s (the owner's physical check).
--buttons prints, for every physical button pressed, the Switch button it becomes with button_layout=switch / xbox.
"""
import argparse
import ctypes as C
import os
from pathlib import Path
import subprocess
import sys
import time
import zlib

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import s2r_paths as P


def main():
    ap = argparse.ArgumentParser()
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument('--device', action='store_true')
    mode.add_argument('--pulse', action='store_true')
    mode.add_argument('--rumble', action='store_true')
    mode.add_argument('--buttons', nargs='?', const=20, type=int, metavar='SECONDS')
    args = ap.parse_args()
    out = ROOT / 'build' / ('controllers-' + time.strftime('%Y%m%d-%H%M%S-') + str(time.time_ns() % 1000000000))
    out.mkdir(parents=True)
    env = P.env_with_gcc()
    cc = P.gcc_tool('gcc')
    log = []

    def run(cmd):
        r = subprocess.run([str(x) for x in cmd], cwd=ROOT, env=env, capture_output=True, text=True)
        log.append('$ ' + ' '.join(str(x) for x in cmd) + '\n' + r.stdout + r.stderr)
        (out / 'checks.log').write_text('\n'.join(log), encoding='utf-8')
        print(r.stdout + r.stderr, end='')
        r.check_returncode()

    print('Python:', sys.executable, '\nEvidence:', out)
    # compile the full changed translation units without linking a game or touching runtime objects
    for file in ('backend_win32.c', 'svc_hid.c'):
        run([cc, '-std=gnu11', '-fsyntax-only', '-Werror=implicit-function-declaration', ROOT / 'runtime' / file])
    exe = out / 'controller_device.exe'
    run([cc, '-std=gnu11', '-O2', ROOT / 'tests/controller_device.c', '-o', exe])
    if args.buttons is not None:
        sys.exit(subprocess.run([str(exe), '--buttons', str(args.buttons)], env=env).returncode)  # live output
    if args.device or args.pulse or args.rumble:
        run([exe] + (['--pulse'] if args.pulse else ['--rumble'] if args.rumble else []))
        return
    # regression (2026-11-16): the output open must share write access. Denying FILE_SHARE_WRITE fails with
    # ERROR_SHARING_VIOLATION whenever any other handle on the pad can write, which left the owner's DualSense
    # read-only (no lights, no rumble) in every session. --device/--rumble prove the real open.
    win32 = (ROOT / 'runtime/controller_sony_win32.h').read_text()
    assert 'GENERIC_READ|(output?GENERIC_WRITE:0),FILE_SHARE_READ|FILE_SHARE_WRITE' in win32
    wrapper = out / 'protocol.c'
    wrapper.write_text('''#include "controller_sony.h"
__declspec(dllexport) size_t packet(int m,int t,unsigned seq,unsigned l,unsigned h,int lit,int reset,uint8_t* p,size_t n){return sony_output(m,t,seq,l,h,lit,reset,p,n);}
__declspec(dllexport) size_t packet_v(int m,int t,unsigned seq,unsigned l,unsigned h,int lit,int reset,int v2,uint8_t* p,size_t n){return sony_output_v(m,t,seq,l,h,lit,reset,v2,p,n);}
__declspec(dllexport) int vib_v2(unsigned pid,const uint8_t* f,size_t n){return sony_vibration_v2(pid,f,n);}
__declspec(dllexport) int input(int m,int t,const uint8_t* p,size_t n,SonyState* s){return sony_input(m,t,p,n,s);}
__declspec(dllexport) int model(unsigned v,unsigned p){return sony_model(v,p);}
__declspec(dllexport) int transport(int m,unsigned i,unsigned o){return sony_transport(m,i,o);}
''')
    dll = out / 'protocol.dll'
    run([cc, '-std=gnu11', '-shared', '-O2', '-I', ROOT / 'runtime', wrapper, '-o', dll])
    lib = C.CDLL(str(dll))
    byte = C.c_uint8
    class State(C.Structure):
        _fields_ = [('buttons', C.c_uint64)] + [(v, C.c_int32) for v in ('lx', 'ly', 'rx', 'ry')]
    lib.packet.argtypes = [C.c_int, C.c_int] + [C.c_uint]*3 + [C.c_int]*2 + [C.POINTER(byte), C.c_size_t]
    lib.packet.restype = C.c_size_t
    lib.packet_v.argtypes = [C.c_int, C.c_int] + [C.c_uint]*3 + [C.c_int]*3 + [C.POINTER(byte), C.c_size_t]
    lib.packet_v.restype = C.c_size_t
    lib.vib_v2.argtypes = [C.c_uint, C.POINTER(byte), C.c_size_t]
    def fw(version, rid=0x20, n=64):
        f = bytearray(n); f[0] = rid
        if n >= 46: f[44:46] = version.to_bytes(2, 'little')
        return (byte*n).from_buffer_copy(f), n
    # DualSense firmware feature 0x20: update version at 44 (Linux hid-playstation: v2 from 2.21, Edge always)
    assert lib.vib_v2(0xce6, *fw(0x0214)) == 0 and lib.vib_v2(0xce6, *fw(0x0215)) == 1 and lib.vib_v2(0xce6, *fw(0x0630)) == 1
    assert lib.vib_v2(0xdf2, *fw(0x0100)) == 1
    assert lib.vib_v2(0xce6, None, 0) == 1 and lib.vib_v2(0xce6, *fw(0x0100, rid=0x21)) == 1 and lib.vib_v2(0xce6, *fw(0x0100, n=40)) == 1
    lib.input.argtypes = [C.c_int, C.c_int, C.POINTER(byte), C.c_size_t, C.POINTER(State)]
    for vid, pid, expected in ((0x54c, 0xce6, 5), (0x54c, 0xdf2, 5), (0x54c, 0x5c4, 4), (0x54c, 0x9cc, 4), (1, 0xce6, 0), (0x54c, 1, 0)):
        assert lib.model(vid, pid) == expected
    for model in (4, 5):
        for bus in (1, 2):
            n = 78 if bus == 2 else 48 if model == 5 else 32
            assert lib.transport(model, 78 if bus == 2 else 64, n) == bus
            assert lib.transport(model, 64, 78) == 0
            p = (byte*80)(*([0xcd]*80))
            for size in (0, n-1):
                assert not lib.packet(model, bus, 0, 1, 2, 1, 0, p, size)
                assert bytes(p) == b'\xcd'*80
            for seq in (0, 1, 15, 16, 255):
                assert lib.packet(model, bus, seq, 999, 16, 1, 0, p, 80) == n
                assert bytes(p)[n:] == b'\xcd'*(80-n)
                off = 3 if bus == 2 else 1
                assert p[0] == ({(4, 1): 5, (4, 2): 0x11, (5, 1): 2, (5, 2): 0x31}[model, bus])
                if model == 5:
                    assert bytes(p)[off:off+4] == bytes([3, 0x14, 16, 255])
                    assert bytes(p)[off+43:off+47] == bytes([4, 0, 40, 128])
                    if bus == 2: assert p[1] == (seq & 15)*16 and p[2] == 0x10
                else:
                    assert p[off] == 7 and bytes(p)[off+3:off+8] == bytes([16, 255, 0, 40, 128])
                    if bus == 2: assert p[1] == 0xc4
                if bus == 2:
                    assert int.from_bytes(bytes(p)[74:78], 'little') == zlib.crc32(b'\xa2' + bytes(p)[:74])
            # v2 rumble: same packet plus valid_flag2 COMPATIBLE_VIBRATION2 (0x04) at common offset 38 on DualSense only
            q = (byte*80)()
            assert lib.packet_v(model, bus, 3, 999, 16, 1, 0, 1, q, 80) == n and lib.packet_v(model, bus, 3, 999, 16, 1, 0, 0, p, 80) == n
            diff = [i for i in range(n) if p[i] != q[i]]
            assert diff == ([off+38] + ([74, 75, 76, 77] if bus == 2 else []) if model == 5 else []), diff
            if model == 5: assert q[off+38] == 4 and q[off] == 3
            if bus == 2: assert int.from_bytes(bytes(q)[74:78], 'little') == zlib.crc32(bytes([0xa2]) + bytes(q)[:74])
            assert lib.packet(model, bus, 0, 0, 0, 0, 0, p, 80) == n
            assert p[off+(2 if model == 5 else 3)] == p[off+(3 if model == 5 else 4)] == 0
            if model == 5:
                lib.packet(model, bus, 0, 0, 0, 0, 1, p, 80)
                assert p[off+38] == p[off+41] == 2 and p[off] == p[off+1] == 0
            # independent physical-button fixtures for full reports and simple Bluetooth mode
            for simple in ([False, True] if bus == 2 else [False]):
                raw = bytearray(78 if bus == 2 else 64)
                raw[0] = 1 if bus == 1 or simple else 0x31 if model == 5 else 0x11
                off = 1 if bus == 1 or simple else 2 if model == 5 else 3
                bo = off + (7 if model == 5 and not simple else 4)
                raw[off:off+4] = b'\x80'*4
                raw[bo] = 8

                def parse(data, length=None):
                    data = bytearray(data)
                    if bus == 2 and not simple:
                        data[74:78] = zlib.crc32(b'\xa1' + data[:74]).to_bytes(4, 'little')
                    state = State(0xdead, 1, 2, 3, 4)
                    r = lib.input(model, bus, (byte*len(data)).from_buffer_copy(data), len(data) if length is None else length, C.byref(state))
                    return r, state

                r, s = parse(raw)
                assert r and (s.buttons, s.lx, s.ly, s.rx, s.ry) == (0, 0, 0, 0, 0)
                # by position (pad_layout.h maps to the guest): circle = A position, cross = B, triangle = X, square = Y
                for bit, target in ((6, 0), (5, 1), (7, 2), (4, 3)):
                    data = raw.copy(); data[bo] |= 1 << bit
                    assert parse(data)[1].buttons == 1 << target
                for bit, target in ((0, 6), (1, 7), (2, 8), (3, 9), (4, 11), (5, 10), (6, 4), (7, 5)):
                    data = raw.copy(); data[bo+1] = 1 << bit
                    assert parse(data)[1].buttons == 1 << target
                for hat, targets in enumerate(((13,), (13,14), (14,), (14,15), (15,), (15,12), (12,), (12,13), (), (), (), (), (), (), (), ())):
                    data = raw.copy(); data[bo] = hat
                    assert parse(data)[1].buttons == sum(1 << t for t in targets)
                data = raw.copy(); data[off:off+4] = bytes([0, 0, 255, 255])
                s = parse(data)[1]
                assert (s.lx, s.ly, s.rx, s.ry) == (-32767, 32767, 32767, -32767)
                for size in (0, 1, 9):
                    r, s = parse(raw, size)
                    assert not r and s.buttons == 0xdead
                if bus == 2 and not simple:
                    corrupt = (byte*78).from_buffer_copy(raw); corrupt[74] ^= 1
                    assert not lib.input(model, bus, corrupt, 78, C.byref(State()))
    print('PASS production Sony model/transport, button/hat/axis fixtures, packet bounds, motor stop, lights, DualSense rumble v1/v2 + firmware gate, Bluetooth sequence and independent zlib CRC')
    log.append('PASS Sony protocol fixtures (synthetic, not physical button validation)\n')
    svc_exe = out / 'controller_service.exe'
    run([cc, '-std=gnu11', '-O1', '-ffunction-sections', '-fdata-sections', ROOT / 'tests/controller_service.c', '-Wl,--gc-sections', '-o', svc_exe])
    run([svc_exe])
    backend = (ROOT / 'runtime/backend_win32.c').read_text()
    # Keep the real globals and functions together, excluding window/GPU code from this executable.
    begin = backend.index('/* ---------------------------------------------------------------- XInput,')
    end = backend.index('static int joy_poll(')
    (out / 'controller_backend_production.h').write_text('#include "rt.h"\n#include <math.h>\n#include <mmsystem.h>\n#include "controller_sony_win32.h"\n#include "pad_layout.h"\n' + backend[begin:end])
    backend_exe = out / 'controller_backend.exe'
    run([cc, '-std=gnu11', '-O1', '-I', out, '-I', ROOT / 'runtime', '-ffunction-sections', '-fdata-sections', ROOT / 'tests/controller_backend.c', '-Wl,--gc-sections', '-o', backend_exe])
    run([backend_exe])
    (out / 'checks.log').write_text('\n'.join(log), encoding='utf-8')


if __name__ == '__main__':
    main()
