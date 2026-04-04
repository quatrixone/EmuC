/* Linker script for PS5 libretro cores.
 * Base address 0 so that stored function addresses equal their byte offsets
 * from the start of the binary – the frontend adds the mapped base at load time.
 */
OUTPUT_FORMAT("elf64-x86-64", "elf64-x86-64", "elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_core_start)

PHDRS
{
    text PT_LOAD FLAGS(7);
}

SECTIONS
{
    . = 0;

    .text : {
        *(.text._core_start)   /* core_header must be the very first bytes */
        *(.text)
        *(.text.*)
        *(.rodata)
        *(.rodata.*)
        *(.data)
        *(.data.*)
        *(.bss)
        *(.bss.*)
        *(COMMON)
    } :text

    /DISCARD/ : {
        *(.comment)
        *(.note.*)
        *(.eh_frame*)
        *(.got)
        *(.got.plt)
        *(.interp)
        *(.dynsym)
        *(.dynstr)
        *(.dynamic)
        *(.gnu.hash)
        *(.gnu.version*)
        *(.rela.*)
        *(.rel.*)
        *(.plt)
        *(.plt.*)
    }
}
