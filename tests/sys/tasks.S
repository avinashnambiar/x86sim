/* Bare-metal test: hardware task switches (JMP to a TSS, a task gate in the
 * IDT with nesting and IRET through the back link, busy bits), a double
 * fault delivered through a task gate (#GP whose IDT gate is not present ->
 * #NP -> #DF), and virtual-8086 mode (real-mode segment loads, INT through
 * the IDT with the VM86 frame, #GP to the monitor, leaving VM86 by an
 * interrupt). Output goes to port 0xE9; the exit code to port 0xF4. */

#define CODE0 0x08
#define DATA0 0x10
#define TSSA 0x28
#define TSSB 0x30
#define TSSC 0x38
#define TSSD 0x40

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

    xor %ecx, %ecx
1:  mov stub_table(,%ecx,4), %eax
    mov $0x8E00, %edx
    call set_gate
    inc %ecx
    cmp $32, %ecx
    jne 1b
    mov $0x30, %ecx
    mov $int30, %eax
    mov $0xEE00, %edx           /* DPL 3: reachable from VM86 with IOPL 3 */
    call set_gate
    mov $0x31, %ecx
    mov $int31, %eax
    mov $0xEE00, %edx
    call set_gate
    movw $TSSC, idt + 0x41 * 8 + 2 /* 0x41: task gate -> task C */
    movw $0x8500, idt + 0x41 * 8 + 4
    lidt idt_desc

    /* the four TSS descriptors and their initial states */
    mov $tss_a, %eax
    mov $TSSA, %edi
    call set_tss
    mov $tss_b, %eax
    mov $TSSB, %edi
    call set_tss
    mov $tss_c, %eax
    mov $TSSC, %edi
    call set_tss
    mov $tss_d, %eax
    mov $TSSD, %edi
    call set_tss
    movl $0x88000, tss_a + 4
    movl $DATA0, tss_a + 8
    movw $0x68, tss_a + 0x66    /* no I/O bitmap */
    mov $tss_b, %edi
    mov $task_b, %eax
    mov $0x80000, %edx
    call init_tss
    mov $tss_c, %edi
    mov $task_c, %eax
    mov $0x7C000, %edx
    call init_tss
    mov $tss_d, %edi
    mov $df_task, %eax
    mov $0x78000, %edx
    call init_tss
    mov $TSSA, %ax
    ltr %ax

    /* task switches */
    ljmp $TSSB, $0
    mov $msg_back_a, %esi
    call puts
    str %eax
    call print_hex
    movzbl gdt + TSSA + 5, %eax /* busy */
    call print_hex
    movzbl gdt + TSSB + 5, %eax /* available again after JMP away */
    call print_hex
    movzbl gdt + TSSC + 5, %eax /* available again after IRET */
    call print_hex
    call newline

    /* double fault through a task gate */
    movw $TSSD, idt + 8 * 8 + 2
    movw $0x8500, idt + 8 * 8 + 4
    andb $0x7F, idt + 13 * 8 + 5 /* #GP gate not present */
    movl $13, expect
    movl $1f, resume
    mov $0x1234, %ax
    mov %ax, %ds                /* #GP -> #NP -> #DF; the DF task repairs */
1:  mov $msg_df_done, %esi      /* the gate and the load faults again */
    call puts

    /* virtual-8086 mode: IOPL 3, CS = 0, SS:SP = 0:6000 */
    mov %esp, saved_esp
    push $0                     /* gs */
    push $0                     /* fs */
    push $0                     /* ds */
    push $0                     /* es */
    push $0                     /* ss */
    push $0x6000                /* sp */
    push $0x23002               /* VM | IOPL 3 */
    push $0                     /* cs */
    push $vm86_code
    iret
vm_done:
    mov $DATA0, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    mov saved_esp, %esp
    mov $msg_bye, %esi
    call puts
    xor %al, %al
    out %al, $0xF4
    hlt

task_b:
    mov $msg_task_b, %esi
    call puts
    str %eax
    call print_hex
    call newline
    int $0x41                   /* nested task C */
    mov $msg_back_b, %esi
    call puts
    pushf
    pop %eax
    and $0x4000, %eax           /* NT clear again in B */
    call print_hex
    call newline
    ljmp $TSSA, $0

task_c:
    mov $msg_task_c, %esi
    call puts
    str %eax
    call print_hex
    movzwl tss_c, %eax          /* back link */
    call print_hex
    pushf
    pop %eax
    and $0x4000, %eax           /* NT */
    call print_hex
    movzbl gdt + TSSB + 5, %eax /* B still busy while nested */
    call print_hex
    call newline
    iret

