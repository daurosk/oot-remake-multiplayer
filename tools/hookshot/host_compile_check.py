"""Compiles the Longshot files our patches touch, syntax only, with Longshot's own compiler and build.py's own flags.

Run by Longshot's Python on Longshot's source with our patches applied (tools/hookshot/host_compile_check.ps1 on Windows,
tools/hookshot/host_compile_check.sh on Linux):

    python -I host_compile_check.py <patched source> <hookshot/patches folder>

nx2/build.py --standalone --dry-run is run in this process (no game needed: it stops where the compile would start), and
its list of compiles is taken from it at that point: each job's compiler, source and flags, exactly as a game build would
run them on this system. Of those, the files a patch changes and the files that include a header a patch changes are
compiled again here with -fsyntax-only (host/*.c, host/posix/*.c on Linux, gpu/vk/*.cpp). Unknown escape sequences and
multi-character constants are errors (a lost backslash makes either). Before that, the lift's two generated headers
that rt/a64.h includes (a64_suyu.h, a64_fpx.h) are written into the gen folder on the jobs' -I path by the lift's own
generators, as nx2/lift.py writes them: the dry run has no lift, and without them every file that includes hos.h stops
at a64.h before our code is parsed. Then every .py a patch changes is compiled
(py_compile). A patched C or C++ file the dry run would not compile on this system is listed as such (host/posix on
Windows, display.c on Linux), never silently left out.

Exit status 0 only when every file compiled. Needs the toolchain environment of a kit build (Windows: LONGSHOT_MINGW and
RX_CPM; Linux: linux_tools.py env) and OOT_WORK (an empty work folder).
"""
import builtins
import os
import py_compile
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hs  # noqa: E402  (run_quiet: a console child with no window)

EXTRA = ["-Wunknown-escape-sequence", "-Werror=unknown-escape-sequence", "-Wmultichar", "-Werror=multichar"]
C_EXT = (".c", ".cpp", ".cc")
H_EXT = (".h", ".hpp", ".inc")


class Captured(Exception):
    pass


def dry_run_jobs(src):
    """build.py's compile jobs (cc, source, object, flags, deps) for a --standalone build on this system"""
    nx2 = os.path.join(src, "nx2")
    sys.path.insert(0, nx2)
    saved = sys.argv
    sys.argv = [os.path.join(nx2, "build.py"), "--standalone", "--dry-run"]
    try:
        import build
        got = {}

        def capture(*args, **kw):
            # (build.py's dry run says "dry run: N files to compile ..." with its jobs in the caller's locals)
            if args and isinstance(args[0], str) and args[0].startswith("dry run: ") and "files to compile" in args[0]:
                got["jobs"] = list(sys._getframe(1).f_locals["jobs"])
                raise Captured()
            builtins.print(*args, **kw)

        build.print = capture
        try:
            status = build.main()
        except Captured:
            return got["jobs"]
        raise SystemExit("host_compile_check: build.py --dry-run ended (status %r) before its list of compiles" % (status,))
    finally:
        sys.argv = saved


def write_lift_headers(src, jobs):
    """a64_suyu.h and a64_fpx.h, the headers rt/a64.h includes from the lift's gen folder, written there by the lift's own
    generators with nx2/lift.py's changes installed (a64_rtgen.py + suyu_rt_fixes; a64_lift.Lifter.write's a64_fpx.h, with
    fastfp's addition). The dry run makes the gen folder but no lift, so they are missing there. The guard shapes of a64_fpx.h
    are the ones the lift found in the game (none here): host code does not call those helpers by name."""
    import kit
    gen = os.path.join(kit.WORK, "sa", "gen")
    if not any("-I" + gen in job[3] for job in jobs):
        raise SystemExit("host_compile_check: the lift's gen folder %s is not on the dry run's include path: "
                         "update write_lift_headers" % gen)
    for sub in ("lift", "tools"):   # (as nx2/lift.py and kit.py: the vendored a64-kit)
        p = os.path.join(kit.KIT, sub)
        if p not in sys.path:
            sys.path.insert(0, p)
    import a64_lift
    import a64_rtgen
    import fastfp
    import insn
    import lift_fixes
    import suyu_rt_fixes
    insn.install()
    fastfp.install()
    lift_fixes.install()
    suyu_rt_fixes.install()
    a64_rtgen.write(gen)   # (builds a64emit with the host compiler first, as a lift does: build_emit.py)
    with open(os.path.join(gen, "a64_fpx.h"), "w", newline="\n") as f:   # (a64_lift.Lifter.write, then fastfp's wrap)
        f.write(a64_lift.fpx_header(set()))
        if a64_lift.local_flags():
            f.write(a64_lift.FCMPK_HEADER)
        if fastfp.fastfp_mode():
            f.write(fastfp.FASTFP_HEADER)
    for name in ("a64_suyu.h", "a64_fpx.h"):
        print("generated %s (%d bytes) in %s" % (name, os.path.getsize(os.path.join(gen, name)), gen), flush=True)


