/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Layout of the prototype module: text (RX), rodata + dynamic linking tables + unwind
   tables (R), data (RW) then bss, each page aligned, as an NSO's three segments need.
   The unwind tables stay in: rtld ignores them, a lifting recomp finds functions by them. */
PHDRS {
    text PT_LOAD FLAGS(5);
    ro   PT_LOAD FLAGS(4);
    data PT_LOAD FLAGS(6);
    dyn  PT_DYNAMIC FLAGS(4);
    eh   PT_GNU_EH_FRAME FLAGS(4);
}

SECTIONS {
    . = 0;
    .text : {
        KEEP(*(.text.start))
        *(.text .text.*)
        *(.plt .iplt)
    } :text

    . = ALIGN(0x1000);
    .rodata : {
        KEEP(*(.rodata.mod0))
        *(.rodata .rodata.*)
    } :ro
    .eh_frame_hdr : {
        __eh_frame_hdr_start = .;
        *(.eh_frame_hdr)
        __eh_frame_hdr_end = .;
    } :ro :eh
    .eh_frame : { KEEP(*(.eh_frame)) } :ro
    .hash    : { *(.hash) } :ro
    .dynsym  : { *(.dynsym) } :ro
    .dynstr  : { *(.dynstr) } :ro
    .rela.dyn : { *(.rela.dyn) *(.rela.plt) } :ro

    . = ALIGN(0x1000);
    .dynamic : { *(.dynamic) } :data :dyn
    .got     : { *(.got) *(.got.plt) } :data
    .data    : { *(.data.rel.ro .data.rel.ro.*) *(.data .data.*) } :data
    .coop_hooks : { KEEP(*(.coop_hooks)) } :data
    __bss_start__ = .;
    .bss (NOLOAD) : { *(.bss .bss.*) *(COMMON) } :data
    __bss_end__ = .;

    /DISCARD/ : { *(.comment) *(.note.*) }
}
