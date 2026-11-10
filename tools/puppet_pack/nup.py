"""Reads the game's RomFS files straight from a .nup game file.

A .nup (Fritchii/Eden "NX Package", src/core/file_sys/nup_container.cpp) holds the
base game, its updates and add-ons as loose ExeFS/RomFS trees, each file stored raw or
as one zstd frame. Nothing in it is encrypted, so no keys are needed: this is how the
pack tool works for players whose keys cannot decrypt the update for Dump RomFS.

Layout (all little-endian):
  superblock (0x80): "NUP\\x01", header size, version, toc offset/size, toc sha256,
                     blob offset, file size, package count, flags, uuid
  toc: header (0x30) "TOC\\x01", packages (0x40 each), dirs (0x18 each),
       files (0x50 each), children (u32 each), string pool
A child index below the dir count is a dir, else a file (index - dir count). The game's
RomFS is the base package's tree with the newest update's tree laid over it, the
update winning, as the emulator's AppLoader_NUP::ReadRomFS does.
"""
import fnmatch
import hashlib
import io
import os
import struct

MAGIC = b"NUP\x01"
TOC_MAGIC = 0x01434F54
NO_DIR = 0xFFFFFFFF
KIND_BASE, KIND_UPDATE, KIND_ADDON = 0, 1, 2
CODEC_RAW, CODEC_ZSTD = 0, 1

SUPER = struct.Struct("<4sHHQQ32sQQII16s32s")
TOC = struct.Struct("<IIQQQQQ")
PACKAGE = struct.Struct("<QIIIIBBHIQI20s")
DIR = struct.Struct("<IIIIQ")
FILE = struct.Struct("<IIQQQ32sBBH12s")
assert (SUPER.size, TOC.size, PACKAGE.size, DIR.size, FILE.size) == (0x80, 0x30, 0x40, 0x18, 0x50)


class NupError(RuntimeError):
    pass


def safe_name(name):
    """A single path part: not empty, not . or .., no separators, drive colons or NUL."""
    return bool(name) and name not in (".", "..") and not any(ch in name for ch in "/\\:\0")


def write_under(root, rel, data):
    """Writes data to root/rel, refusing any path that would land outside root.
    The data is already read, so nothing is opened before the check."""
    parts = rel.split("/")
    if not all(safe_name(p) for p in parts):
        raise NupError(f"refused to write {rel!r}: not a plain relative path")
    real_root = os.path.realpath(root)
    dst = os.path.realpath(os.path.join(real_root, *parts))
    if os.path.commonpath([real_root, dst]) != real_root or dst == real_root:
        raise NupError(f"refused to write {rel!r}: it is outside {root}")
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    with open(dst, "wb") as f:
        f.write(data)
    return dst


def is_nup(path):
    try:
        with open(path, "rb") as f:
            return f.read(4) == MAGIC
    except OSError:
        return False


