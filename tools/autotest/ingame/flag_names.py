"""Every game flag name the in-game tests and their boot scenarios read or write, checked against the game's own flag
list (B #168, after probe0121e-16ac9701 2026-11-28 05:07 UTC: test_epona's GET_EPONA was 'Get_Epona', no flag at all,
so its 'won' set-ups wrote nothing and read None for weeks).

ONE source of truth: flag_table.json beside this file, made by this tool from the game's
romfs/GameData/GameDataList.Product.110.byml.zs (Murmur3 x86 32, seed 0, of each name; Bool64bitKey for key64:) plus
MOD below (flags a mod pack adds, each with its source). Never edit the json by hand: run

    python tools/autotest/ingame/flag_names.py --romfs <romfs>      (writes flag_table.json, exit 1 on a name the
                                                                      game does not have)
    python tools/autotest/ingame/flag_names.py                      (checks every use against flag_table.json)

tools/autotest/tests/test_flag_names.py runs the check in CI (no game, no fake, no romfs).

What a use is (the module's own name handling, lichii/hook/coop_testinput.cpp and hookshot/src/coop_playtest.cpp):
- read: Pair.flag_read(x, NAME), the bridge's flag_read -> ReadFlag: NAME or 0xHASH (NameHash), key64:0xKEY, NAME[i]
  (a String64Array element); kinds Int, Bool, Enum, UInt, String64 (ReadFlag's order). Anything else reads 'unknown'.
- set: Pair.flag(x, NAME, number), an act/scenario 'setflag NAME V' -> SetFlag: flag_hash(NAME), Int or Bool only, or
  key64:0xKEY. A 0x name is hashed as its TEXT there (SetFlag and FlagInt use flag_hash, not NameHash), so it is no
  name for a set: the write is a logged no-op.
- enum: Pair.flag(x, NAME, 'Value'|'0xHASH'), an act/scenario 'setenum NAME Value' -> SetEnum/SetEnumHash: an Enum
  (NAME or 0xHASH); a value's name must be one of the Enum's RawValues, a 0x value one of its Values.
- text: Pair.flag(x, 'NAME[i]', 'text') -> SetStringArrayElement: a String64Array.
A name the scan cannot work out (a function's parameter, a built string) must be in DYNAMIC with the reason.
"""
import argparse
import ast
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SCENARIOS = os.path.join(os.path.dirname(HERE), "scenarios")
TABLE = os.path.join(HERE, "flag_table.json")
GAME_LIST = "GameData/GameDataList.Product.110.byml.zs"

# Flags a mod pack adds to GameDataList (not in the game's own list): name -> (type, hash, source).
MOD = {
    "CharacterMenu_SelectedCharacter": (
        "Int", 0xd6b580ea, "Snail's Character Select (the coop-puppet pack's GameDataList: no_cs.py:27-28, "
        "lane1-B-nocs; lichii/hook/coop_char_ages.h:4)"),
}

def _save_profile_names():
    """testsave.prove's reads: every ingame/saves profile's FLAGS and CLEAR_TEXT hashes and EXPECT items (IsGet.)."""
    import testsave
    out = []
    for fn in sorted(os.listdir(os.path.join(HERE, "saves"))):
        if fn.endswith(".py"):
            name = fn[:-3]
            out += [("read", "0x%08x" % h) for _kind, h, _want in testsave.load(name)]
            out += [("read", "0x%08x" % h) for h in testsave.clear_text_of(name)]
            for e in testsave.expect_of(name).values():
                out += [("read", "IsGet." + item) for item in e.get("items") or []]
    return out


def _key_item_names():
    """test_key_items' reads and sets: the counted flags tally_flags() reads from game_items.cpp."""
    import test_key_items as K
    return [(op, n) for names in K.tally_flags().values() for n in names for op in ("read", "set")]