def touched_files(patches):
    out = set()
    for name in sorted(os.listdir(patches)):
        if name.endswith(".patch"):
            with open(os.path.join(patches, name), encoding="utf-8", errors="replace") as f:
                for line in f:
                    m = re.match(r"\+\+\+ b/(\S+)", line)
                    if m:
                        out.add(m.group(1))
    return out


def main():
    src, patches = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
    touched = touched_files(patches)
    jobs = dry_run_jobs(src)
    write_lift_headers(src, jobs)
    rel = lambda p: os.path.relpath(os.path.abspath(p), src).replace(os.sep, "/")
    headers = {os.path.basename(t) for t in touched if t.endswith(H_EXT)}
    inc = re.compile(r'#\s*include\s*"(?:[^"]*/)?([^"/]+)"')
    picked, seen = [], set()
    for job in jobs:
        cc, path, _obj, flags = job[0], job[1], job[2], job[3]
        r = rel(path)
        if r in seen or not r.endswith(C_EXT) or not (r.startswith("host/") or r.startswith("gpu/vk/")) or not os.path.isfile(path):
            continue
        why = "patched" if r in touched else ""
        if not why and headers:
            with open(path, encoding="utf-8", errors="replace") as f:
                names = set(inc.findall(f.read()))
            hit = sorted(names & headers)
            if hit:
                why = "includes " + ", ".join(hit)
        if why:
            seen.add(r)
            picked.append((r, why, cc, path, flags))
    failed = []
    for r, why, cc, path, flags in sorted(picked):
        # (the job's own flags; -c/-o and dependency-file options are not in them: compile_one adds those)
        cmd = [cc] + list(flags) + ["-fsyntax-only"] + EXTRA + [path]
        p = hs.run_quiet(cmd, cwd=os.path.join(src, "nx2"), capture_output=True, text=True, errors="replace")
        if p.returncode:
            failed.append(r)
            print("FAIL %s (%s)\n%s" % (r, why, (p.stdout + p.stderr)[-4000:]), flush=True)
        else:
            print("ok   %s (%s)" % (r, why), flush=True)
    for t in sorted(touched):
        if t.endswith(C_EXT) and t not in seen and not os.path.isfile(os.path.join(src, t)):
            print("not in the patched source (a later patch deletes it): %s" % t, flush=True)
        elif t.endswith(C_EXT) and t not in seen:
            print("not compiled on this system (build.py has no job for it here): %s" % t, flush=True)
    with tempfile.TemporaryDirectory() as tmp:
        for t in sorted(x for x in touched if x.endswith(".py")):
            try:
                py_compile.compile(os.path.join(src, t), cfile=os.path.join(tmp, "x.pyc"), doraise=True)
                print("ok   %s" % t, flush=True)
            except py_compile.PyCompileError as e:
                failed.append(t)
                print("FAIL %s\n%s" % (t, e.msg), flush=True)
    if not picked:
        print("host_compile_check: no file to compile was found (the dry run's list had %d jobs)" % len(jobs))
        return 1
    if failed:
        print("host_compile_check: %d file(s) did not compile: %s" % (len(failed), ", ".join(failed)))
        return 1
    print("host_compile_check: %d C/C++ file(s) and the patched Python files compile" % len(picked))
    return 0


if __name__ == "__main__":
    sys.exit(main())
