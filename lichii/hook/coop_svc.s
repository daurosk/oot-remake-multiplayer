// The co-op code cave in Ocarina of Time's main (build e0f3ccfaf50065ae), at
// main+0x3c46280, in the zero padding after the code (the code ends at
// 0x3c461d0, Dynamic FPS uses 0x3c46200 to 0x3c46278, rodata starts at
// 0x3c47000). The patch puts `bl 0x3c46280` at main+0x6e6dd8 in place of
// `bl 0x3b4e300` (moduleCalc_, after the frame's job graph is waited on), so
// this runs once per game frame on the calc thread with that call's x0 and s0.
//
// It asks the emulator what to do with `svc #0xf4` (x0 = a 0x100-byte buffer
// on the stack, x1 = the last call's x0, x2 = 1 after a call, 0 at the start
// of the frame). The emulator returns x0 = 0 for "nothing more", or nonzero
// for "call": the buffer then holds
//   +0x00  the stack arguments the function sees (0x20 bytes)
//   +0x20  the function's address
//   +0x28  x0 to x7
// At most 16 calls a frame. Then it makes the game's own call as before.
//
// Bytes: coop_svc.cpp (kCave). To rebuild them (LLVM on Windows):
//   clang --target=aarch64-none-elf -c coop_svc.s -o cave.o
//   ld.lld cave.o -o cave.elf --section-start=.text=0x3c46280 -e 0 --defsym=frame_step=0x3b4e300
//   llvm-objcopy -O binary cave.elf cave.bin

    .text
    .globl _start
_start:
    stp   x29, x30, [sp, #-0x30]!
    mov   x29, sp
    stp   x19, x20, [sp, #0x10]
    str   d8, [sp, #0x20]
    mov   x19, x0
    fmov  s8, s0
    sub   sp, sp, #0x100
    mov   w20, #16
    mov   x1, xzr
    mov   x2, xzr
ask:
    mov   x0, sp
    svc   #0xf4
    cbz   x0, done
    subs  w20, w20, #1
    b.mi  done
    ldr   x16, [sp, #0x20]
    ldp   x0, x1, [sp, #0x28]
    ldp   x2, x3, [sp, #0x38]
    ldp   x4, x5, [sp, #0x48]
    ldp   x6, x7, [sp, #0x58]
    blr   x16
    mov   x1, x0
    mov   x2, #1
    b     ask
done:
    add   sp, sp, #0x100
    mov   x0, x19
    fmov  s0, s8
    ldr   d8, [sp, #0x20]
    ldp   x19, x20, [sp, #0x10]
    ldp   x29, x30, [sp], #0x30
    b     frame_step