class Nup:
    def __init__(self, path):
        self.path = path
        self.f = open(path, "rb")
        try:
            self._parse()
        except (struct.error, IndexError, UnicodeDecodeError) as e:
            self.f.close()
            raise NupError(f"{path} is not a readable .nup ({e})") from e
        except NupError:
            self.f.close()
            raise

    def close(self):
        self.f.close()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    def _read(self, offset, size):
        self.f.seek(offset)
        data = self.f.read(size)
        if len(data) != size:
            raise NupError(f"{self.path} is cut short (a download or copy that did not finish?)")
        return data

    def _parse(self):
        size = os.path.getsize(self.path)
        (magic, _hsize, _hver, toc_off, toc_size, toc_sha, _blob, file_size, pkg_count,
         _flags, _uuid, _r) = SUPER.unpack(self._read(0, SUPER.size))
        if magic != MAGIC:
            raise NupError(f"{self.path} is not a .nup file")
        if file_size != size:
            raise NupError(f"{self.path} is {size} bytes but says {file_size}: "
                           "it is cut short or damaged")
        toc = self._read(toc_off, toc_size)
        if hashlib.sha256(toc).digest() != toc_sha:
            raise NupError(f"{self.path} is damaged (its table of contents does not match its hash)")
        tmagic, tpkgs, ndirs, nfiles, pool_off, pool_size, _ = TOC.unpack_from(toc, 0)
        if tmagic != TOC_MAGIC or tpkgs != pkg_count:
            raise NupError(f"{self.path} has a bad table of contents")
        pool = toc[pool_off:pool_off + pool_size]
        pos = TOC.size
        self.packages = []
        for _ in range(tpkgs):
            tid, root, exefs, romfs, ver, kind, flags, _a, _b, _mo, _ms, _c = PACKAGE.unpack_from(toc, pos)
            pos += PACKAGE.size
            self.packages.append({"title_id": tid, "romfs_dir": romfs, "version": ver,
                                  "kind": kind, "standalone": bool(flags & 1)})
        self.dirs = []
        for _ in range(ndirs):
            noff, nlen, first, count, _ = DIR.unpack_from(toc, pos)
            pos += DIR.size
            self.dirs.append((pool[noff:noff + nlen].decode("utf-8"), first, count))
        self.files = []
        for _ in range(nfiles):
            noff, nlen, fsize, eoff, esize, sha, codec, _cf, _a, _b = FILE.unpack_from(toc, pos)
            pos += FILE.size
            self.files.append((pool[noff:noff + nlen].decode("utf-8"), fsize, eoff, esize, sha, codec))
        n_children = (pool_off - pos) // 4
        self.children = struct.unpack_from(f"<{n_children}I", toc, pos)
        # Every index and every name a tree walk can reach must be sound: a bad index
        # is a NupError, never an IndexError, and no child name can leave its folder
        # when it is joined into a path.
        for p in self.packages:
            if p["romfs_dir"] != NO_DIR and p["romfs_dir"] >= ndirs:
                raise NupError(f"{self.path} has a bad table of contents (package dir out of range)")
        for _name, first, count in self.dirs:
            if first + count > n_children:
                raise NupError(f"{self.path} has a bad table of contents (children out of range)")
        for c in self.children:
            if c >= ndirs + nfiles:
                raise NupError(f"{self.path} has a bad table of contents (child out of range)")
            name = self.dirs[c][0] if c < ndirs else self.files[c - ndirs][0]
            if not safe_name(name):
                raise NupError(f"{self.path} has a file or folder named {name!r}, which is not allowed")

    # -- packages --------------------------------------------------------------

    def base(self):
        return next((p for p in self.packages if p["kind"] == KIND_BASE), None)

    def best_update(self):
        ups = [p for p in self.packages if p["kind"] == KIND_UPDATE]
        return max(ups, key=lambda p: p["version"]) if ups else None

    # -- trees -----------------------------------------------------------------

    def _entries(self, dir_index):
        """{name: ("d", dir index) | ("f", file index)} of one dir node."""
        _name, first, count = self.dirs[dir_index]
        out = {}
        for c in self.children[first:first + count]:
            if c < len(self.dirs):
                out[self.dirs[c][0]] = ("d", c)
            else:
                out[self.files[c - len(self.dirs)][0]] = ("f", c - len(self.dirs))
        return out

    def romfs_roots(self):
        """The RomFS dir nodes to merge, lowest priority first (base, then update)."""
        roots = []
        for p in (self.base(), self.best_update()):
            if p and p["romfs_dir"] != NO_DIR and p["romfs_dir"] < len(self.dirs):
                roots.append(p["romfs_dir"])
        if not roots:
            raise NupError(f"{self.path} has no RomFS")
        return roots

    def listdir(self, rel_dir):
        """Merged {name: (kind, index)} of a RomFS folder ("" for the root)."""
        merged = {}
        for root in self.romfs_roots():
            node = root
            for part in [p for p in rel_dir.split("/") if p]:
                hit = self._entries(node).get(part)
                if not hit or hit[0] != "d":
                    node = None
                    break
                node = hit[1]
            if node is not None:
                merged.update(self._entries(node))
        return merged

    def glob(self, pattern):
        """RomFS paths matching `dir/.../name-pattern` (wildcards in the last part only)."""
        d, _, name = pattern.rpartition("/")
        return sorted(f"{d}/{n}" if d else n for n, (k, _i) in self.listdir(d).items()
                      if k == "f" and fnmatch.fnmatchcase(n, name))

    def read(self, rel_path):
        d, _, name = rel_path.rpartition("/")
        hit = self.listdir(d).get(name)
        if not hit or hit[0] != "f":
            raise NupError(f"{self.path} has no {rel_path}")
        fname, fsize, eoff, esize, sha, codec = self.files[hit[1]]
        if fsize == 0:
            data = b""
        elif codec == CODEC_RAW:
            data = self._read(eoff, esize)[:fsize]
        elif codec == CODEC_ZSTD:
            import zstandard
            try:
                # Never more than the stored size plus one byte, whatever the frame
                # header claims (max_output_size alone is skipped when the header has
                # a content size), so a bad frame cannot blow up memory.
                with zstandard.ZstdDecompressor().stream_reader(io.BytesIO(self._read(eoff, esize))) as r:
                    data = r.read(fsize + 1)
            except zstandard.ZstdError as e:
                raise NupError(f"{rel_path} in {self.path} is damaged ({e})") from e
        else:
            raise NupError(f"{rel_path} in {self.path} uses an unknown codec {codec}")
        if len(data) != fsize or hashlib.sha256(data).digest() != sha:
            raise NupError(f"{rel_path} in {self.path} is damaged (its hash does not match)")
        return data

    def extract(self, patterns, out_dir):
        """Writes every file matching `patterns` under out_dir; returns the RomFS paths."""
        written = []
        for pattern in patterns:
            for rel in self.glob(pattern):
                write_under(out_dir, rel, self.read(rel))
                written.append(rel)
        return written


