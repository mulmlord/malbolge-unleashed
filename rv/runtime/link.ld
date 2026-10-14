/* Guest memory layout (docs/rv.md section 6): one image from 0x1000 up, then the heap growing up
   from _end through brk, and the stack growing down from the top of guest RAM, where the machine
   sets sp. sbrk keeps the heap __stack_size bytes below the initial sp (override it with
   --defsym=__stack_size=...). Also used for riscv-tests and the bare test programs. */
OUTPUT_ARCH(riscv)
ENTRY(_start)
PROVIDE(__stack_size = 64K);

SECTIONS
{
    . = 0x1000;
    .text : { KEEP(*(.text.init)) *(.text .text.*) }
    .rodata : { *(.rodata .rodata.* .srodata .srodata.*) }
    /* C++ exceptions (-fexceptions): libunwind finds the unwind tables through these symbols, the
       index (.eh_frame_hdr) only with --eh-frame-hdr. Not KEEP: a dead function's entry goes
       too. */
    .eh_frame_hdr : {
        PROVIDE_HIDDEN(__eh_frame_hdr_start = .);
        *(.eh_frame_hdr)
        PROVIDE_HIDDEN(__eh_frame_hdr_end = .);
    }
    .eh_frame : {
        PROVIDE_HIDDEN(__eh_frame_start = .);
        *(.eh_frame)
        PROVIDE_HIDDEN(__eh_frame_end = .);
    }
    .init_array : {
        PROVIDE_HIDDEN(__init_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
        KEEP(*(.init_array))
        PROVIDE_HIDDEN(__init_array_end = .);
    }
    /* Run last to first by picolibc's exit; .fini_array_onexit (its atexit functions) runs first. */
    .fini_array : {
        PROVIDE_HIDDEN(__fini_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*)))
        KEEP(*(.fini_array))
        KEEP(*(.fini_array_onexit))
        PROVIDE_HIDDEN(__fini_array_end = .);
    }
    .data : { *(.data .data.* .sdata .sdata.*) }
    .bss : {
        . = ALIGN(16);
        __bss_start = .;
        *(.sbss .sbss.* .bss .bss.* COMMON)
        . = ALIGN(16);
        __bss_end = .;
    }
    _end = .;
}
