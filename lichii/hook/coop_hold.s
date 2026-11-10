// The AI hold cave in Ocarina of Time's main (build e0f3ccfaf50065ae), at
// main+0x3c46a80, right after the co-op cave (coop_svc.s). Patched in only
// with LICHII_COOP_OBJECTS_AI_HOLD (docs/objects.md, "Holding a copy's AI").
//
// The loop over all BSA components (the enemies' brains) asks, for each one
// (x21), an object for its calc bits with `blr x8` at main+0x369df8 and
// puts bits 0 to 3 of the answer into the component's u16 at +0xc4; bit 0
// is what QueryBsaIsBrainSkip reads. The patch makes that `bl 0x3c46a80`:
// this makes the same call, then, when x21 is in the table the emulator
// keeps at main+0x3c46b00, changes the answer: w0 = (w0 & and) | or.
//   table +0x00  u32 count (at most 64)
//         +0x04  u32 and
//         +0x08  u32 or
//         +0x10  u64 component, count times
// x9 to x12 are free here: the caller reloads what it needs after the call.
//
// Bytes: coop_hold.h (kHoldCave). To rebuild them (LLVM on Windows):
//   clang --target=aarch64-none-elf -c coop_hold.s -o hold.o
//   ld.lld hold.o -o hold.elf --section-start=.text=0x3c46a80 -e 0 --defsym=hold_table=0x3c46b00
//   llvm-objcopy -O binary hold.elf hold.bin

    .text
    .globl _start
_start:
    stp   x29, x30, [sp, #-0x10]!
    mov   x29, sp
    blr   x8
    adrp  x9, hold_table
    add   x9, x9, :lo12:hold_table
    ldr   w10, [x9]
    cbz   w10, done
    add   x11, x9, #0x10
next:
    ldr   x12, [x11], #8
    cmp   x12, x21
    b.eq  held
    subs  w10, w10, #1
    b.ne  next
    b     done
held:
    ldp   w10, w11, [x9, #4]
    and   w0, w0, w10
    orr   w0, w0, w11
done:
    ldp   x29, x30, [sp], #0x10
    ret