# -- writer, for tests ---------------------------------------------------------

def write_nup(path, packages):
    """Writes a .nup. packages: [(title_id, version, kind, {romfs path: (bytes, codec)})]."""
    import zstandard
    dirs, files, children, pool = [], [], [], bytearray()
    blobs = []

    def name(s):
        b = s.encode("utf-8")
        off = len(pool)
        pool.extend(b)
        return off, len(b)

    def tree(paths):
        t = {}
        for p, v in paths.items():
            node = t
            parts = p.split("/")
            for part in parts[:-1]:
                node = node.setdefault(part, {})
            node[parts[-1]] = v
        return t

    # Dir indices must be known before file indices are offset, so lay out dirs first.
    pending = []

    def add_dir(dname, node):
        idx = len(dirs)
        dirs.append([dname, 0, 0])
        pending.append((idx, node))
        return idx

    pkg_roots = []
    for _tid, _ver, _kind, content in packages:
        pkg_roots.append(add_dir("romfs", tree(content)))
    i = 0
    while i < len(pending):
        idx, node = pending[i]
        for k, v in sorted(node.items()):
            if isinstance(v, dict):
                add_dir(k, v)
        i += 1
    dir_index = {}
    for idx, node in pending:
        dir_index[id(node)] = idx
    for idx, node in pending:
        dirs[idx][1] = len(children)
        kids = []
        for k, v in sorted(node.items()):
            if isinstance(v, dict):
                kids.append(dir_index[id(v)])
            else:
                data, codec = v
                stored = zstandard.ZstdCompressor().compress(data) if codec == CODEC_ZSTD else data
                files.append([k, len(data), stored, hashlib.sha256(data).digest(), codec])
                kids.append(("file", len(files) - 1))
        dirs[idx][2] = len(kids)
        children.extend(kids)
    children = [c if not isinstance(c, tuple) else len(dirs) + c[1] for c in children]

    toc_size_wo_pool = TOC.size + PACKAGE.size * len(packages) + DIR.size * len(dirs) + FILE.size * len(files) + 4 * len(children)
    dir_recs = [(name(d[0]), d[1], d[2]) for d in dirs]
    file_names = [name(f[0]) for f in files]
    toc_size = toc_size_wo_pool + len(pool)
    blob_off = (SUPER.size + toc_size + 0xF) & ~0xF
    off = blob_off
    extents = []
    for f in files:
        if f[1] == 0:
            extents.append((0, 0))
            continue
        extents.append((off, len(f[2])))
        blobs.append((off, f[2]))
        off = (off + len(f[2]) + 0xF) & ~0xF
    file_size = off
    toc = bytearray(TOC.pack(TOC_MAGIC, len(packages), len(dirs), len(files), toc_size_wo_pool, len(pool), 0))
    for (tid, ver, kind, _c), root in zip(packages, pkg_roots):
        toc += PACKAGE.pack(tid, root, NO_DIR, root, ver, kind, 0, 0, 0, 0, 0, b"\0" * 20)
    for (noff, nlen), first, count in dir_recs:
        toc += DIR.pack(noff, nlen, first, count, 0)
    for f, (noff, nlen), (eoff, esize) in zip(files, file_names, extents):
        toc += FILE.pack(noff, nlen, f[1], eoff, esize, f[3], f[4], 0, 0, b"\0" * 12)
    toc += struct.pack(f"<{len(children)}I", *children)
    toc += pool
    out = bytearray(file_size)
    out[0:SUPER.size] = SUPER.pack(MAGIC, SUPER.size, 1, SUPER.size, len(toc), hashlib.sha256(toc).digest(),
                                   blob_off, file_size, len(packages), 0, b"\0" * 16, b"\0" * 32)
    out[SUPER.size:SUPER.size + len(toc)] = toc
    for o, b in blobs:
        out[o:o + len(b)] = b
    with open(path, "wb") as f:
        f.write(out)
