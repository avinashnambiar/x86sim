/* Bare-metal test: real mode -> 32-bit protected mode, exceptions through
 * the IDT, paging (4 KiB and 4 MiB pages), ring 3 via IRET with a TSS stack
 * switch, interrupt gates with DPL checks, the I/O permission bitmap, a call
 * gate and a far return to the outer level. Output goes to port 0xE9; the
 * exit code is written to port 0xF4. Run: x86sim --pc pm32.o */

#define CODE0 0x08
#define DATA0 0x10
#define CODE3 0x1B
#define DATA3 0x23
#define TSSSEL 0x28
#define NPSEL 0x30
#define GATE3 0x3B

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
    ljmpl $CODE0, $pm_entry

    .code32
pm_entry:
    mov $DATA0, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    mov %ax, %fs
    mov %ax, %gs
    mov $0x90000, %esp
    mov $msg_hello, %esi
    call puts

    /* IDT: 0..31 exception stubs, 0x40 (DPL0), 0x80 (DPL3), 0x81 (DPL0) */
    xor %ecx, %ecx
1:  mov stub_table(,%ecx,4), %eax
    mov $0x8E00, %edx
    call set_gate
    inc %ecx
    cmp $32, %ecx
    jne 1b
    mov $0x40, %ecx
    mov $int40, %eax
    mov $0x8E00, %edx
    call set_gate
    mov $0x80, %ecx
    mov $int80, %eax
    mov $0xEE00, %edx
    call set_gate
    mov $0x81, %ecx
    mov $int40, %eax
    mov $0x8E00, %edx
    call set_gate
    lidt idt_desc

    /* faults with error codes */
    movl $13, expect
    movl $1f, resume
    mov $0x1234, %ax
    mov %ax, %ds
1:  movl $11, expect
    movl $1f, resume
    mov $NPSEL, %ax
    mov %ax, %es
1:  movl $6, expect
    movl $1f, resume
    ud2
1:  movl $0, expect
    movl $1f, resume
    xor %edx, %edx
    xor %ecx, %ecx
    div %ecx
1:  int $0x40

    /* paging: identity 0-4 MiB (4 KiB pages), 0x40000000 -> 0x200000 (4 KiB),
     * 0x80000000 -> 0x400000 (4 MiB page), page 0x300000 read-only, page
     * 0x301000 supervisor-only */
    mov $page_dir, %edi
    mov $1024, %ecx
    xor %eax, %eax
    rep stosl
    mov $page_tab, %edi
    mov $0x007, %eax
    mov $1024, %ecx
2:  stosl
    add $0x1000, %eax
    loop 2b
    movl $0x300005, page_tab + 0x300 * 4   /* present, user, read-only */
    movl $0x301003, page_tab + 0x301 * 4   /* present, writable, supervisor */
    mov $page_tab, %eax
    or $7, %eax
    mov %eax, page_dir
    mov $page_tab2, %eax
    or $7, %eax
    mov %eax, page_dir + (0x40000000 >> 22) * 4
    movl $0x200007, page_tab2
    movl $0x400087, page_dir + (0x80000000 >> 22) * 4
    movl $0x400087, page_dir + 4  /* identity 4-8 MiB as a 4 MiB page */
    mov %cr4, %eax
    or $0x10, %eax              /* PSE */
    mov %eax, %cr4
    mov $page_dir, %eax
    mov %eax, %cr3
    mov %cr0, %eax
    or $0x80010000, %eax        /* PG | WP */
    mov %eax, %cr0
    movl $0x11223344, 0x40000000
    movl $0x55667788, 0x80000010
    mov 0x200000, %eax
    call print_hex
    mov 0x400010, %eax
    call print_hex
    mov $msg_paging, %esi
    call puts
    movl $14, expect
    movl $1f, resume
    movl $1, 0x50000000         /* not present: error 2 */
1:  mov %cr2, %eax
    call print_hex
    movl $14, expect
    movl $1f, resume
    movl $1, 0x300000           /* read-only with CR0.WP: error 3 */
1:  mov %cr2, %eax
    call print_hex
    mov $msg_nl, %esi
    call puts

    /* TSS: ring-0 stack, I/O bitmap allowing port 0x80 only */
    movl $0x88000, tss + 4
    movl $DATA0, tss + 8
    movw $(iomap - tss), tss + 0x66
    mov $iomap, %edi
    mov $0xFFFFFFFF, %eax
    mov $(8192 / 4 + 1), %ecx
    rep stosl
    andb $0xFE, iomap + 0x80 / 8
    mov $tss, %eax              /* TSS descriptor base, limit */
    mov %ax, gdt + TSSSEL + 2
    shr $16, %eax
    mov %al, gdt + TSSSEL + 4
    mov %ah, gdt + TSSSEL + 7
    movw $(iomap - tss + 8192), gdt + TSSSEL
    mov $call_gate_target, %eax /* call gate offset */
    mov %ax, gdt + (GATE3 & ~3)
    shr $16, %eax
    mov %ax, gdt + (GATE3 & ~3) + 6
    mov $TSSSEL, %ax
    ltr %ax

    /* to ring 3 */
    push $DATA3
    push $0x70000
    pushf
    orl $0x200, (%esp)          /* IF=1, IOPL=0 */
    push $CODE3
    push $user
    iret

    .p2align 4