# Uses whose name the scan cannot work out: (file, function) -> {"why", "names" (optional: a function giving the
# (op, name) uses there, read from the repo, checked like any other)}. Kept short; every new one is a review point.
DYNAMIC = {
    ("pair.py", "flag"): {"why": "the API itself: its callers' names are what this scan checks"},
    ("pair.py", "flag_read"): {"why": "the API itself"},
    ("testsave.py", "prove"): {"why": "a save profile's own hashes and items", "names": _save_profile_names},
    ("test_key_items.py", "owned"): {"why": "tally_flags(): game_items.cpp's counted flags",
                                     "names": _key_item_names},
    ("test_key_items.py", "holds_all"): {"why": "as owned()", "names": None},
    ("test_key_items.py", "test_key_items_pages"): {"why": "as owned()", "names": None},
}

# What a red check asks for (review-tests-153-6 SHOULD): the table is made only from the romfs, which CI has not.
REMAKE = ("A new name: where the game dump is, run `python tools/autotest/ingame/flag_names.py --romfs <romfs>` "
          "(e.g. OOT REMAKE/dump/romfs) and commit flag_table.json; it prints NOT IN THE GAME for a wrong name")
READ_TYPES = ("Int", "Bool", "Enum", "UInt", "String64")
SET_TYPES = ("Int", "Bool")
NUM = re.compile(r"-?\d+$")
HEX = re.compile(r"0[xX][0-9a-fA-F]{1,8}$")
LINE = re.compile(r"^\s*(setflag|setenum)\s+(\S+)\s+(\S+)")


def murmur3(text, seed=0):
    """The game's flag hash (coop::protocol::flag_hash, coop/protocol/src/codec.cpp): Murmur3 x86 32-bit."""
    data = text.encode("utf-8")
    c1, c2, m = 0xcc9e2d51, 0x1b873593, 0xffffffff
    h, n = seed, len(data)
    for i in range(0, n - n % 4, 4):
        k = int.from_bytes(data[i:i + 4], "little")
        k = (k * c1) & m
        k = ((k << 15) | (k >> 17)) & m
        h ^= (k * c2) & m
        h = ((h << 13) | (h >> 19)) & m
        h = (h * 5 + 0xe6546b64) & m
    k = int.from_bytes(data[n - n % 4:], "little") if n % 4 else 0
    if n % 4:
        k = (k * c1) & m
        k = ((k << 15) | (k >> 17)) & m
        h ^= (k * c2) & m
    h ^= n
    h ^= h >> 16
    h = (h * 0x85ebca6b) & m
    h ^= h >> 13
    h = (h * 0xc2b2ae35) & m
    return h ^ (h >> 16)


def key_of(name):
    """The table key of a used name: 'key64:0x...' as is (lower case), NAME[i] as NAME, 0xHASH lower case."""
    if name.lower().startswith("key64:"):
        return name.lower()
    if name.endswith("]") and "[" in name:
        name = name[:name.rindex("[")]
    return name.lower() if HEX.match(name) else name


# -- the scan ---------------------------------------------------------------------------------------------------------

class Use:
    def __init__(self, name, op, value, where):
        self.name, self.op, self.value, self.where = name, op, value, where

    def __repr__(self):
        return "%s %s %r (%s)" % (self.op, self.name, self.value, self.where)


def _flat(v):
    """A Python value as its str/int leaves (a dict's values), or None when it holds anything else."""
    if isinstance(v, bool):
        return None
    if isinstance(v, (str, int)):
        return [v]
    if isinstance(v, dict):
        v = list(v.values())
    if isinstance(v, (list, tuple, set, frozenset)):
        out = []
        for e in v:
            f = _flat(e)
            if f is None:
                return None
            out += f
        return out
    return None


