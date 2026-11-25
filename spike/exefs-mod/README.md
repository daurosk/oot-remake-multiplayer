# exefs spike: co-op as an exefs code mod (research, not shipped)

A tiny subsdk9 (an NSO) that runs inside Ocarina of Time 1.1.1 (main build e0f3ccfaf50065ae) on an emulator that loads
exefs mods: it hooks the game's per-frame call (main+0x6e70f8, `bl frame_step` in moduleCalc_), opens a UDP socket
with the game's own `nn::socket`, sends one ENet CONNECT to coop_server on 127.0.0.1:27510 and logs the server's
VERIFY_CONNECT with svcOutputDebugString (the emulator's log, `Debug_Emulated`).

- `crt0.s`: MOD0 header, SVC wrappers, the frame trampoline. `module.c`: the hook and the hello (freestanding C).
- `link.ld` + `elf2nso.py`: three page-aligned segments packed into an uncompressed NSO0.
- `build.ps1`: open LLVM (clang + ld.lld, `--target=aarch64-none-elf`) and the repo's Python. Output: `out\coop-exefs\exefs\subsdk9`.
- `run-fritchii.ps1`: unattended run on a stock Fritchii (keys/firmware/config copied from a kit's emulator).

Install by hand: copy `out\coop-exefs` to `<Fritchii>\user\load\0400486024DEC000\`.
The findings and the v1.0 plan: coord report `exefs-spike.md`.
