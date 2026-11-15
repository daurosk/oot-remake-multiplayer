// Module start and the frame hook's entry (AArch64).
//
// A Switch module starts with a branch and the offset of its MOD0 header;
// rtld finds MOD0 through it, relocates the module from its .dynamic and
// keeps its module object at MOD0 + 0x18 (inside our bss).

    .section .text.start, "ax"
    .global __module_start
__module_start:
    b       1f
    .word   __mod0 - __module_start
1:  ret                                 // never entered: rtld calls _init

// The game's `bl frame_step` at main+0x6e70f8 is turned into `bl coop_frame_entry`.
// x30 holds the game's return address; every argument register is kept, our
// tick runs, then we jump on to the real frame step as if the game had called it.
    .section .text, "ax"
    .global coop_frame_entry
    .protected coop_frame_entry
    .type coop_frame_entry, %function
coop_frame_entry:
    .cfi_startproc
    sub     sp, sp, #0x120
    .cfi_def_cfa_offset 0x120
    stp     x0, x1, [sp, #0x00]
    stp     x2, x3, [sp, #0x10]
    stp     x4, x5, [sp, #0x20]
    stp     x6, x7, [sp, #0x30]
    stp     x8, x29, [sp, #0x40]
    str     x30, [sp, #0x50]
    stp     q0, q1, [sp, #0x60]
    stp     q2, q3, [sp, #0x80]
    stp     q4, q5, [sp, #0xa0]
    stp     q6, q7, [sp, #0xc0]
    .cfi_offset x29, -0xd8
    .cfi_offset x30, -0xd0
    add     x29, sp, #0x40
    bl      coop_proto_frame
    ldp     q6, q7, [sp, #0xc0]
    ldp     q4, q5, [sp, #0xa0]
    ldp     q2, q3, [sp, #0x80]
    ldp     q0, q1, [sp, #0x60]
    ldr     x30, [sp, #0x50]
    ldp     x8, x29, [sp, #0x40]
    ldp     x6, x7, [sp, #0x30]
    ldp     x4, x5, [sp, #0x20]
    ldp     x2, x3, [sp, #0x10]
    ldp     x0, x1, [sp, #0x00]
    add     sp, sp, #0x120
    .cfi_def_cfa_offset 0
    .cfi_restore x29
    .cfi_restore x30
    // Continue through the hook table's slot (a lifting recomp makes this a direct branch).
    adrp    x16, coop_slot_frame_step
    ldr     x16, [x16, :lo12:coop_slot_frame_step]
    br      x16
    .cfi_endproc
    .size coop_frame_entry, . - coop_frame_entry

// Calls into the game: one thunk per game-function slot of the hook table (hooks.h).
// C++ calls the thunk with a plain `bl`; the thunk's tail jump through the slot is the
// only indirect branch, in one fixed shape a lifting recomp turns into a direct call.
    .macro game_fn name, slot
    .section .text, "ax"
    .global \name
    .protected \name
    .type \name, %function
\name:
    .cfi_startproc
    adrp    x16, \slot
    ldr     x16, [x16, :lo12:\slot]
    br      x16
    .cfi_endproc
    .size \name, . - \name
    .endm

    game_fn coop_call_set_time, coop_slot_set_time

    .section .rodata.mod0, "a"
    .balign 4
    .global __mod0
__mod0:
    .ascii  "MOD0"
    .word   _DYNAMIC - __mod0
    .word   __bss_start__ - __mod0
    .word   __bss_end__ - __mod0
    .word   __eh_frame_hdr_start - __mod0   // unwind tables, for a lifter's function spans
    .word   __eh_frame_hdr_end - __mod0
    .word   __module_object - __mod0

    .section .bss.module_object, "aw", %nobits
    .balign 8
__module_object:
    .space  0x200
