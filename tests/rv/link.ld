/* Layout of the test ELFs (riscv-tests and tests/rv/programs): one image from 0x1000 up in guest
   RAM [0, ram size); the reference emulator sets sp to the top of RAM. */
OUTPUT_ARCH(riscv)
ENTRY(_start)

SECTIONS
{
    . = 0x1000;
    .text : { *(.text.init) *(.text .text.*) }
    .rodata : { *(.rodata .rodata.* .srodata .srodata.*) }
    .data : { *(.data .data.* .sdata .sdata.*) }
    .bss : { *(.sbss .sbss.* .bss .bss.* COMMON) }
    _end = .;
}