class Module:
    """One scanned file. A module-level name or module.attr the AST cannot work out (GIFTS = once_gifts.names(),
    BOTTLES = hookshot.BOTTLE_ITEMS) is read from the imported module; only dotted names are evaluated, never a call."""
    _loaded = {}

    def __init__(self, path):
        self.path, self.name = path, os.path.splitext(os.path.basename(path))[0]
        self.tree = ast.parse(open(path, encoding="utf-8").read())

    def live(self, node):
        src = ast.unparse(node)
        if not re.match(r"[A-Za-z_][\w.]*$", src):
            return None
        mod = Module._loaded.get(self.path)
        if mod is None:
            import importlib.util
            for d in (os.path.dirname(self.path), os.path.dirname(os.path.dirname(self.path))):
                if d not in sys.path:
                    sys.path.insert(0, d)
            spec = importlib.util.spec_from_file_location("flagscan_" + self.name, self.path)
            mod = importlib.util.module_from_spec(spec)
            try:
                spec.loader.exec_module(mod)
            except Exception:      # (a module that does not import here: its names stay unresolved, reported)
                mod = False
            Module._loaded[self.path] = mod
        if not mod:
            return None
        try:
            return _flat(eval(src, vars(mod)))     # a dotted name only (checked above)
        except Exception:
            return None


def _values(node, env, mod=None):
    """The str/int values a node can have, or None when not worked out. env: {name: [values]} of the scope."""
    if node is None:
        return None
    if isinstance(node, ast.Constant):
        return [node.value] if isinstance(node.value, (str, int)) and not isinstance(node.value, bool) else None
    if isinstance(node, ast.Name):
        if node.id in env:
            return env[node.id]
        return None if mod is None or node.id in env.get("__params__", ()) else mod.live(node)
    if isinstance(node, ast.Attribute):
        return mod.live(node) if mod is not None else None
    if isinstance(node, (ast.Tuple, ast.List, ast.Set)):
        vals = []
        for e in node.elts:
            v = _values(e, env, mod)
            if v is None:
                return None
            vals += v
        return vals
    if isinstance(node, ast.Dict):
        return _values(ast.Tuple(elts=list(node.values)), env, mod)
    if isinstance(node, ast.Subscript):
        return _values(node.value, env, mod)
    if isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute) and node.func.attr in ("values", "keys") \
            and not node.args:
        return _values(node.func.value, env, mod)
    if isinstance(node, ast.BinOp) and isinstance(node.op, ast.Mod) and isinstance(node.left, ast.Constant) \
            and isinstance(node.left.value, str):
        parts = node.right.elts if isinstance(node.right, ast.Tuple) else [node.right]
        tpl = node.left.value.replace("%%", "\0")
        specs = re.findall(r"%[-+ #0-9.]*[sdixX]", tpl)
        if len(specs) != len(parts):
            return None
        first = _values(parts[0], env, mod)              # the flag's name: worked out, or not at all
        if first is None:
            return None
        rest = [_values(p, env, mod) for p in parts[1:]]
        tpl = re.sub(r"%[-+ #0-9.]*[sdixX]", "%s", tpl)
        rest = tuple(str(r[0]) if r and len(r) == 1 else "*" for r in rest)
        return [(tpl % ((f,) + rest)).replace("\0", "%") for f in first]
    return None


def _scope_env(func, mod):
    """A function's own simple assignments and loop/comprehension names over values worked out; its parameters
    are never read from the module (a parameter named as a module constant is still the caller's value)."""
    if func is None:
        return {}
    params = {a.arg for a in func.args.args + func.args.kwonlyargs}
    local = {"__params__": params, "__loops__": {}}
    for n in ast.walk(func):
        if isinstance(n, (ast.For, ast.comprehension)) and isinstance(n.target, ast.Name):
            it = n.iter.value if isinstance(n.iter, ast.Subscript) else n.iter
            if isinstance(it, ast.Name) and it.id in params:
                local["__loops__"][n.target.id] = it.id
    for _ in range(2):                         # (a loop over a name assigned further down in the source)
        for n in ast.walk(func):
            targets = []
            if isinstance(n, (ast.For, ast.comprehension)) and isinstance(n.target, ast.Name):
                targets = [(n.target.id, n.iter)]
            elif isinstance(n, ast.Assign) and len(n.targets) == 1 and isinstance(n.targets[0], ast.Name):
                targets = [(n.targets[0].id, n.value)]
            for name, val in targets:
                if name in params:
                    continue
                v = _values(val, local, mod)
                if v is not None:
                    local[name] = sorted(set(local.get(name, [])) | set(v), key=str)
    return local


