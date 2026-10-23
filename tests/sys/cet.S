/* Bare-metal CET test in 64-bit long mode: supervisor shadow stacks (busy
 * token, SETSSBSY/CLRSSBSY, CALL/RET through the shadow stack, #CP on a
 * corrupted return address, INCSSP, WRSS/WRUSS, page protection of shadow
 * stack pages, stack switching with RSTORSSP/SAVEPREVSSP) and indirect
 * branch tracking (#CP without ENDBR64, NOTRACK). The shadow stack is the
 * 2 MiB page at 4 MiB (read-only + dirty). */

#define CODE32 0x08
#define DATA0 0x10
#define CODE64 0x18
#define SSTOP 0x5FFFF8                  /* supervisor shadow stack token */

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
    ljmpl $CODE32, $pm32

    .code32
pm32:
    mov $DATA0, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    mov $0x90000, %esp
    mov $pml4, %eax
    or $3, %eax                         /* pml4 -> pdpt -> pd */
    mov $pdpt, %ebx
    or $3, %ebx
    mov %ebx, pml4
    mov $pd, %ebx
    or $3, %ebx
    mov %ebx, pdpt
    movl $0x00000083, pd                /* 0-2 MiB, writable */
    movl $0x00200083, pd + 8            /* 2-4 MiB, writable */
    movl $0x004000E1, pd + 16           /* 4-6 MiB: shadow stack (P, PS, A, D; not writable) */
    mov %cr4, %eax
    or $0x20, %eax                      /* PAE */
    mov %eax, %cr4
    mov $pml4, %eax
    mov %eax, %cr3
    mov $0xC0000080, %ecx
    rdmsr
    or $0x100, %eax                     /* LME */
    wrmsr
    mov %cr0, %eax
    or $0x80010000, %eax                /* PG | WP (CET requires WP) */
    mov %eax, %cr0
    ljmp $CODE64, $lm64

    .code64
lm64:
    mov $DATA0, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    mov $0x90000, %rsp
    xor %ecx, %ecx
1:  mov stub_table(,%rcx,8), %rax
    call set_gate
    inc %ecx
    cmp $32, %ecx
    jne 1b
    lidt idt_desc

    mov %cr4, %rax                      /* CET on: shadow stacks, WRSS, IBT, NOTRACK */
    or $(1 << 23), %rax
    mov %rax, %cr4
    mov $0x6A2, %ecx
    mov $0x17, %eax
    xor %edx, %edx
    wrmsr

    mov $SSTOP, %rbx                    /* supervisor token: its own address, not busy */
    mov %rbx, %rax
    wrssq %rax, (%rbx)
    mov $0x6A4, %ecx                    /* IA32_PL0_SSP */
    mov $SSTOP, %eax
    xor %edx, %edx
    wrmsr
    setssbsy
    rdsspq %rax
    call print_hex
    call f1                             /* SSP inside the callee */
    rdsspq %rax
    call print_hex
    call newline

    movl $14, expect                    /* ordinary write to a shadow-stack page */
    lea 1f(%rip), %rax
    mov %rax, resume
    movq $1, 0x5FF000
1:  movl $21, expect                    /* corrupted return address */
    lea 2f(%rip), %rax
    mov %rax, resume
    call f2
    nop                                 /* the real return address */
2:  add $8, %rsp                        /* RET faulted: drop its return address and pop the shadow copy */
    mov $1, %eax
    incsspq %rax
    rdsspq %rax
    call print_hex
    call newline

    movl $21, expect                    /* IBT: indirect call to a target without ENDBR64 */
    lea 3f(%rip), %rax
    mov %rax, resume
    lea no_endbr(%rip), %rax
    call *%rax
3:  add $8, %rsp
    mov $1, %eax
    incsspq %rax
    lea with_endbr(%rip), %rax
    call *%rax
    lea no_endbr2(%rip), %rax
    notrack call *%rax

    /* switch to a second shadow stack (top 0x5FF000) and back */
    mov $0x5FEFF8, %rbx
    mov $0x5FF001, %rax                 /* restore token: top address | 64-bit */
    wrssq %rax, (%rbx)
    rstorssp (%rbx)
    rdsspq %rax
    call print_hex
    saveprevssp
    rdsspq %rax
    call print_hex
    mov $(SSTOP - 8), %rbx              /* the restore token SAVEPREVSSP left behind */
    rstorssp (%rbx)
    rdsspq %rax
    call print_hex
    saveprevssp
    rdsspq %rax
    call print_hex
    call newline

    movl $14, expect                    /* WRUSS to a supervisor shadow stack: #PF */
    lea 4f(%rip), %rax
    mov %rax, resume
    mov $0x5FF100, %rbx
    wrussq %rax, (%rbx)
