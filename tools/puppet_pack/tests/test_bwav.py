"""Tests for the sound archive reader and writer (bwav.py). Made-up clips only: no game files.

    python tools/puppet_pack/tests/test_bwav.py
"""
import os
import struct
import sys
import unittest
import zlib

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

from puppet_pack import bwav  # noqa: E402


def template(prefetch, n_here, n_full=None, channels=1):
    """An ADPCM BWAV header in the game's layout, with silent data."""
    start = 0x80 if channels == 1 else 0xC0
    d = bytearray(start)
    struct.pack_into("<4sHH", d, 0, b"BWAV", 0xFEFF, 1)
    struct.pack_into("<HH", d, 0x0C, prefetch, channels)
    for c in range(channels):
        b = 0x10 + bwav.CHANNEL * c
        struct.pack_into("<HHIII", d, b, 1, c, 48000, n_full or n_here, n_here)
        struct.pack_into("<II", d, b + 0x30, start + c * 0x100, start + c * 0x100)
    return bytes(d) + bytes((n_here + 13) // 14 * 8 * channels)


def amta(name):
    """An AMTA block whose name starts 0x10 after the offset field at 0x24."""
    raw = name.encode("latin1") + b"\0"
    size = (0x34 + len(raw) + 3) & ~3
    d = bytearray(size)
    d[0:4] = b"AMTA"
    struct.pack_into("<I", d, 8, size)
    struct.pack_into("<I", d, 0x24, 0x10)
    d[0x34:0x34 + len(raw)] = raw
    return bytes(d)


def sine(n, freq=440.0, amp=8000):
    t = np.arange(n) / 48000.0
    return (amp * np.sin(2 * np.pi * freq * t) * np.hanning(n)).astype(np.int16)


def snr_db(a, b):
    a = a.astype(np.float64)
    noise = a - b.astype(np.float64)
    return 10 * np.log10((a * a).sum() / max((noise * noise).sum(), 1e-9))


class Codec(unittest.TestCase):
    def test_encode_then_decode_is_close(self):
        x = sine(4800)
        (coefs, data), = bwav.encode_adpcm([x])
        clip, stream = bwav.clip_bytes(template(0, 14), len(x), coefs, data)
        self.assertIsNone(stream)
        y, rate = bwav.decode(clip)
        self.assertEqual(rate, 48000)
        self.assertEqual(len(y), len(x))
        self.assertGreater(snr_db(x, y), 30.0)

    def test_header_of_a_new_clip(self):
        x = sine(1000)
        (coefs, data), = bwav.encode_adpcm([x])
        clip, _ = bwav.clip_bytes(template(0, 14), len(x), coefs, data)
        h = bwav.header(clip)
        self.assertEqual(h["prefetch"], 0)
        self.assertEqual((h["channels"][0]["n_full"], h["channels"][0]["n"]), (1000, 1000))
        self.assertEqual(tuple(h["channels"][0]["coefs"]), tuple(coefs))
        self.assertEqual(struct.unpack_from("<I", clip, 8)[0], zlib.crc32(data))
        self.assertEqual(bwav.bwav_size(clip), len(clip))

    def test_prefetch_template_gives_a_head_and_a_stream(self):
        x = sine(700)
        (coefs, data), = bwav.encode_adpcm([x])
        head, stream = bwav.clip_bytes(template(1, 280, 5000), len(x), coefs, data)
        hh, hs = bwav.header(head), bwav.header(stream)
        self.assertEqual((hh["prefetch"], hh["channels"][0]["n_full"], hh["channels"][0]["n"]), (1, 700, 280))
        self.assertEqual((hs["prefetch"], hs["channels"][0]["n_full"], hs["channels"][0]["n"]), (0, 700, 700))
        self.assertEqual(head[8:12], stream[8:12])  # the game pairs head and stream by this crc
        whole, _ = bwav.decode(stream)
        part, _ = bwav.decode(head)
        self.assertTrue((whole[:280] == part).all())

    def test_a_short_clip_in_a_prefetch_template_is_all_head(self):
        x = sine(100)
        (coefs, data), = bwav.encode_adpcm([x])
        head, stream = bwav.clip_bytes(template(1, 280, 5000), len(x), coefs, data)
        self.assertEqual(bwav.header(head)["channels"][0]["n"], 100)
        self.assertEqual(bwav.header(stream)["channels"][0]["n"], 100)

    def test_a_two_channel_template_gives_one_channel(self):
        x = sine(500)
        (coefs, data), = bwav.encode_adpcm([x])
        clip, _ = bwav.clip_bytes(template(0, 14, channels=2), len(x), coefs, data)
        self.assertEqual(len(bwav.header(clip)["channels"]), 1)
        y, _ = bwav.decode(clip)
        self.assertGreater(snr_db(x, y), 30.0)

    def test_wrong_data_length_is_refused(self):
        with self.assertRaises(bwav.BwavError):
            bwav.clip_bytes(template(0, 14), 100, [0] * 16, b"\0" * 8)


class Archive(unittest.TestCase):
    def test_build_then_read(self):
        a = template(0, 28)
        b = template(0, 42)
        names = ["Vo_PlChild_Jump_S_01", "Vo_PlChild_Jump_S_02", "Se_Nothing"]
        assets = [(zlib.crc32(names[0].encode()), amta(names[0]), a),
                  (zlib.crc32(names[1].encode()), amta(names[1]), a),
                  (zlib.crc32(names[2].encode()), amta(names[2]), None)]
        d = bwav.bars_build(assets, extra=[7, 3])
        got = bwav.bars_names(d)
        self.assertEqual(sorted(got), sorted(names))
        self.assertIsNone(got["Se_Nothing"][1])
        self.assertEqual(got[names[0]][1], got[names[1]][1])  # one clip stored once
        self.assertEqual(got[names[0]][1] % 64, 0)
        self.assertEqual(d[got[names[0]][1]:got[names[0]][1] + len(a)], a)
        _, extra = bwav.read_bars(d)
        self.assertEqual(extra, [3, 7])
        d2 = bwav.bars_build(assets + [(zlib.crc32(b"X"), amta("X"), b)])
        self.assertEqual(len(bwav.bars_names(d2)), 4)

    def test_two_assets_with_one_hash_are_refused(self):
        with self.assertRaises(bwav.BwavError):
            bwav.bars_build([(1, amta("A"), None), (1, amta("B"), None)])

    def test_amta_name(self):
        m = amta("Vo_PlChild_Jump_S_01")
        self.assertEqual(bwav.amta_name(m), "Vo_PlChild_Jump_S_01")
        m2 = bwav.with_amta_name(m, "VSaPlChild_Jump_S_01")
        self.assertEqual(bwav.amta_name(m2), "VSaPlChild_Jump_S_01")
        self.assertEqual(len(m2), len(m))
        with self.assertRaises(bwav.BwavError):
            bwav.with_amta_name(m, "Short")


if __name__ == "__main__":
    unittest.main()