def _param_index(func, node, env):
    """The positional index among func's parameters of `node`: a parameter, or a loop name over one (`for n in
    names`, `for n in flags[g]`); else None."""
    if func is None or not isinstance(node, ast.Name):
        return None
    names = [a.arg for a in func.args.args]
    name = env.get("__loops__", {}).get(node.id, node.id)
    return names.index(name) if name in names else None


def _uses_of_call(call, env, mod, func, where, out, dyn, sinks):
    """The flag uses of one call. A name that is the enclosing function's parameter goes to `sinks` (worked out at
    that function's callers); one not worked out at all goes to `dyn`."""
    attr = call.func.attr if isinstance(call.func, ast.Attribute) else None
    if attr == "act" and len(call.args) >= 2:
        lines = _values(call.args[1], env, mod)
        if lines is None:
            if re.search(r"set(flag|enum)", ast.unparse(call.args[1])):
                dyn.append(where)
            return
        for text in lines:
            for ln in str(text).splitlines():
                _line_use(ln, where, out)
        return
    if attr not in ("flag", "flag_read") or len(call.args) < 2:
        return
    vnode = call.args[2] if len(call.args) >= 3 else next((k.value for k in call.keywords if k.arg == "value"), None)
    names = _values(call.args[1], env, mod)
    if names is None:
        i = _param_index(func, call.args[1], env)
        if i is not None:
            sinks.append((func.name, i, attr, vnode, env, where))
        else:
            dyn.append(where)
        return
    _add(names, attr, vnode, env, mod, where, out)


def _add(names, attr, vnode, env, mod, where, out):
    if attr == "flag_read":
        for nm in names:
            out.append(Use(str(nm), "read", None, where))
        return
    values = [1] if vnode is None else _values(vnode, env, mod)
    for nm in names:
        nm = str(nm)
        for v in values if values is not None else [None]:
            if nm.endswith("]"):
                out.append(Use(nm, "text", v, where))
            elif isinstance(v, int) or (isinstance(v, str) and NUM.match(v)):
                out.append(Use(nm, "set", v, where))
            elif v is None:
                out.append(Use(nm, "write", None, where))      # value not worked out: a set or an enum
            else:
                out.append(Use(nm, "enum", v, where))


def _line_use(line, where, out):
    m = LINE.match(line)
    if m:
        cmd, name, value = m.groups()
        out.append(Use(name, "set" if cmd == "setflag" else "enum", value, where))


def _imported(m, other):
    """The names module m takes from module `other` by `from other import ...`."""
    out = set()
    for n in m.tree.body:
        if isinstance(n, ast.ImportFrom) and n.module == other.name:
            out |= {a.asname or a.name for a in n.names}
    return out


