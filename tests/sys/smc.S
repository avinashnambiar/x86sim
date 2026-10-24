/* Self- and cross-modifying code (run with --smp 2 --cache; smcnocache.S
 * runs the same program without the caches). The decoded-instruction cache
 * must notice every store to code, and with the caches on, a fetch must see
 * a store that still sits modified in another core's L1D.
 *   SMC: patch an instruction that already ran; patch the very next
 *        instruction (no jump in between); overwrite a routine with
 *        REP MOVSB; a loop that rewrites its own body halfway through.
 *   CMC: the BSP spins calling a routine until the AP patches it; the AP
 *        runs a routine, then runs it again after the BSP rewrote it (with
 *        CPUID to serialize, as the SDM's cross-modifying protocol asks). */

#define CODE32 0x08
#define DATA0 0x10
#define LAPIC 0xFEE00000

    .code16
    .globl _start
_start:
    cli
    xor %ax, %ax
    mov %ax, %ds
    mov %ax, %ss
    mov $0x7C00, %sp
    lgdtl gdt_desc
    mov %cr0, %eax
    or $1, %eax
    mov %eax, %cr0
    ljmpl $CODE32, $bsp32

ap_entry16:
    cli
    xor %ax, %ax
    mov %ax, %ds
    lgdtl gdt_desc
    mov %cr0, %eax
    or $1, %eax
    mov %eax, %cr0
    ljmpl $CODE32, $ap32

    .code32
bsp32:
    mov $DATA0, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    mov $0x90000, %esp
    cld

    /* SMC 1: patch the immediate of code that already ran */
    mov $msg_smc1, %esi
    call puts
    call f
    call print_hex
    movl $2, f + 1
    call f
    call print_hex
    call newline

    /* SMC 2: the store rewrites the next instruction's immediate; twice,
     * so the second time that instruction has already been decoded */
    mov $msg_smc2, %esi
    call puts
    xor %eax, %eax
    mov $0x42, %dl
    mov $2, %ecx
1:  mov %dl, 2f + 1
2:  mov $0x11, %al
    inc %dl
    loop 1b
    call print_hex                      /* 0x43 */
    call newline

    /* SMC 3: copy a new routine over f with REP MOVSB */
    mov $msg_smc3, %esi
    call puts
    mov $g, %esi
    mov $f, %edi
    mov $g_end - g, %ecx
    rep movsb
    call f
    call print_hex
    call newline

    /* SMC 4: ten iterations; the fifth changes "add $1" to "add $16" */
    mov $msg_smc4, %esi
    call puts
    xor %ebx, %ebx
    mov $10, %ecx
1:  cmp $5, %ecx
    jne 2f
    movb $16, 3f + 2
2:
3:  add $1, %ebx
    loop 1b
    mov %ebx, %eax                      /* 5 * 1 + 5 * 16 = 0x55 */
    call print_hex
    call newline

    /* start the AP; it runs buf (0x11) once before we rewrite it */
    mov $r11, %esi
    mov $buf, %edi
    mov $r11_end - r11, %ecx
    rep movsb
    movb $0xEA, 0x70000                 /* trampoline: ljmp $0, $ap_entry16 */
    mov $ap_entry16, %eax
    mov %ax, 0x70001
    movw $0, 0x70003
    movl $0x000C4500, LAPIC + 0x300     /* INIT to all excluding self */
    movl $0x000C4670, LAPIC + 0x300     /* STARTUP, vector 0x70 */

    /* CMC 1: spin calling h until the AP patches it to return 7 */
    mov $msg_cmc1, %esi
    call puts
1:  call h
    test %eax, %eax
    jz 1b
    call print_hex
    call newline

    /* CMC 2: the AP ran buf once; rewrite it and let it run it again */
1:  pause
    cmpl $1, ap_step
    jne 1b
    mov $r99, %esi
    mov $buf, %edi
    mov $r99_end - r99, %ecx
    rep movsb
    movl $1, go
1:  pause
    cmpl $2, ap_step
    jne 1b
    mov $msg_cmc2, %esi
    call puts
    mov ap_res1, %eax
    call print_hex
    mov ap_res2, %eax
    call print_hex
    call newline

    mov $msg_bye, %esi
    call puts
    xor %al, %al
    out %al, $0xF4
    hlt

ap32:
    mov $DATA0, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    mov $0x80000, %esp
    call buf                            /* the old routine: 0x11 */
    mov %eax, ap_res1
    mov $500, %ecx                      /* let the BSP spin on h for a while */
1:  loop 1b
    movl $7, h + 1                      /* CMC 1: patch the BSP's routine */
    movl $1, ap_step
1:  pause
    cmpl $0, go
    je 1b
    xor %eax, %eax
    cpuid                               /* serialize before running new code */
    call buf                            /* the new routine: 0x99 */
    mov %eax, ap_res2
    movl $2, ap_step
1:  hlt
    jmp 1b

/* patched routines: mov $imm32, %eax; ret */
    .p2align 6
f:  .byte 0xB8
    .long 1
    ret
g:  mov $3, %eax
    ret
g_end:
    .p2align 6
h:  .byte 0xB8
    .long 0
    ret
r11: mov $0x11, %eax
    ret
r11_end:
r99: mov $0x99, %eax
    ret
r99_end:
    .p2align 6
buf: .space 64, 0xCC

puts:
    push %eax
1:  lodsb
    test %al, %al
    jz 2f
    out %al, $0xE9
    jmp 1b
2:  pop %eax
    ret

newline:
    push %eax
    mov $'\n', %al
    out %al, $0xE9
    pop %eax
    ret

print_hex:
    push %ecx
    push %eax
    mov $8, %ecx
1:  rol $4, %eax
    push %eax
    and $15, %eax
    mov hexdig(%eax), %al
    out %al, $0xE9
    pop %eax
    loop 1b
    mov $' ', %al
    out %al, $0xE9
    pop %eax
    pop %ecx
    ret

    .data
    .p2align 6
go: .long 0
    .p2align 6
ap_step: .long 0
    .p2align 6
ap_res1: .long 0
ap_res2: .long 0
hexdig: .ascii "0123456789abcdef"
msg_smc1: .asciz "smc patch executed code "
msg_smc2: .asciz "smc patch next instruction "
msg_smc3: .asciz "smc rep movsb over routine "
msg_smc4: .asciz "smc loop rewrites itself "
msg_cmc1: .asciz "cmc bsp sees ap's patch "
msg_cmc2: .asciz "cmc ap runs old, then bsp's new code "
msg_bye: .asciz "bye\n"
    .p2align 3
gdt:
    .quad 0
    .quad 0x00CF9A000000FFFF            /* 0x08 code32 */
    .quad 0x00CF92000000FFFF            /* 0x10 data */
gdt_end:
gdt_desc:
    .word gdt_end - gdt - 1
    .long gdt