4:  mov $SSTOP, %rbx
    clrssbsy (%rbx)                     /* SSP = 0: no calls until SETSSBSY */
    setc %r12b
    rdsspq %r13
    setssbsy                            /* succeeds only if the token was freed */
    movzbl %r12b, %eax
    call print_hex
    mov %r13, %rax
    call print_hex
    rdsspq %rax
    call print_hex
    call newline

    mov $0x6A2, %ecx
    xor %eax, %eax
    xor %edx, %edx
    wrmsr
    mov %cr4, %rax
    and $~(1 << 23), %rax
    mov %rax, %cr4
    mov $msg_bye, %esi
    call puts
    xor %al, %al
    out %al, $0xF4
    hlt

f1:
    rdsspq %rax
    call print_hex
    ret
f2:
    lea 2b(%rip), %rax                  /* overwrite the return address (skipping the nop) */
    mov %rax, (%rsp)
    ret
no_endbr:
    ret
with_endbr:
    endbr64
    mov $msg_endbr, %esi
    call puts
    ret
no_endbr2:
    mov $msg_notrack, %esi
    call puts
    ret

/* frame: vector, error, rip, cs, rflags, rsp, ss */
exc_common:
    push %rax
    push %rsi
    mov $msg_exc, %esi
    call puts
    mov 16(%rsp), %rax
    call print_hex
    mov 24(%rsp), %rax
    call print_hex
    call newline
    mov 16(%rsp), %rax
    cmp expect, %eax
    jne unexpected
    movl $-1, expect
    mov resume, %rax
    mov %rax, 32(%rsp)
    pop %rsi
    pop %rax
    add $16, %rsp
    iretq
unexpected:
    mov $msg_unexp, %esi
    call puts
    mov $1, %al
    out %al, $0xF4
    hlt

/* ecx = vector, rax = handler */
set_gate:
    mov %ecx, %edi
    shl $4, %edi
    add $idt, %edi
    mov %ax, (%rdi)
    movw $CODE64, 2(%rdi)
    movw $0x8E00, 4(%rdi)
    shr $16, %rax
    mov %ax, 6(%rdi)
    shr $16, %rax
    mov %eax, 8(%rdi)
    movl $0, 12(%rdi)
    ret

puts:
    push %rax
1:  lodsb
    test %al, %al
    jz 2f
    out %al, $0xE9
    jmp 1b
2:  pop %rax
    ret

newline:
    push %rax
    mov $'\n', %al
    out %al, $0xE9
    pop %rax
    ret

print_hex:
    push %rcx
    push %rax
    mov $8, %ecx
1:  rol $4, %eax
    push %rax
    and $15, %eax
    mov hexdig(%rax), %al
    out %al, $0xE9
    pop %rax
    loop 1b
    mov $' ', %al
    out %al, $0xE9
    pop %rax
    pop %rcx
    ret

/* interrupt delivery is an indirect transfer: every handler starts with ENDBR64 */
#define STUB(n) stub##n: endbr64; push $0; push $n; jmp exc_common
#define STUBE(n) stub##n: endbr64; push $n; jmp exc_common
STUB(0)
STUB(1)
STUB(2)
STUB(3)
STUB(4)
STUB(5)
STUB(6)
STUB(7)
STUBE(8)
STUB(9)
STUBE(10)
STUBE(11)
STUBE(12)
STUBE(13)
STUBE(14)
STUB(15)
STUB(16)
STUBE(17)
STUB(18)
STUB(19)
STUB(20)
STUBE(21)
STUB(22)
STUB(23)
STUB(24)
STUB(25)
STUB(26)
STUB(27)
STUB(28)
STUB(29)
STUB(30)
STUB(31)

    .data
    .p2align 3
stub_table:
    .long stub0, 0, stub1, 0, stub2, 0, stub3, 0, stub4, 0, stub5, 0, stub6, 0, stub7, 0
    .long stub8, 0, stub9, 0, stub10, 0, stub11, 0, stub12, 0, stub13, 0, stub14, 0, stub15, 0
    .long stub16, 0, stub17, 0, stub18, 0, stub19, 0, stub20, 0, stub21, 0, stub22, 0, stub23, 0
    .long stub24, 0, stub25, 0, stub26, 0, stub27, 0, stub28, 0, stub29, 0, stub30, 0, stub31, 0
expect: .long -1
resume: .quad 0
hexdig: .ascii "0123456789abcdef"
msg_endbr: .asciz "endbr target ok\n"
msg_notrack: .asciz "notrack ok\n"
msg_exc: .asciz "exception "
msg_bye: .asciz "bye\n"
msg_unexp: .asciz "UNEXPECTED\n"
    .p2align 3
gdt:
    .quad 0
    .quad 0x00CF9A000000FFFF            /* 0x08 code32 */
    .quad 0x00CF92000000FFFF            /* 0x10 data */
    .quad 0x00AF9A000000FFFF            /* 0x18 code64 */
gdt_end:
gdt_desc:
    .word gdt_end - gdt - 1
    .long gdt
idt_desc:
    .word 32 * 16 - 1
    .long idt, 0

    .bss
    .p2align 12
pml4: .space 4096
pdpt: .space 4096
pd: .space 4096
idt: .space 32 * 16