def scan(ingame=HERE, scenarios=SCENARIOS):
    """(uses, unresolved) of every test/harness .py in ingame/ (fakes/ not: they model the game, they do not ask it)
    and every scenario .txt. unresolved: 'file:line func()' of each use whose name was not worked out and is not in
    DYNAMIC (whose `names`, when it has them, are checked instead)."""
    uses, unresolved, files = [], [], []
    for fn in sorted(os.listdir(ingame)):
        if fn.endswith(".py") and fn != os.path.basename(__file__):
            files.append(Module(os.path.join(ingame, fn)))
    calls = []                                         # (module, call, owner function, env) of every call
    for m in files:
        funcs = [n for n in ast.walk(m.tree) if isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef))]
        owner = {}
        for f in sorted(funcs, key=lambda f: f.lineno):      # a later (inner) function wins for its own nodes
            for n in ast.walk(f):
                owner[n] = f
        envs = {}
        for n in ast.walk(m.tree):
            if isinstance(n, ast.Call):
                f = owner.get(n)
                if f not in envs:
                    envs[f] = _scope_env(f, m)
                calls.append((m, n, f, envs[f]))

    def dynamic(m, f):
        return (os.path.basename(m.path), f.name if f else "") in DYNAMIC

    sinks = []
    for m, n, f, env in calls:
        dyn, mine = [], []
        _uses_of_call(n, env, m, f, "%s:%d" % (os.path.basename(m.path), n.lineno), uses, dyn, mine)
        sinks += [(m, f) + s for s in mine]
        if not dynamic(m, f):
            unresolved += ["%s %s()" % (w, f.name if f else "<module>") for w in dyn]
    for depth in range(3):                             # a helper's name parameter: worked out at each of its callers
        nxt = []
        for sm, sf, fname, i, attr, vnode, senv, where in sinks:
            if dynamic(sm, sf):
                continue
            found = False
            for m, n, f, env in calls:
                callee = n.func.id if isinstance(n.func, ast.Name) else (
                    n.func.attr if isinstance(n.func, ast.Attribute) else None)
                if callee != fname or len(n.args) <= i or (m is not sm and fname not in _imported(m, sm)):
                    continue
                found = True
                at = "%s:%d -> %s" % (os.path.basename(m.path), n.lineno, where)
                names = _values(n.args[i], env, m)
                if names is not None:
                    _add(names, attr, vnode, senv, sm, at, uses)
                    continue
                j = _param_index(f, n.args[i], env)
                if j is not None and depth < 2:
                    nxt.append((m, f, f.name, j, attr, vnode, senv, at))
                elif not dynamic(m, f):
                    unresolved.append("%s %s()" % (at, f.name if f else "<module>"))
            if not found:
                unresolved.append("%s %s(): no caller of it passes a name the scan sees" % (where, fname))
        sinks = nxt
    for (fn, func), d in sorted(DYNAMIC.items()):
        if d.get("names") and os.path.normcase(os.path.abspath(ingame)) == os.path.normcase(HERE):   # (they read HERE)
            for op, name in d["names"]():
                uses.append(Use(name, op, None if op == "read" else 1, "%s %s() (DYNAMIC names)" % (fn, func)))
    if os.path.isdir(scenarios):
        for fn in sorted(os.listdir(scenarios)):
            if fn.endswith(".txt"):
                with open(os.path.join(scenarios, fn), encoding="utf-8", errors="replace") as fh:
                    for i, ln in enumerate(fh, 1):
                        _line_use(ln, "scenarios/%s:%d" % (fn, i), uses)
    return uses, unresolved


# -- the table and the rules ------------------------------------------------------------------------------------------

def load_table(path=TABLE):
    with open(path, encoding="utf-8") as fh:
        return json.load(fh)["flags"]


def problems(uses, table):
    """One line per use the game would not take, naming the use, the name and why."""
    bad = []
    for u in uses:
        e = table.get(key_of(u.name))
        if e is None:
            bad.append("%s: %s %r: not in flag_table.json. %s" % (u.where, u.op, u.name, REMAKE))
            continue
        t, array = e["type"], u.name.endswith("]") and not u.name.lower().startswith("key64:")
        k64 = t == "Bool64bitKey"
        if array and t != "String64Array":
            bad.append("%s: %r: NAME[i] needs a String64Array, %s is %s" % (u.where, u.name, key_of(u.name), t))
        elif u.op == "read" and not (k64 or array or t in READ_TYPES):
            bad.append("%s: flag_read %r: a %s, which the bridge's ReadFlag does not read (kinds %s)" % (
                u.where, u.name, t, "/".join(READ_TYPES)))
        elif u.op == "set" and not k64 and (t not in SET_TYPES or HEX.match(u.name)):
            bad.append("%s: setflag %r %s: %s" % (u.where, u.name, u.value, (
                "a 0x name is hashed as text by setflag (SetFlag/FlagInt use flag_hash): write it by its name"
                if HEX.match(u.name) else "a %s; setflag writes only Int or Bool" % t)))
        elif u.op == "enum":
            if t != "Enum":
                bad.append("%s: setenum %r %s: a %s, not an Enum" % (u.where, u.name, u.value, t))
            elif u.value in ("?", "*", None):          # '?': only reads it (SetEnum); '*'/None: not worked out
                pass
            elif HEX.match(str(u.value)):
                if int(u.value, 16) not in [int(v, 16) for v in e.get("hashes", [])]:
                    bad.append("%s: setenum %r %s: not one of the Enum's values" % (u.where, u.name, u.value))
            elif u.value not in e.get("values", []):
                bad.append("%s: setenum %r %r: not one of the Enum's values %s" % (
                    u.where, u.name, u.value, e.get("values")))
        elif u.op == "write" and not (t in SET_TYPES or t == "Enum") or (u.op == "write" and HEX.match(u.name)
                                                                         and t != "Enum"):
            bad.append("%s: flag %r: %s" % (u.where, u.name, (
                "a 0x name is hashed as text by setflag (SetFlag/FlagInt use flag_hash): write it by its name"
                if HEX.match(u.name) else "a %s, which Pair.flag cannot write" % t)))
        elif u.op == "text" and t != "String64Array":
            bad.append("%s: flag %r: not a String64Array" % (u.where, u.name))
    return bad


