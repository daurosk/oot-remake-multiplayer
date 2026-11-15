# ExeFS code module prototype (v1 research)

The v1 plan is to ship co-op as one ExeFS code module: an AArch64 module (`subsdk1`) that the game's own loader (rtld) loads into the game's process, next to `main` and `sdk`. A Switch 2 emulator loads such a module from its mod folder. A modded console would too, but the Switch 2 has no custom firmware yet. A recomp can lift the same binary into its own build (see "Lifting"). This prototype only checks that the approach works. It is not co-op.

## What it does

`switch/proto/` builds one module that, on a Switch 2 emulator (Fritchii):

1. logs `coop-proto: hello from subsdk1 (ExeFS prototype)` when rtld runs its `_init` (after `nninitStartup`, before `nnMain`);
2. finds `main`'s base from the address of `nnMain` (rtld links the module against `main` and `sdk` by name, `nnMain` is at main+0x1202cd0) and checks the build by the offset of `malloc` (0x14c6c90);
3. applies its hook table (`switch/proto/hooks.h`, below). Entry 0 hooks the game: the per-frame `bl 0x3b4e300` at `main+0x6e70f8` (the frame site co-op has always used, `lichii/hook/coop_svc.h`) becomes a `bl` into the module, which runs its tick and jumps on to the game's frame step with every argument register kept (`start.s`). Before writing, it checks that the site still holds the game's own instruction. If another mod changed it, that hook is skipped. Entry 1 fills the slot for one game function;
4. calls that game function: the world clock setter `main+0x34522d0` (world manager, seconds), the same call `lichii/hook/coop_time.cpp` makes, once a world is loaded;
5. sends `coop-proto hello from subsdk1` three times over UDP to `127.0.0.1:27520` with the game's own `nn::socket` (from `sdk`), after initializing it if the game has not yet.

No libc, no SDK headers, no code from exlaunch (GPL-2.0) or skyline: the hook is one rewritten `bl`, made writable with `svcSetProcessMemoryPermission` and set back to read+execute.

## Lifting

A recomp such as Longshot lifts every NSO module of the game (rtld, main, subsdk*, sdk) from AArch64 into C. It finds functions from symbols, unwind-table spans, `adr`/`adrp` references and an optional `--entries` file, and it cannot follow an indirect call it does not know. A recomp that accepts our `exefs` folder would lift this module as `subsdk1`: the slot is free in this game (only `subsdk0` is used) and lands right after `subsdk0`, within a `bl`'s reach of `main`. So the module is built to lift:

- **Unwind tables kept.** Built with `-funwind-tables`; `.eh_frame` and `.eh_frame_hdr` stay in the read-only segment and MOD0 points at the header (rtld ignores them). `start.s` has CFI for its function.
- **Entry points exported** (`exports.map`): `_init`, `coop_frame_entry`, `coop_proto_frame`, the hook table `coop_hooks`, every `coop_slot_*` and every `coop_call_*` thunk. `build.py` writes `subsdk1.entries` next to the module: every exported function, one per line, `0x<module offset> <name>`.
- **No unknown indirect calls.** Every call into the game and every hook continue goes through a named slot in the hook table, in one fixed shape: C++ calls a three-instruction thunk in `start.s` (`coop_call_<name>`: `adrp`/`ldr`/`br x16` through `coop_slot_<name>`); the only other indirect branches are the PLT's import calls. No virtual functions, `std::function`, function pointers or jump tables (`-fno-jump-tables`). `tools/switch_mod/lift_check.py` decodes the module's code and fails the build on any `blr`/`br` that loads its target from anywhere else.
- **One hook table, two readers** (`hooks.h`, section `.coop_hooks`, exported as `coop_hooks`). Header: `"COOPHOOK"`, version 1, entry size 32, count, `applied_by_host`. Entries: kind, expected instruction, `main` offset, replacement, slot. Kinds: 1 call redirect (the `bl` at the offset calls the replacement; the slot gets the original target to continue at), 2 entry hook (defined, not used or applied yet), 3 game function (the slot gets `main` + offset). The linker writes module-relative addresses into the table (`--apply-dynamic-relocs`), so a lifter reads it without relocating. On an emulator, `_init` applies the table itself. A lifting recomp applies it at build time (redirect the `bl` in the lifted `main`, turn every call through a slot into a direct call), then sets `applied_by_host` to 1 in its copy of the data; `_init` then patches nothing and makes no kernel calls for hooks.

Still needed on a recomp: `nn::socket` there is a stub, so the module runs offline until the recomp implements the socket functions it imports.

## Build

Needs LLVM (clang and ld.lld, any stock build with the AArch64 target) and Python 3. No devkitPro is needed: `tools/switch_mod/elf2nso.py` writes the NSO itself.

```
python tools/switch_mod/build.py              # -> build/switch-proto/subsdk1 + subsdk1.entries (runs lift_check)
python tools/switch_mod/elf2nso_test.py -v    # converter unit tests
python tools/switch_mod/lift_check_test.py -v # instruction decoder unit tests
```

CI builds the module on Linux with the runner's LLVM and runs the lift check.

## Run on an emulator

1. Copy `build/switch-proto/subsdk1` to `<emulator user folder>/load/0400486024DEC000/coop-proto/exefs/subsdk1`. The game uses `main`, `sdk`, `subsdk0` and `rtld`; slot 1 is free.
2. Start `python tools/switch_mod/listen.py`, then the game (your own dump, build e0f3ccfa, 1.1.1).
3. In the emulator log (`Debug_Emulated` lines) look for `coop-proto:` lines: hello, `main at ...`, `hook 0: frame hook in at main+0x6e70f8`, `hook 1: game function slot`, `frame 600`, `game call: set_time ...`, `net: ...`. The listener prints the three datagrams.

Remove the `coop-proto` folder to go back to the plain game.

## What it does not cover yet

- Consoles: the Switch 2 has no custom firmware. A console would also need a `main.npdm` that grants `svcSetProcessMemoryPermission` (0x73), and would fault on the null reads the emulator returns as 0.
- Recomps (Longshot, Hookshot) load only `.ips`/`.pchtxt` byte patches, not code modules (Longshot PR #134, Hookshot PR 2900). Lifting the module needs a recomp that accepts it (above).
- PAC: the hook does not touch a signed prologue (it rewrites a `bl` and restores `x30` and `sp` before jumping on), so pointer signing is unaffected.
