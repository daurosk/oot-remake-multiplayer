# ExeFS code module prototype (v1 research)

The v1 plan is to ship co-op as one ExeFS code module: an AArch64 module (`subsdk9`) that the game's own loader (rtld) loads into the game's process, next to `main` and `sdk`. A Switch 2 emulator loads such a module from its mod folder. A modded console would too, but the Switch 2 has no custom firmware yet. This prototype only checks that the approach works. It is not co-op.

## What it does

`switch/proto/` builds one module that, on a Switch 2 emulator (Fritchii):

1. logs `coop-proto: hello from subsdk9 (ExeFS prototype)` when rtld runs its `_init` (after `nninitStartup`, before `nnMain`);
2. finds `main`'s base from the address of `nnMain` (rtld links the module against `main` and `sdk` by name) and checks the build by the offsets of `nnMain` (0x1202cd0) and `malloc` (0x14c6c90);
3. hooks the game: the per-frame `bl 0x3b4e300` at `main+0x6e70f8` (the frame site co-op has always used, `lichii/hook/coop_svc.h`) becomes a `bl` into the module, which runs its tick and jumps on to the game's frame step with every argument register kept (`start.s`). Before writing, it checks that the site still holds the game's own instruction. If another mod changed it, the module installs no hook;
4. calls one game function: the world clock setter `main+0x34522d0` (world manager, seconds), the same call `lichii/hook/coop_time.cpp` makes, once a world is loaded;
5. sends `coop-proto hello from subsdk9` three times over UDP to `127.0.0.1:27520` with the game's own `nn::socket` (from `sdk`), after initializing it if the game has not yet.

No libc, no SDK headers, no code from exlaunch (GPL-2.0) or skyline: the hook is one rewritten `bl`, made writable with `svcSetProcessMemoryPermission` and set back to read+execute.

## Build

Needs LLVM (clang and ld.lld, any stock build with the AArch64 target) and Python 3. No devkitPro is needed: `tools/switch_mod/elf2nso.py` writes the NSO itself.

```
python tools/switch_mod/build.py            # -> build/switch-proto/subsdk9
python tools/switch_mod/elf2nso_test.py -v  # converter unit tests
```

## Run on an emulator

1. Copy `build/switch-proto/subsdk9` to `<emulator user folder>/load/0400486024DEC000/coop-proto/exefs/subsdk9`. The game uses `main`, `sdk`, `subsdk0` and `rtld`; slot 9 is free.
2. Start `python tools/switch_mod/listen.py`, then the game (your own dump, build e0f3ccfa, 1.1.1).
3. In the emulator log (`Debug_Emulated` lines) look for `coop-proto:` lines: hello, `main at ...`, `frame hook in`, `frame 600`, `game call: set_time ...`, `net: ...`. The listener prints the three datagrams.

Remove the `coop-proto` folder to go back to the plain game.

## What it does not cover yet

- Consoles: the Switch 2 has no custom firmware. A console would also need a `main.npdm` that grants `svcSetProcessMemoryPermission` (0x73), and would fault on the null reads the emulator returns as 0.
- Recomps (Longshot, Hookshot) load only `.ips`/`.pchtxt` byte patches, not code modules (Longshot PR #134, Hookshot PR 2900).
- PAC: the hook does not touch a signed prologue (it rewrites a `bl` and restores `x30` and `sp` before jumping on), so pointer signing is unaffected.