def make_table(romfs, uses):
    """{key: entry} for every used name from the romfs's GameDataList (+ MOD); (table, missing names)."""
    sys.path.insert(0, os.path.dirname(os.path.dirname(HERE)))
    from puppet_pack import byml, zs  # noqa: E402  (tools/puppet_pack: the repo's romfs readers)
    dicts = zs.Dicts.from_romfs(romfs)
    with open(os.path.join(romfs, GAME_LIST), "rb") as fh:
        data = byml.loads(dicts.decompress(fh.read()))["Data"]

    def num(v):
        return int(getattr(v, "v", v))

    by_hash, k64 = {}, {num(r["Hash"]) for r in data["Bool64bitKey"]}
    for t, rows in data.items():
        if t != "Bool64bitKey":
            for r in rows:
                by_hash.setdefault(num(r["Hash"]), (t, r))
    table, missing = {}, []
    for k in sorted({key_of(u.name) for u in uses}):
        if k.startswith("key64:"):
            if int(k[6:], 16) in k64:
                table[k] = {"type": "Bool64bitKey", "source": GAME_LIST}
            else:
                missing.append(k)
            continue
        if k in MOD:
            t, h, src = MOD[k]
            table[k] = {"type": t, "hash": "0x%08x" % h, "source": src}
            continue
        h = int(k, 16) if HEX.match(k) else murmur3(k)
        if h not in by_hash:
            missing.append(k)
            continue
        t, r = by_hash[h]
        e = {"type": t, "hash": "0x%08x" % h, "source": GAME_LIST}
        if t == "Enum":
            e["values"] = list(r["RawValues"])
            e["hashes"] = ["0x%08x" % num(v) for v in r["Values"]]
        table[k] = e
    return table, missing


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--romfs", help="the game's romfs: (re)write flag_table.json from its GameDataList")
    a = ap.parse_args(argv)
    uses, unresolved = scan()
    if a.romfs:
        table, missing = make_table(a.romfs, uses)
        with open(TABLE, "w", encoding="utf-8", newline="\n") as fh:
            json.dump({"made_by": "tools/autotest/ingame/flag_names.py --romfs <romfs> (%s + MOD); do not edit"
                       % GAME_LIST, "flags": table}, fh, indent=1, sort_keys=True)
            fh.write("\n")
        for k in missing:
            print("NOT IN THE GAME: %s (%s)" % (k, ", ".join(u.where for u in uses if key_of(u.name) == k)))
        print("flag_table.json: %d names, %d missing" % (len(table), len(missing)))
    bad = problems(uses, load_table()) + ["%s: name not worked out (add to DYNAMIC with the reason, or use a "
                                          "constant)" % w for w in unresolved]
    for b in bad:
        print(b)
    print("%d uses, %d problems" % (len(uses), len(bad)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
