    .section .text.init
    .globl _start
_start:
    /* Disable M-mode interrupts during early setup */
    csrw mie, zero
    csrw mip, zero

    /* Only Hart 0 runs the test; park any extra harts */
    csrr t0, mhartid
    bnez t0, park_hart

    /* Initialize stack pointer */
    la   sp, _stack_top

    /* Zero .bss section */
    la   t0, __bss_start
    la   t1, __bss_end
1:
    bgeu t0, t1, 2f
    sd   zero, 0(t0)
    addi t0, t0, 8
    j    1b
2:
    /* Install M-mode trap vector (Direct mode) */
    la   t0, m_trap_entry
    csrw mtvec, t0

    /* Jump to C test suite */
    call main

    /* Exit Verilator / fesvr via HTIF with main() return code in a0 */
    call htif_exit

park_hart:
    wfi
    j    park_hart

/* =========================================================================
 * M-Mode Trap Entry (4-byte aligned for Direct Mode)
 * ========================================================================= */
    .align 4
    .globl m_trap_entry
m_trap_entry:
    addi sp, sp, -144
    sd   ra,   0(sp)
    sd   t0,   8(sp)
    sd   t1,  16(sp)
    sd   t2,  24(sp)
    sd   t3,  32(sp)
    sd   t4,  40(sp)
    sd   t5,  48(sp)
    sd   t6,  56(sp)
    sd   a0,  64(sp)
    sd   a1,  72(sp)
    sd   a2,  80(sp)
    sd   a3,  88(sp)
    sd   a4,  96(sp)
    sd   a5, 104(sp)
    sd   a6, 112(sp)
    sd   a7, 120(sp)

    csrr a0, mcause
    csrr a1, mepc
    call m_trap_handler

    ld   ra,   0(sp)
    ld   t0,   8(sp)
    ld   t1,  16(sp)
    ld   t2,  24(sp)
    ld   t3,  32(sp)
    ld   t4,  40(sp)
    ld   t5,  48(sp)
    ld   t6,  56(sp)
    ld   a0,  64(sp)
    ld   a1,  72(sp)
    ld   a2,  80(sp)
    ld   a3,  88(sp)
    ld   a4,  96(sp)
    ld   a5, 104(sp)
    ld   a6, 112(sp)
    ld   a7, 120(sp)
    addi sp, sp, 144
    mret
