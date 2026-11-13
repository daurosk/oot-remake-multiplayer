r"""Splits tools/autotest/groups/hookshot-parity.txt into wave files of at most 3 groups (6 games at once):
hookshot-parity-wave<N>.txt. groups.py starts every group of a file at once, so a wave is what this PC
runs at a time. Each wave keeps the master's header and renumbers its groups g1..g3, so it reuses the
pairs hookshot-A1..3 / -B1..3 (the saves' setup steps in the header name the master's groups: see the
wave's own comment line). Run from the repo root: python -I tools/autotest/groups/mkwaves.py"""
import re

MASTER = "tools/autotest/groups/hookshot-parity.txt"
text = open(MASTER, encoding="utf-8").read()
first = re.search(r"^\[g\d+\]$", text, re.M)
header, body = text[:first.start()], text[first.start():]
blocks = re.split(r"(?m)^(?=\[g\d+\]$)", body)
blocks = [b.rstrip("\n") + "\n" for b in blocks if b.strip()]
waves = [blocks[i:i + 3] for i in range(0, len(blocks), 3)]
for n, wave in enumerate(waves, 1):
    names = [re.match(r"\[(g\d+)\]", b).group(1) for b in wave]
    out = [header.rstrip("\n"), "",
           f"# WAVE {n} of {len(waves)}: the master's {', '.join(names)}, made by tools/autotest/groups/mkwaves.py from {MASTER.split('/')[-1]}",
           f"# (do not edit here: edit the master and make the waves again). Here they are g1..g{len(wave)}: pairs",
           f"# hookshot-A1..{len(wave)} / -B1..{len(wave)}. A setup step above that names a master group (g3, g4, g7) applies to",
           "# the pair that group has here.", ""]
    for i, b in enumerate(wave, 1):
        out.append(re.sub(r"^\[g\d+\]", f"[g{i}]", b, count=1))
    path = MASTER.replace(".txt", f"-wave{n}.txt")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(out))
    print(path, names)
