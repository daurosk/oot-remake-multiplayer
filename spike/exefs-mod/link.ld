/* SPDX-License-Identifier: GPL-3.0-or-later */
/* exefs spike: three page-aligned segments (code, read-only, data+bss) as an NSO holds them; elf2nso.py packs them. */
PHDRS {
    text PT_LOAD FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data PT_LOAD FLAGS(6);
    dynamic PT_DYNAMIC;
}

SECTIONS {
    . = 0;
    .text : { KEEP(*(.text.crt0)) *(.text .text.*) *(.plt .plt.*) } :text

    . = ALIGN(0x1000);
    .rodata : { KEEP(*(.rodata.mod0)) *(.rodata .rodata.*) } :rodata
    .hash : { *(.hash) } :rodata
    .gnu.hash : { *(.gnu.hash) } :rodata
    .dynsym : { *(.dynsym) } :rodata
    .dynstr : { *(.dynstr) } :rodata
    .rela.dyn : { *(.rela.dyn) } :rodata
    .rela.plt : { *(.rela.plt) } :rodata

    . = ALIGN(0x1000);
    .data : { *(.data .data.*) } :data
    .dynamic : { *(.dynamic) } :data :dynamic
    .got : { *(.got) } :data
    .got.plt : { *(.got.plt) } :data
    . = ALIGN(8);
    HIDDEN(__bss_start = .);
    .bss (NOLOAD) : {
        *(.bss .bss.*) *(COMMON)
        . = ALIGN(8);
        HIDDEN(__module_object = .);
        . += 0x200;
    } :data
    HIDDEN(__bss_end = .);

    /DISCARD/ : { *(.comment) *(.note .note.*) *(.eh_frame .eh_frame_hdr) *(.interp) }
}