user:
    mov $DATA3, %ax
    mov %ax, %ds
    mov %ax, %es
    mov $1, %eax
    int $0x80                   /* print CS/SS seen by the kernel */
    movl $13, %ds:expect_u
    cli                         /* #GP: CPL 3 > IOPL 0 */
    in $0x80, %al               /* allowed by the bitmap */
    movl $13, %ds:expect_u
    in $0x81, %al               /* #GP: denied by the bitmap */
    movl $14, %ds:expect_u
    mov 0x301000, %eax          /* #PF error 5: user read of a supervisor page */
    movl $14, %ds:expect_u
    movl $0, 0x300000           /* #PF error 7: user write to a read-only page */
    lcall $GATE3, $0            /* call gate to ring 0 */
    mov $2, %eax
    int $0x80
    movl $13, %ds:expect_u
    int $0x81                   /* #GP error 0x40a: gate DPL 0 */
    mov $3, %eax
    int $0x80                   /* exit */
    jmp .

/* user-mode faults: the handler skips the faulting instruction by length */
expect_u: .long 0

    .p2align 4
call_gate_target:
    mov $msg_gate, %esi
    call puts
    mov %cs, %eax
    call print_hex
    mov %ss, %eax
    call print_hex
    mov 12(%esp), %eax          /* caller's SS */
    call print_hex
    mov $msg_nl, %esi
    call puts
    lret

int40:
    push %esi
    mov $msg_int40, %esi
    call puts
    pop %esi
    iret

int80:
    push %eax
    push %esi
    cmp $3, %eax
    je do_exit
    mov $msg_sys, %esi
    call puts
    mov 4(%esp), %eax
    call print_hex
    mov 12(%esp), %eax          /* user CS */
    call print_hex
    mov 24(%esp), %eax          /* user SS */
    call print_hex
    mov %ss, %eax
    call print_hex
    mov $msg_nl, %esi
    call puts
    pop %esi
    pop %eax
    iret
do_exit:
    mov $msg_bye, %esi
    call puts
    mov $0, %al
    out %al, $0xF4
    hlt

/* common exception handler: frame = vector, error, eip, cs, eflags (ss, esp) */
exc_common:
    pusha
    mov $msg_exc, %esi
    call puts
    mov 32(%esp), %eax
    call print_hex
    mov 36(%esp), %eax
    call print_hex
    mov $msg_nl, %esi
    call puts
    mov 32(%esp), %eax
    testb $3, 44(%esp)
    jnz user_fault
    cmp expect, %eax
    jne unexpected
    movl $-1, expect
    mov resume, %eax
    mov %eax, 40(%esp)
    popa
    add $8, %esp
    iret
user_fault:
    cmp expect_u, %eax
    jne unexpected
    movl $-1, expect_u
    mov 40(%esp), %esi          /* skip the faulting instruction */
    movzbl (%esi), %eax
    mov $1, %ecx
    cmp $0xFA, %al              /* cli */
    je 3f
    mov $2, %ecx
    cmp $0xE4, %al              /* in al, imm8 */
    je 3f
    cmp $0xCD, %al              /* int imm8 */
    je 3f
    mov $5, %ecx
    cmp $0xA1, %al              /* mov eax, moffs32 */
    je 3f
    mov $10, %ecx               /* movl $imm32, moffs32 */
3:  add %ecx, 40(%esp)
    popa
    add $8, %esp
    iret
unexpected:
    mov $msg_unexp, %esi
    call puts
    mov $1, %al
    out %al, $0xF4
    hlt

/* ecx = vector, eax = handler, edx = type/DPL bits */
set_gate:
    lea idt(,%ecx,8), %edi
    mov %ax, (%edi)
    movw $CODE0, 2(%edi)
    shr $16, %eax
    mov %dx, 4(%edi)
    mov %ax, 6(%edi)
    ret

puts:
    push %eax
1:  lodsb
    test %al, %al
    jz 2f
    out %al, $0xE9
    jmp 1b
2:  pop %eax
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

#define STUB(n) stub##n: push $0; push $n; jmp exc_common
#define STUBE(n) stub##n: push $n; jmp exc_common
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
STUB(21)
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
    .p2align 2
stub_table:
    .long stub0, stub1, stub2, stub3, stub4, stub5, stub6, stub7
    .long stub8, stub9, stub10, stub11, stub12, stub13, stub14, stub15
    .long stub16, stub17, stub18, stub19, stub20, stub21, stub22, stub23
    .long stub24, stub25, stub26, stub27, stub28, stub29, stub30, stub31
expect: .long -1
resume: .long 0
hexdig: .ascii "0123456789abcdef"
msg_hello: .asciz "pm32: protected mode\n"
msg_exc: .asciz "exception "
msg_int40: .asciz "int 0x40\n"
msg_paging: .asciz "paging ok\n"
msg_sys: .asciz "syscall "
msg_gate: .asciz "call gate: "
msg_bye: .asciz "bye\n"
msg_unexp: .asciz "UNEXPECTED\n"
msg_nl: .asciz "\n"

    .p2align 3
gdt:
    .quad 0
    .quad 0x00CF9A000000FFFF    /* 0x08 code ring 0 */
    .quad 0x00CF92000000FFFF    /* 0x10 data ring 0 */
    .quad 0x00CFFA000000FFFF    /* 0x18 code ring 3 */
    .quad 0x00CFF2000000FFFF    /* 0x20 data ring 3 */
    .quad 0x0000890000000000    /* 0x28 TSS (base/limit filled in) */
    .quad 0x00CF12000000FFFF    /* 0x30 data, not present */
    .quad 0x0000EC0000080000    /* 0x38 call gate DPL 3 -> CODE0 (offset filled in) */
gdt_end:
gdt_desc:
    .word gdt_end - gdt - 1
    .long gdt
idt_desc:
    .word 256 * 8 - 1
    .long idt

    .bss
    .p2align 12
page_dir: .space 4096
page_tab: .space 4096
page_tab2: .space 4096
idt: .space 256 * 8
tss: .space 0x68
iomap: .space 8192 + 4
iomap_end:
