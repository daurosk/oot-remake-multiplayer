"""Static check of '%' format strings in the bridge tests (gate-ui-epona-0121-4, 2026-11-27).

probe0121-c53046a6 (lane 1, 22:41 UTC) lost its whole Epona measurement to a TypeError ('not enough arguments for
format string') in a note() line: 5 placeholders, 4 values. --lane-check-only said SETUP-ONLY OK, because nothing read
the test source. This check runs at collection (bt_plugin), so a run and its --lane-check-only fail in seconds
(SETUP-FAIL source) instead of in the middle of a lane run.

It only judges what it can count for sure: a str literal % a tuple literal (no *starred), and a str literal % a
single non-tuple literal. '%%' is not a placeholder; a '*' width/precision takes one more value; a '%(name)s' string
is left out (it takes a mapping).
"""
import ast
import re

# (printf-style spec as Python's % operator reads it: flags, width, precision, length, conversion)
_SPEC = re.compile(r"%(\([^)]*\))?([#0\- +]*)(\*|\d+)?(?:\.(\*|\d+))?[hlL]?([diouxXeEfFgGcrsa%])")


def count_values(fmt):
    """Values a '%' format string takes, or None when it cannot be counted (a mapping key, a bad spec)."""
    n, pos = 0, 0
    while True:
        i = fmt.find("%", pos)
        if i < 0:
            return n
        m = _SPEC.match(fmt, i)
        if not m:
            return None
        if m.group(1) is not None:
            return None
        if m.group(5) != "%":
            n += 1 + (m.group(3) == "*") + (m.group(4) == "*")
        pos = m.end()


def problems(source, filename="<src>"):
    """[(line, message)] for each '%' whose value count does not match its format string."""
    out = []
    for node in ast.walk(ast.parse(source, filename)):
        if not (isinstance(node, ast.BinOp) and isinstance(node.op, ast.Mod)):
            continue
        if not (isinstance(node.left, ast.Constant) and isinstance(node.left.value, str)):
            continue
        want = count_values(node.left.value)
        if want is None:
            continue
        r = node.right
        if isinstance(r, ast.Tuple):
            if any(isinstance(e, ast.Starred) for e in r.elts):
                continue
            got = len(r.elts)
        elif isinstance(r, (ast.Constant, ast.JoinedStr, ast.List, ast.ListComp)) and \
                not (isinstance(r, ast.Constant) and isinstance(r.value, tuple)):
            got = 1
        else:
            continue
        if got != want:
            out.append((node.lineno, "format string takes %d value(s), given %d: %r" % (
                want, got, node.left.value[:80])))
    return out


def check_files(paths):
    """['file:line: message'] for every problem in these files (a file that does not parse is reported too)."""
    out = []
    for p in sorted(set(paths)):
        try:
            with open(p, encoding="utf-8") as f:
                src = f.read()
            out += ["%s:%d: %s" % (p, ln, msg) for ln, msg in problems(src, p)]
        except (OSError, SyntaxError, ValueError) as e:
            out.append("%s: cannot check: %s" % (p, e))
    return out
