OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

PHDRS
{
    text PT_LOAD FILEHDR PHDRS;
}

SECTIONS
{
    . = 0x400000 + SIZEOF_HEADERS;

    .text : {
        *(.text*)
        *(.text.*)
    } :text

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame)
        *(.eh_frame_hdr)
        *(.got)
        *(.got.plt)
        *(.bss)
        *(.rodata*)
        *(.interp)
        *(.dynsym)
        *(.dynstr)
        *(.hash)
        *(.dynamic)
        *(.symtab)
        *(.strtab)
        *(.shstrtab)
    }
}
