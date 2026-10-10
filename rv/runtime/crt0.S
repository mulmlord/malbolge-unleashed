# Entry point of programs linked with the runtime (docs/rv.md section 6). The machine starts with
# sp at the top of guest RAM and every other register 0.
    .section .text.init, "ax", @progbits
    .globl _start
    .type _start, @function
_start:
    # Clear .bss, 16 bytes at a time (link.ld aligns both ends). The loaders already zero it; this
    # keeps the runtime independent of that.
    la t0, __bss_start
    la t1, __bss_end
    j 2f
1:  sw zero, 0(t0)
    sw zero, 4(t0)
    sw zero, 8(t0)
    sw zero, 12(t0)
    addi t0, t0, 16
2:  bltu t0, t1, 1b

    # Stack: 16-aligned (the ilp32 ABI); sbrk keeps the heap below it.
    andi sp, sp, -16
    la t0, __malm_stack_top
    sw sp, 0(t0)

    # Static constructors.
    la s0, __init_array_start
    la s1, __init_array_end
3:  bgeu s0, s1, 4f
    lw t0, 0(s0)
    jalr t0
    addi s0, s0, 4
    j 3b

    # exit(main(0, argv, envp)) with argv and envp empty vectors on the stack.
4:  addi sp, sp, -16
    sw zero, 0(sp)
    li a0, 0
    mv a1, sp
    mv a2, sp
    call main
    call exit
    .size _start, . - _start
