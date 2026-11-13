// The counter cave in Ocarina of Time's main (build e0f3ccfaf50065ae), at
// main+0x3c46700 (docs/objects.md, "Moving platforms: the pause counters").
// Thanks to Hot Onion for the counter start and the node's layout.
//
// main+0x5e6760 starts ExecuteGenericCounter, the AI node that times a
// pause between two moves of a looping platform (x0 = the node), each time
// the node is entered. Its first instruction, `pacibsp` (a hint, nothing on
// this CPU), becomes `b 0x3c46700`: this keeps the node in a ring the
// emulator reads, does the `pacibsp` and goes back to main+0x5e6764.
// The ring is in the game's heap (the game's own malloc, coop_objects.cpp);
// its address is the u64 at main+0x3c46780, 0 until it is made (then this
// only does the `pacibsp`). Code pages cannot be written by the game.
//   ring +0x00  u32 count of nodes kept (wraps)
//        +0x08  u64 node, 256 of them, at count % 256 after the add
// x16 and x17 are free at a function's start (AAPCS64 IP0/IP1).
//
// Bytes: coop_counter.h (kCave). To rebuild them (LLVM on Windows):
//   clang --target=aarch64-none-elf -c coop_counter.s -o counter.o
//   ld.lld counter.o -o counter.elf --section-start=.text=0x3c46700 -e 0 --defsym=counter_back=0x5e6764 --defsym=counter_ring=0x3c46780
//   llvm-objcopy -O binary counter.elf counter.bin

    .text
    .globl _start
_start:
    hint  #27                     // pacibsp, the instruction the hook replaced
    ldr   x16, counter_ring
    cbz   x16, 1f
    ldr   w17, [x16]
    add   w17, w17, #1
    str   w17, [x16]
    and   w17, w17, #255
    add   x16, x16, #8
    str   x0, [x16, w17, uxtw #3]
1:
    b     counter_back
