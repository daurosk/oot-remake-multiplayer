"""The ONE list of the one-time gift flags (row 237): read from the module's own table, never copied.

Source: lichii/hook/coop_shop_sale.h `inline constexpr std::array<GiftFlag, N> kOnceGifts{{ {hash, "name", "what"}, ... }}`
(the list the module shares: coop_hook.cpp `CoopShopSale::MergeGifts` + `share_pickup_named` for each). The test, its
fake and its selftest all read it here, so a flag added to or taken out of kOnceGifts changes every one of them.
"""
import os
import re

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
HEADER = os.path.join(REPO, "lichii", "hook", "coop_shop_sale.h")

_ARRAY = re.compile(r"std::array<GiftFlag,\s*(\d+)>\s*kOnceGifts\{\{(.*?)\}\};", re.S)
_ENTRY = re.compile(r'\{\s*0x([0-9a-fA-F]{8})\s*,\s*"([^"]+)"\s*,\s*"([^"]*)"\s*\}')


def read(path=HEADER):
    """[(hash int, name, what)] in the table's order; raises ValueError when the table is not found or its entry
    count is not the array's declared size (a reader that silently lost entries would test fewer flags)."""
    with open(path, encoding="utf-8") as f:
        text = f.read()
    m = _ARRAY.search(text)
    if not m:
        raise ValueError("kOnceGifts not found in %s" % path)
    size = int(m.group(1))
    out = [(int(h, 16), name, what) for h, name, what in _ENTRY.findall(m.group(2))]
    if len(out) != size:
        raise ValueError("kOnceGifts: %d entries read, the array says %d (%s)" % (len(out), size, path))
    return out


def names(path=HEADER):
    return [name for _, name, _ in read(path)]
