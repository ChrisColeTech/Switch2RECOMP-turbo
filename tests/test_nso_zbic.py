#!/usr/bin/env python3
# Copyright 2026 ChrisColeTech
# SPDX-License-Identifier: MIT
"""ZBIC NSO segments (Switch 2 zstd variant); creates no copyrighted game data.

zbic_sample.zbic is sample() below compressed at level 19 by the reference ZBIC encoder (pip package zbic 1.0.1,
https://github.com/kinnay/zbic). It has Huffman weight tables, all three sequence tables in both normal and
low-probability form, and several blocks.
"""
import hashlib
import struct
import sys
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
import nxloader
import zbic

SAMPLE = Path(__file__).with_name('zbic_sample.zbic')


def sample(n=70000):
    words = [b'link', b'zelda', b'ocarina', b'temple', b'rupee', b'sword', b'shield', b'deku', b'hyrule', b'navi',
             b'ganon', b'epona', b'castle', b'forest', b'water', b'fire', b'shadow', b'spirit', b'time', b'song']
    x = 12345
    out = bytearray()
    while len(out) < n:
        x = (x * 1103515245 + 12345) & 0x7FFFFFFF
        w = words[(x >> 8) % len(words) if x & 3 else (x >> 8) % 4]
        out += w + (b' ' if x & 16 else bytes([48 + (x >> 20) % 10, 10]))
    return bytes(out[:n]) + bytes(4096)


def zbic_nso(segment, size, flags=0xff):
    """NSO whose three segments are the same ZBIC data, mapped one after another."""
    data = bytearray(0x100)
    data[:4] = b'NSO0'
    struct.pack_into('<I', data, 0xC, flags)
    for i in range(3):
        struct.pack_into('<III', data, 0x10 + i * 0x10, 0x100 + i * len(segment), i * size, size)
        struct.pack_into('<I', data, 0x60 + i * 4, len(segment))
    data[0x40:0x44] = b'zbic'
    data[0xA0:0x100] = hashlib.sha256(sample()).digest() * 3
    return bytes(data) + segment * 3


class NoZstd:
    """import hook that hides every zstd decoder"""
    def find_spec(self, name, path=None, target=None):
        if name.split('.')[0] in ('backports', 'compression', 'zstandard'):
            raise ImportError(name)


class ZbicTests(unittest.TestCase):
    def setUp(self):
        self.data = SAMPLE.read_bytes()
        self.expected = sample()

    def test_decompress_matches_reference(self):
        self.assertTrue(zbic.is_zbic(self.data))
        self.assertEqual(zbic.decompress(self.data, len(self.expected)), self.expected)

    def test_nso_with_zbic_segments_loads_and_hash_checks(self):
        module = nxloader.load_nso_data(zbic_nso(self.data, len(self.expected)), 'game:main', 'main')
        self.assertEqual(module.image[:len(self.expected)], self.expected)
        self.assertEqual(module.image[2 * len(self.expected):], self.expected)

    def test_without_zstd_says_not_supported(self):
        hidden = {k: None for k in list(sys.modules) if k.split('.')[0] in ('backports', 'compression', 'zstandard')}
        with mock.patch.dict(sys.modules, hidden), mock.patch.object(sys, 'meta_path', [NoZstd()] + sys.meta_path):
            with self.assertRaises(ValueError) as caught:
                nxloader.load_nso_data(zbic_nso(self.data, len(self.expected)), 'game:main', 'main')
        message = str(caught.exception)
        self.assertIn('segment 0 uses ZBIC compression', message)
        self.assertIn('not supported', message)
        self.assertIn('pip install zstandard', message)
        self.assertNotIn('dump the game again', message)

    def test_damaged_zbic_is_not_called_a_bad_dump(self):
        damaged = bytearray(self.data)
        damaged[4] ^= 0xFF          # frame header
        with self.assertRaises(ValueError) as caught:
            nxloader.load_nso_data(zbic_nso(bytes(damaged), len(self.expected)), 'game:main', 'main')
        self.assertIn('uses ZBIC compression', str(caught.exception))
        self.assertNotIn('dump the game again', str(caught.exception))

    def test_damaged_lz4_still_says_dump_again(self):
        with mock.patch.object(nxloader, '_lz4', None):
            with self.assertRaises(ValueError) as caught:
                nxloader.unpack_segment(b'\x00\x10\x27', 64, 0x3f, 'game:main', 0)
        self.assertIn('dump the game again', str(caught.exception))


if __name__ == '__main__':
    unittest.main()
