/* Layout of the prototype module: text (RX), rodata + dynamic linking tables (R),
   data (RW) then bss, each page aligned, as an NSO's three segments need. */
PHDRS {
    text PT_LOAD FLAGS(5);
    ro   PT_LOAD FLAGS(4);
    data PT_LOAD FLAGS(6);
    dyn  PT_DYNAMIC FLAGS(4);
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
    .hash    : { *(.hash) } :ro
    .dynsym  : { *(.dynsym) } :ro
    .dynstr  : { *(.dynstr) } :ro
    .rela.dyn : { *(.rela.dyn) *(.rela.plt) } :ro

    . = ALIGN(0x1000);
    .dynamic : { *(.dynamic) } :data :dyn
    .got     : { *(.got) *(.got.plt) } :data
    .data    : { *(.data.rel.ro .data.rel.ro.*) *(.data .data.*) } :data
    __bss_start__ = .;
    .bss (NOLOAD) : { *(.bss .bss.*) *(COMMON) } :data
    __bss_end__ = .;

    /DISCARD/ : { *(.eh_frame) *(.eh_frame_hdr) *(.comment) *(.note.*) }
}
