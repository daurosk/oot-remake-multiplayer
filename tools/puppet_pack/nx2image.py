"""Reads single files out of the RomFS image the NX2navi port keeps of the player's game.

The port's game folder holds `romfs/romfs.bin` (the raw RomFS image) and `romfs/index.json`,
whose "files" list gives each file's path, byte offset and size inside romfs.bin. So one file
is a slice of the image. Standard library only; nothing is written here.
"""
import fnmatch
import json
import os


class ImageError(RuntimeError):
    pass


def is_image(path):
    """True for a folder that holds romfs.bin and index.json."""
    return (os.path.isdir(path) and os.path.isfile(os.path.join(path, "romfs.bin"))
            and os.path.isfile(os.path.join(path, "index.json")))


def find(path):
    """The image folder for `path`: the folder itself or its `romfs` child (a game folder); None."""
    for p in (path, os.path.join(path, "romfs")):
        if is_image(p):
            return p
    return None


class Image:
    def __init__(self, folder):
        self.folder = folder
        try:
            with open(os.path.join(folder, "index.json"), encoding="utf-8") as f:
                index = json.load(f)
            self.files = {e["path"]: (int(e["offset"]), int(e["size"])) for e in index["files"]}
        except (OSError, ValueError, KeyError, TypeError) as e:
            raise ImageError(f"cannot read {os.path.join(folder, 'index.json')}: {e}") from e
        self.size = os.path.getsize(os.path.join(folder, "romfs.bin"))

    def glob(self, pattern):
        """RomFS paths matching the pattern (wildcards in the last part only), sorted."""
        d, _, name = pattern.rpartition("/")
        prefix = d + "/" if d else ""
        return sorted(p for p in self.files
                      if p.startswith(prefix) and "/" not in p[len(prefix):]
                      and fnmatch.fnmatchcase(p[len(prefix):], name))

    def read(self, rel_path):
        if rel_path not in self.files:
            raise ImageError(f"{rel_path} is not in the image's index")
        offset, size = self.files[rel_path]
        if offset < 0 or size < 0 or offset + size > self.size:
            raise ImageError(f"{rel_path}: romfs.bin is shorter than index.json says; is the extraction complete?")
        with open(os.path.join(self.folder, "romfs.bin"), "rb") as f:
            f.seek(offset)
            data = f.read(size)
        if len(data) != size:
            raise ImageError(f"{rel_path}: short read from romfs.bin")
        return data
