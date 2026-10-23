/* Many-CPU test (run with --smp 32 --cache): the bootstrap processor reads
 * the CPU count from CPUID leaf 0xB, wakes every application processor at
 * once with broadcast INIT + STARTUP IPIs (all excluding self), and each AP
 * checks in by setting the bit of its APIC ID. All CPUs then hammer one
 * shared counter with LOCK INC, and finally the BSP broadcasts a fixed IPI
 * that every AP must take. */

#define CODE32 0x08
#define DATA0 0x10
#define LAPIC 0xFEE00000
#define N 1000

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

ap_entry16:                             /* every AP, through the trampoline at 0x70000 */
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
    mov $ipi_handler, %eax              /* vector 0x40 */
    mov %ax, idt + 0x40 * 8
    movw $CODE32, idt + 0x40 * 8 + 2
    movw $0x8E00, idt + 0x40 * 8 + 4
    shr $16, %eax
    mov %ax, idt + 0x40 * 8 + 6
    lidt idt_desc
    movl $0x1FF, LAPIC + 0xF0

    mov $0xB, %eax                      /* logical processors at the core level */
    mov $1, %ecx
    cpuid
    movzx %bx, %ebx
    mov %ebx, ncpus
    mov $msg_n, %esi
    call puts
    mov ncpus, %eax
    call print_hex
    mov $1, %eax                        /* addressable IDs in leaf 1 EBX[23:16] */
    cpuid
    shr $16, %ebx
    movzx %bl, %eax
    call print_hex
    call newline

    movb $0xEA, 0x70000                 /* trampoline: ljmp $0, $ap_entry16 */
    mov $ap_entry16, %eax
    mov %ax, 0x70001
    movw $0, 0x70003
    movl $0x000C4500, LAPIC + 0x300     /* INIT to all excluding self */
    movl $0x000C4670, LAPIC + 0x300     /* STARTUP, vector 0x70, to all excluding self */
    mov ncpus, %ebx
    dec %ebx
1:  pause
    cmp arrived, %ebx
    jne 1b
    mov $msg_up, %esi
    call puts
    mov arrived, %eax
    call print_hex
    xor %eax, %eax                      /* distinct APIC IDs: one bit each */
    xor %ecx, %ecx
2:  popcnt bitmap(,%ecx,4), %edx
    add %edx, %eax
    inc %ecx
    cmp $8, %ecx
    jne 2b
    call print_hex
    mov bitmap, %eax                    /* bit 0 (the BSP) stays clear */
    and $1, %eax
    call print_hex
    call newline

    movl $1, go
    mov $N, %ecx
1:  lock incl counter
    loop 1b
1:  pause
    cmp done, %ebx
    jne 1b
    mov $msg_cnt, %esi
    call puts
    mov counter, %eax
    call print_hex
    mov ncpus, %eax
    imul $N, %eax
    call print_hex
    call newline

    movl $0x000C0040, LAPIC + 0x300     /* fixed IPI 0x40 to all excluding self */
1:  pause
    cmp ipis, %ebx
    jne 1b
    mov $msg_ipi, %esi
    call puts
    mov ipis, %eax
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
    mov LAPIC + 0x20, %eax
    shr $24, %eax
    mov %eax, %esp                      /* a 256-byte stack per APIC ID below 0x80000 */
    shl $8, %esp
    neg %esp
    add $0x80000, %esp
    lidt idt_desc
    movl $0x1FF, LAPIC + 0xF0
    lock bts %eax, bitmap
    lock incl arrived
1:  pause
    cmpl $0, go
    je 1b
    mov $N, %ecx
1:  lock incl counter
    loop 1b
    lock incl done
    sti
1:  hlt
    jmp 1b

ipi_handler:
    lock incl ipis
    movl $0, LAPIC + 0xB0               /* EOI */
    iret

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
arrived: .long 0
    .p2align 6
go: .long 0
    .p2align 6
done: .long 0
    .p2align 6
counter: .long 0
    .p2align 6
ipis: .long 0
    .p2align 6
bitmap: .space 32                       /* one bit per APIC ID */
ncpus: .long 0
hexdig: .ascii "0123456789abcdef"
msg_n: .asciz "cpus (cpuid 0xb, cpuid 1) "
msg_up: .asciz "aps up, distinct ids, bsp bit "
msg_cnt: .asciz "counter, expected "
msg_ipi: .asciz "broadcast ipis taken "
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
idt_desc:
    .word 0x41 * 8 - 1
    .long idt

    .bss
    .p2align 3
idt: .space 0x41 * 8
