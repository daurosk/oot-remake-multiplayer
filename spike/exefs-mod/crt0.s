// SPDX-License-Identifier: GPL-3.0-or-later
// exefs spike: the module header the game's rtld reads, the SVCs the module uses, and the frame trampoline.
//
// rtld finds this module (subsdk9) by walking the process's code regions, reads the MOD0 block the word at +4 points
// to, relocates it and binds its imports (nn::socket from sdk), then calls its DT_INIT (coop_init, module.c).

    .section .text.crt0, "ax"
    .global __module_start
    .hidden __module_start
__module_start:
    b       1f                              // (never entered: rtld calls DT_INIT, not the module's start)
    .word   __nx_mod0 - __module_start
1:  ret

    .section .rodata.mod0, "a"
    .balign 4
__nx_mod0:
    .ascii  "MOD0"
    .word   _DYNAMIC - __nx_mod0
    .word   __bss_start - __nx_mod0
    .word   __bss_end - __nx_mod0
    .word   0                               // eh_frame_hdr start (none)
    .word   0                               // eh_frame_hdr end
    .word   __module_object - __nx_mod0     // rtld's ModuleObject for this module

    .text
// u32 svc_query_memory(MemoryInfo *out, u64 addr): x0 = result
    .global svc_query_memory
    .hidden svc_query_memory
svc_query_memory:
    mov     x2, x1
    svc     #0x06
    ret

// u32 svc_set_process_memory_permission(u32 handle, u64 addr, u64 size, u32 perm)
    .global svc_set_process_memory_permission
    .hidden svc_set_process_memory_permission
svc_set_process_memory_permission:
    svc     #0x73
    ret

// u32 svc_output_debug_string(const char *s, u64 len)
    .global svc_output_debug_string
    .hidden svc_output_debug_string
svc_output_debug_string:
    svc     #0x27
    ret

// void flush_code(void *word): make one written instruction visible to the instruction fetch
    .global flush_code
    .hidden flush_code
flush_code:
    dc      cvau, x0
    dsb     ish
    ic      ivau, x0
    dsb     ish
    isb
    ret

// The game's `bl frame_step` at main+0x6e70f8 becomes `bl` to a two-instruction cave in main's code padding, which
// jumps here with x30 still the game's return address. The game's arguments (x0-x8, q0-q7) are kept around the C
// hook, then the frame function runs as before (a tail branch: it returns straight to the game).
    .global coop_frame_tramp
    .hidden coop_frame_tramp
coop_frame_tramp:
    stp     x29, x30, [sp, #-0xd0]!
    mov     x29, sp
    stp     x0, x1, [sp, #0x10]
    stp     x2, x3, [sp, #0x20]
    stp     x4, x5, [sp, #0x30]
    stp     x6, x7, [sp, #0x40]
    str     x8, [sp, #0x50]
    stp     q0, q1, [sp, #0x60]
    stp     q2, q3, [sp, #0x80]
    stp     q4, q5, [sp, #0xa0]
    str     q6, [sp, #0xc0]
    str     q7, [sp, #-0x10]!               // (q7 below the frame; sp stays 16-byte aligned)
    bl      coop_frame
    ldr     q7, [sp], #0x10
    ldp     x0, x1, [sp, #0x10]
    ldp     x2, x3, [sp, #0x20]
    ldp     x4, x5, [sp, #0x30]
    ldp     x6, x7, [sp, #0x40]
    ldr     x8, [sp, #0x50]
    ldp     q0, q1, [sp, #0x60]
    ldp     q2, q3, [sp, #0x80]
    ldp     q4, q5, [sp, #0xa0]
    ldr     q6, [sp, #0xc0]
    ldp     x29, x30, [sp], #0xd0
    adrp    x16, g_frame_fn
    ldr     x16, [x16, :lo12:g_frame_fn]
    br      x16