df_task:
    pop %ebx                    /* error code */
    mov $msg_df, %esi
    call puts
    mov %ebx, %eax
    call print_hex
    movzwl tss_d, %eax          /* back link: task A */
    call print_hex
    call newline
    orb $0x80, idt + 13 * 8 + 5
    iret

    .code16
vm86_code:
    mov $0x1234, %ax
    mov %ax, %ds                /* real-mode semantics */
    mov %ds:0, %cx              /* linear 0x12340 */
    int $0x30
    hlt                         /* #GP(0): CPL 3 */
    pushf
    pop %ax
    int $0x31
    .code32

int30:
    mov %ds, %ebx               /* data segments are null in the handler */
    mov $DATA0, %ax
    mov %ax, %ds
    mov $msg_vm86, %esi
    call puts
    mov %ebx, %eax
    call print_hex
    mov 24(%esp), %eax          /* VM86 DS from the frame */
    call print_hex
    mov 8(%esp), %eax           /* EFLAGS */
    call print_hex
    mov 4(%esp), %eax           /* CS */
    call print_hex
    call newline
    iret

int31:
    mov $DATA0, %ax
    mov %ax, %ds
    mov $msg_vm86_exit, %esi
    call puts
    movzwl 20(%esp), %eax       /* VM86 ES */
    call print_hex
    mov 8(%esp), %eax
    call print_hex
    call newline
    jmp vm_done

/* frame after the pushes below: regs 0-31, es 32, ds 36, vector 40,
 * error 44, eip 48, cs 52, eflags 56 */
exc_common:
    push %ds
    push %es
    pusha
    mov $DATA0, %ax
    mov %ax, %ds
    mov %ax, %es
    mov $msg_exc, %esi
    call puts
    mov 40(%esp), %eax
    call print_hex
    mov 44(%esp), %eax
    call print_hex
    call newline
    mov 40(%esp), %eax
    testl $0x20000, 56(%esp)
    jz 1f
    cmp $13, %eax               /* VM86 #GP: skip the one-byte HLT */
    jne unexpected
    incl 48(%esp)
    jmp 2f
1:  cmp expect, %eax
    jne unexpected
    movl $-1, expect
    mov resume, %eax
    mov %eax, 48(%esp)
2:  popa
    pop %es
    pop %ds
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

/* eax = TSS base, edi = selector: 32-bit TSS descriptor, limit 0x67 */
set_tss:
    movw $0x67, gdt(%edi)
    mov %ax, gdt + 2(%edi)
    shr $16, %eax
    mov %al, gdt + 4(%edi)
    movb $0x89, gdt + 5(%edi)
    movb $0, gdt + 6(%edi)
    mov %ah, gdt + 7(%edi)
    ret

/* edi = TSS, eax = eip, edx = esp: ring-0 flat task, interrupts off */
init_tss:
    mov %eax, 0x20(%edi)
    movl $2, 0x24(%edi)
    mov %edx, 0x38(%edi)
    movl $DATA0, 0x48(%edi)
    movl $CODE0, 0x4C(%edi)
    movl $DATA0, 0x50(%edi)
    movl $DATA0, 0x54(%edi)
    movl $DATA0, 0x58(%edi)
    movl $DATA0, 0x5C(%edi)
    movw $0x68, 0x66(%edi)
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
saved_esp: .long 0
hexdig: .ascii "0123456789abcdef"
msg_hello: .asciz "tasks: protected mode\n"
msg_task_b: .asciz "task b "
msg_task_c: .asciz "task c "
msg_back_b: .asciz "back in b "
msg_back_a: .asciz "back in a "
msg_df: .asciz "double fault task "
msg_df_done: .asciz "df recovered\n"
msg_vm86: .asciz "vm86 int "
msg_vm86_exit: .asciz "vm86 exit "
msg_exc: .asciz "exception "
msg_bye: .asciz "bye\n"
msg_unexp: .asciz "UNEXPECTED\n"

    .p2align 3
gdt:
    .quad 0
    .quad 0x00CF9A000000FFFF    /* 0x08 code ring 0 */
    .quad 0x00CF92000000FFFF    /* 0x10 data ring 0 */
    .quad 0x00CFFA000000FFFF    /* 0x18 code ring 3 */
    .quad 0x00CFF2000000FFFF    /* 0x20 data ring 3 */
    .quad 0, 0, 0, 0            /* 0x28-0x40 TSS A-D (filled in) */
gdt_end:
gdt_desc:
    .word gdt_end - gdt - 1
    .long gdt
idt_desc:
    .word 256 * 8 - 1
    .long idt

    .bss
    .p2align 12
idt: .space 256 * 8
tss_a: .space 0x68
tss_b: .space 0x68
tss_c: .space 0x68
tss_d: .space 0x68
