"""The game's sound clips: BARS archives and BWAV clips (read and write).

Adapted from OoT Character Select `tools/bwav.py` (Copyright (c) 2026 snaildev99, MIT; the licence text is in
THIRD_PARTY_NOTICES.md): the archive and clip layouts, the DSP ADPCM decoder, the encoder (`fit_coefs`,
`encode_adpcm`) and `bars_build`. Changed here: it works on bytes (no paths, no zstd), and `clip_bytes` writes a clip
of any length into the shape of a template clip (his kept every clip's length).

A BARS archive (`Sound/Resource/<bank>.bars.zs`) holds a metadata block (AMTA, with the asset's name) and a BWAV per
asset, found by the crc32 of the name. A long clip is a "prefetch" BWAV: the archive has its head, the whole clip is
`Sound/Resource/Stream/<name in its AMTA>.bwav`.
"""
import struct
import zlib

try:
    import numpy as np
except ImportError:      # reading and writing archives needs no numpy; decoding and encoding do
    np = None

CHANNEL = 0x4C   # bytes per channel in a BWAV header
NO_BWAV = 0xFFFFFFFF


class BwavError(ValueError):
    pass


def amta_name(amta):
    """The name in an AMTA block (for a stream: the stream file's name)."""
    at = 0x24 + struct.unpack_from("<I", amta, 0x24)[0]
    end = amta.index(b"\0", at)
    return amta[at:end].decode("latin1")


def with_amta_name(amta, name):
    """An AMTA block with another name of the same length (the block keeps its layout)."""
    old = amta_name(amta)
    if len(name) != len(old):
        raise BwavError(f"{name}: not as long as {old}")
    at = 0x24 + struct.unpack_from("<I", amta, 0x24)[0]
    return amta[:at] + name.encode("latin1") + amta[at + len(old):]


def header(d, o=0):
    if d[o:o + 4] != b"BWAV":
        raise BwavError("not a BWAV")
    prefetch, nch = struct.unpack_from("<HH", d, o + 0x0C)
    chans = []
    for c in range(nch):
        b = o + 0x10 + CHANNEL * c
        enc, pan, rate, n_full, n_here = struct.unpack_from("<HHIII", d, b)
        coefs = struct.unpack_from("<16h", d, b + 0x10)
        start_full, start_here = struct.unpack_from("<II", d, b + 0x30)
        chans.append(dict(enc=enc, pan=pan, rate=rate, n_full=n_full, n=n_here, coefs=coefs, start=start_here))
    return dict(prefetch=prefetch, channels=chans)


def bwav_size(d, o=0):
    """Bytes of the BWAV at `o` (its header and the sample data that is there)."""
    return max(c["start"] + (c["n"] + 13) // 14 * 8 if c["enc"] == 1 else c["start"] + 2 * c["n"]
               for c in header(d, o)["channels"])


def _adpcm(d, start, n, coefs):
    """Nintendo DSP ADPCM: 8-byte frames, a header byte and 14 samples."""
    out = np.zeros(n, dtype=np.int16)
    h1 = h2 = 0
    pos, i = start, 0
    while i < n:
        head = d[pos]
        scale = 1 << (head & 15)
        c1, c2 = coefs[2 * (head >> 4)], coefs[2 * (head >> 4) + 1]
        for k in range(min(14, n - i)):
            byte = d[pos + 1 + k // 2]
            nib = (byte >> 4) if k % 2 == 0 else (byte & 15)
            if nib >= 8:
                nib -= 16
            s = ((nib * scale) << 11) + 1024 + c1 * h1 + c2 * h2 >> 11
            s = -32768 if s < -32768 else 32767 if s > 32767 else s
            out[i] = s
            h2, h1 = h1, s
            i += 1
        pos += 8
    return out


def decode(d, o=0):
    """The first channel's samples (int16) and the sample rate. Only the part that is at `o` (a stream's head in an
    archive)."""
    h = header(d, o)
    ch = h["channels"][0]
    if ch["enc"] == 1:
        return _adpcm(d, o + ch["start"], ch["n"], ch["coefs"]), ch["rate"]
    if ch["enc"] == 0:
        return np.frombuffer(d, "<i2", ch["n"], o + ch["start"]).copy(), ch["rate"]
    raise BwavError(f"encoding {ch['enc']}")


def read_bars(d):
    """{name hash: (AMTA block, offset of its BWAV or None)} of a BARS archive, and the hashes of its second table."""
    if d[:4] != b"BARS":
        raise BwavError("not a BARS")
    n = struct.unpack_from("<I", d, 12)[0]
    hashes = struct.unpack_from(f"<{n}I", d, 16)
    offs = struct.unpack_from(f"<{2 * n}I", d, 16 + 4 * n)
    m = struct.unpack_from("<I", d, 16 + 12 * n)[0]
    extra = list(struct.unpack_from(f"<{m}I", d, 20 + 12 * n))
    out = {}
    for i in range(n):
        a, b = offs[2 * i], offs[2 * i + 1]
        if d[a:a + 4] != b"AMTA":
            raise BwavError(f"asset {i}: no AMTA")
        size = struct.unpack_from("<I", d, a + 8)[0]
        out[hashes[i]] = (bytes(d[a:a + size]), None if b == NO_BWAV else b)
    return out, extra


def bars_names(d):
    """{asset name: (AMTA block, offset of its BWAV or None)}; names whose crc32 is not the table's hash are left out
    (a stream's AMTA names its file, which is the asset's own name in the game's archives)."""
    assets, _ = read_bars(d)
    out = {}
    for h, (amta, at) in assets.items():
        name = amta_name(amta)
        if zlib.crc32(name.encode("latin1")) == h:
            out[name] = (amta, at)
    return out


# The encoder (snaildev99's): eight predictor pairs per clip, fitted to the clip (frames grouped by the pair that
# predicts them best, each pair refitted to its group, a few rounds); then every 14-sample frame tries each pair at
# two step sizes with the decoder's own arithmetic and keeps the one with the least error. Many clips are encoded
# side by side (one numpy row each).
_START = [(0.0, 0.0), (0.9, 0.0), (1.8, -0.9), (1.5, -0.6), (1.2, -0.4), (1.95, -0.97), (0.5, 0.2), (1.65, -0.75)]


def fit_coefs(x, rounds=12):
    """16 coefficients (8 pairs, 1/2048 units) for one clip of int samples."""
    n = len(x) // 14 * 14
    if n < 28:
        return [int(round(v * 2048)) for pair in _START for v in pair]
    p = np.concatenate([[0.0, 0.0], np.asarray(x[:n], dtype=np.float64)])
    s0, s1, s2 = p[2:].reshape(-1, 14), p[1:-1].reshape(-1, 14), p[:-2].reshape(-1, 14)
    r1, r2 = (s0 * s1).sum(1), (s0 * s2).sum(1)
    r0, r11, r12, r22 = (s0 * s0).sum(1), (s1 * s1).sum(1), (s1 * s2).sum(1), (s2 * s2).sum(1)
    pairs = np.array(_START)
    for _ in range(rounds):
        a1, a2 = pairs[:, 0][:, None], pairs[:, 1][:, None]
        err = r0 - 2 * a1 * r1 - 2 * a2 * r2 + a1 * a1 * r11 + 2 * a1 * a2 * r12 + a2 * a2 * r22
        best = err.argmin(0)
        for k in range(8):
            m = best == k
            if not m.any():
                continue
            a = np.array([[r11[m].sum() + 1.0, r12[m].sum()], [r12[m].sum(), r22[m].sum() + 1.0]])
            pairs[k] = np.linalg.solve(a, np.array([r1[m].sum(), r2[m].sum()]))
    pairs = np.clip(pairs, -15.9, 15.9)
    return [int(round(v * 2048)) for pair in pairs for v in pair]


def encode_adpcm(clips, batch=64):
    """[(16 coefficients, data bytes)] for a list of int16 clips (DSP ADPCM). Clips of like length are encoded
    side by side, `batch` at a time (each batch runs to its longest clip)."""
    order = sorted(range(len(clips)), key=lambda i: len(clips[i]))
    out = [None] * len(clips)
    for b in range(0, len(order), batch):
        part = order[b:b + batch]
        for i, r in zip(part, _encode([clips[i] for i in part])):
            out[i] = r
    return out


def _encode(clips):
    if not clips:
        return []
    count = len(clips)
    frames = [max(1, (len(c) + 13) // 14) for c in clips]
    most = max(frames)
    x = np.zeros((count, most * 14), dtype=np.int64)
    for i, c in enumerate(clips):
        x[i, :len(c)] = c
    coefs = [fit_coefs(np.asarray(c)) for c in clips]
    ca = np.array(coefs, dtype=np.int64).reshape(count, 8, 2)
    c1 = np.repeat(ca[:, :, 0], 2, axis=1)
    c2 = np.repeat(ca[:, :, 1], 2, axis=1)
    h1 = np.zeros(count, dtype=np.int64)
    h2 = np.zeros(count, dtype=np.int64)
    out = np.zeros((count, most, 8), dtype=np.uint8)
    rows = np.arange(count)
    for f in range(most):
        fr = x[:, f * 14:f * 14 + 14]
        m1 = np.concatenate([h1[:, None], fr[:, :-1]], axis=1)
        m2 = np.concatenate([h2[:, None], h1[:, None], fr[:, :-2]], axis=1)
        res = fr[:, None, :] - ((ca[:, :, 0:1] * m1[:, None, :] + ca[:, :, 1:2] * m2[:, None, :] + 1024) >> 11)
        peak = np.abs(res).max(axis=2)
        exp = np.clip(np.ceil(np.log2(np.maximum(peak, 1) / 7.5)), 0, 12).astype(np.int64)
        exps = np.clip(np.stack([exp, exp + 1], axis=2).reshape(count, 16), 0, 12)
        scale = 1 << exps
        a1 = np.repeat(h1[:, None], 16, axis=1)
        a2 = np.repeat(h2[:, None], 16, axis=1)
        err = np.zeros((count, 16), dtype=np.int64)
        nibs = np.zeros((count, 16, 14), dtype=np.int64)
        for k in range(14):
            base = 1024 + c1 * a1 + c2 * a2
            want = fr[:, k:k + 1]
            nib = np.clip(np.rint((want - (base >> 11)) / scale), -8, 7).astype(np.int64)
            got = np.clip((((nib * scale) << 11) + base) >> 11, -32768, 32767)
            err += (got - want) ** 2
            nibs[:, :, k] = nib
            a2, a1 = a1, got
        pick = err.argmin(axis=1)
        h1, h2 = a1[rows, pick], a2[rows, pick]
        chosen = nibs[rows, pick] & 15
        out[:, f, 0] = ((pick // 2) << 4) | exps[rows, pick]
        out[:, f, 1:] = (chosen[:, 0::2] << 4) | chosen[:, 1::2]
    return [(coefs[i], out[i, :frames[i]].tobytes()) for i in range(count)]


def clip_bytes(template, n, coefs, data, at=0):
    """A one-channel ADPCM clip of `n` samples (`coefs`, `data` from encode_adpcm) in the shape of the template BWAV
    at `at` (its first channel's header): -> (the BWAV for the archive, the stream file or None). A prefetch template gives a head of at most its
    own head's samples in the archive and the whole clip as the stream file; else the whole clip."""
    h = header(template, at)
    if h["channels"][0]["enc"] != 1:
        raise BwavError("the template is not ADPCM")
    ch = h["channels"][0]
    start = ch["start"]
    if len(data) != (n + 13) // 14 * 8:
        raise BwavError("data and sample count differ")

    def made(prefetch, here):
        head = bytearray(template[at:at + start])
        struct.pack_into("<I", head, 0x08, zlib.crc32(data))
        struct.pack_into("<HH", head, 0x0C, prefetch, 1)   # one channel, also from a two-channel template
        struct.pack_into("<H", head, 0x10 + 0x02, 2)        # centre, as the game's one-channel clips
        struct.pack_into("<II", head, 0x10 + 0x08, n, here)
        struct.pack_into("<16h", head, 0x10 + 0x10, *coefs)
        struct.pack_into("<Hhh", head, 0x10 + 0x44, data[0], 0, 0)
        return bytes(head) + data[:(here + 13) // 14 * 8]

    if not h["prefetch"]:
        return made(0, n), None
    return made(1, min(n, ch["n"])), made(0, n)


def bars_build(assets, extra=()):
    """A BARS archive (version 0x102) from [(name hash, AMTA block, BWAV or None)]: header, the hashes (sorted), per
    asset the offsets of its AMTA and its BWAV, a second table (count + hashes), the AMTA blocks in table order, then
    each BWAV at a multiple of 64. Equal BWAVs are stored once, as in the game's files."""
    table = sorted(range(len(assets)), key=lambda i: assets[i][0])
    n = len(assets)
    if len({a[0] for a in assets}) != n:
        raise BwavError("two assets with one name hash")
    pos = 16 + 12 * n + 4 + 4 * len(extra)
    amta_at = {}
    for i in table:
        amta = assets[i][1]
        if len(amta) % 4 or struct.unpack_from("<I", amta, 8)[0] != len(amta):
            raise BwavError("an AMTA block of a wrong size")
        amta_at[i] = pos
        pos += len(amta)
    bwav_at, placed, blobs = {}, {}, []
    for i in range(n):
        blob = assets[i][2]
        if blob is None:
            bwav_at[i] = NO_BWAV
            continue
        if blob not in placed:
            pos = (pos + 63) & ~63
            placed[blob] = pos
            blobs.append((pos, blob))
            pos += len(blob)
        bwav_at[i] = placed[blob]
    out = bytearray(pos)
    struct.pack_into("<4sIHHI", out, 0, b"BARS", pos, 0xFEFF, 0x102, n)
    struct.pack_into(f"<{n}I", out, 16, *[assets[i][0] for i in table])
    for k, i in enumerate(table):
        struct.pack_into("<II", out, 16 + 4 * n + 8 * k, amta_at[i], bwav_at[i])
        out[amta_at[i]:amta_at[i] + len(assets[i][1])] = assets[i][1]
    struct.pack_into(f"<I{len(extra)}I", out, 16 + 12 * n, len(extra), *sorted(extra))
    for at, blob in blobs:
        out[at:at + len(blob)] = blob
    return bytes(out)
