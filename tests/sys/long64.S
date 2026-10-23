/* Bare-metal test: real mode -> protected mode -> long mode with 4-level
 * paging (4 KiB, 2 MiB and 1 GiB pages), NX, a 64-bit IDT with an IST
 * stack for #DF, SYSCALL/SYSRET to ring 3 and back, SWAPGS, FSGSBASE,
 * SMEP, SMAP (STAC/CLAC), protection keys, PCID/INVPCID, INVLPG,
 * non-canonical addresses, compatibility mode and MSRs.
 * Output goes to port 0xE9; the exit code is written to port 0xF4. */

#define CODE32 0x08
#define DATA0 0x10
#define CODE64 0x18
#define KDATA 0x20
#define UCODE32 0x2B
#define UDATA 0x33
#define UCODE64 0x3B
#define TSS64 0x40

#define UALIAS 0x200000          /* user alias of physical 0-2 MiB (2 MiB page, U=1) */

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

    /* page tables */
    mov $pml4, %edi
    mov $(5 * 4096 / 4), %ecx
    xor %eax, %eax
    rep stosl
    mov $pdpt, %eax
    or $7, %eax
    mov %eax, pml4
    mov $pd, %eax
    or $7, %eax
    mov %eax, pdpt
    movl $0x00000083, pdpt + 8          /* 0x40000000: 1 GiB page -> 0, supervisor */
    mov $pt, %eax
    or $7, %eax
    mov %eax, pd                        /* 0-2 MiB: 4 KiB pages */
    movl $0x00000087, pd + 8            /* 2-4 MiB: 2 MiB page -> 0, user (alias) */
    movl $0x00400083, pd + 16           /* 4-6 MiB: 2 MiB page, NX */
    movl $0x80000000, pd + 20
    mov $pt, %edi
    mov $0x003, %eax                    /* present, writable, supervisor */
    mov $512, %ecx
1:  mov %eax, (%edi)
    add $0x1000, %eax
    add $8, %edi
    loop 1b

    mov %cr4, %eax
    or $0xA0, %eax                      /* PAE | PGE */
    mov %eax, %cr4
    mov $pml4, %eax
    mov %eax, %cr3
    mov $0xC0000080, %ecx
    rdmsr
    or $0x901, %eax                     /* LME | NXE | SCE */
    wrmsr
    mov %cr0, %eax
    or $0x80010000, %eax                /* PG | WP */
    mov %eax, %cr0
    ljmp $CODE64, $lm64

    .code64
lm64:
    mov $KDATA, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    mov $0x90000, %rsp
    mov $msg_lm, %esi
    call puts
    mov $0xC0000080, %ecx
    rdmsr
    call print_hex
    call newline

    /* IDT: exception stubs; #DF on IST1 */
    xor %ecx, %ecx
1:  mov stub_table(,%rcx,8), %rax
    xor %edx, %edx
    cmp $8, %ecx
    jne 2f
    mov $1, %edx
2:  call set_gate
    inc %ecx
    cmp $32, %ecx
    jne 1b
    lidt idt_desc

    /* TSS: RSP0 and IST1 */
    movq $0x88000, tss + 4
    movq $0x84000, tss + 0x24
    mov $tss, %eax
    mov %ax, gdt + TSS64 + 2
    shr $16, %eax
    mov %al, gdt + TSS64 + 4
    mov %ah, gdt + TSS64 + 7
    mov $TSS64, %ax
    ltr %ax

    /* 1 GiB page alias */
    movl $0xCAFEF00D, 0x100000
    mov 0x40100000, %eax
    call print_hex
    call newline

    /* NX: executing from the NX 2 MiB page */
    movb $0xC3, 0x400000                /* ret */
    movl $14, expect
    lea 1f(%rip), %rax
    mov %rax, resume
    mov $0x400000, %eax
    call *%rax
1:  mov %cr2, %rax
    call print_hex
    call newline

    /* not present, and a non-canonical address */
    movl $14, expect
    lea 1f(%rip), %rax
    mov %rax, resume
    mov $0x80000000, %ebx
    mov (%rbx), %eax
1:  movl $13, expect
    lea 1f(%rip), %rax
    mov %rax, resume
    movabs $0x0000800000000000, %rbx
    mov (%rbx), %eax
1:
    /* INVLPG: remap 0x1F0000 from itself to 0x1F1000 */
    movl $0x1111, 0x1F0000
    movl $0x2222, 0x1F1000
    mov 0x1F0000, %eax
    movq $0x1F1003, pt + 0x1F0 * 8
    invlpg 0x1F0000
    mov 0x1F0000, %eax
    call print_hex
    movq $0x1F0003, pt + 0x1F0 * 8
    invlpg 0x1F0000
    call newline

    /* compatibility mode and back */
    push $CODE32
    push $compat
    lretq
compat_ret:
    mov $msg_back, %esi
    call puts

    /* MSRs for SYSCALL, SWAPGS and RDPID */
    mov $0xC0000081, %ecx               /* STAR: kernel CS 0x18, user base 0x28 */
    xor %eax, %eax
    mov $0x00280018, %edx
    wrmsr
    mov $0xC0000082, %ecx               /* LSTAR */
    mov $syscall_entry, %eax
    xor %edx, %edx
    wrmsr
    mov $0xC0000084, %ecx               /* SFMASK: IF, TF, DF, AC */
    mov $0x40700, %eax
    wrmsr
    mov $0xC0000101, %ecx               /* GS base (user) */
    mov $0x1111, %eax
    wrmsr
    mov $0xC0000102, %ecx               /* KERNEL_GS_BASE */
    mov $0x2222, %eax
    wrmsr
    mov $0xC0000103, %ecx               /* TSC_AUX */
    mov $7, %eax
    wrmsr
    rdpid %rax
    call print_hex
    call newline
    mov %cr4, %rax
    or $(1 << 16), %rax                 /* FSGSBASE */
    mov %rax, %cr4

    /* PCID and INVPCID */
    mov %cr4, %rax
    or $(1 << 17), %rax
    mov %rax, %cr4
    mov $pml4, %eax
    or $1, %eax
    bts $63, %rax                       /* PCID 1, no flush */
    mov %rax, %cr3
    mov $2, %eax
    invpcid invpcid_desc(%rip), %rax
    mov %cr3, %rax
    call print_hex
    call newline

    /* to ring 3: SYSRET with RCX = user entry (in the user alias), R11 = flags */
    lea user(%rip), %rcx
    add $UALIAS, %rcx
    mov $0x202, %r11
    mov $0x70000 + UALIAS, %rsp
    sysretq

/* user mode, running in the U=1 alias: RIP-relative code only */
    .p2align 4
user:
    mov $1, %eax
    syscall
    mov $13, %r14d                      /* #GP: privileged instruction */
    mov $3, %r15d
    mov %cr0, %rax
    mov $2, %eax
    syscall                             /* SMAP / SMEP tests in the kernel */
    mov $4, %eax
    syscall                             /* kernel sets PKRU.AD for key 1 */
    mov $14, %r14d                      /* user data read of a key-1 page */
    mov $4, %r15d
    mov (%rsp), %rax
    mov $5, %eax
    syscall                             /* PKRU = 0 again */
    mov (%rsp), %rax
    mov $3, %eax
    syscall
    jmp .

    .code32
compat:
    mov $msg_compat, %esi
1:  lodsb
    test %al, %al
    jz 2f
    out %al, $0xE9
    jmp 1b
2:  ljmp $CODE64, $compat_ret

    .code64
syscall_entry:
    swapgs
    rdgsbase %rbx
    swapgs
    mov %rsp, %r12
    mov $0x8C000, %rsp
    push %rcx
    push %r11
    cmp $1, %eax
    jne 1f
    mov $msg_sys, %esi
    call puts
    mov %rcx, %rax
    call print_hex
    mov %r11, %rax
    call print_hex
    mov %rbx, %rax
    call print_hex
    call newline
    jmp sys_ret
1:  cmp $2, %eax
    jne 1f
    mov %cr4, %rax
    or $(3 << 20), %rax                 /* SMEP | SMAP */
    mov %rax, %cr4
    movl $14, expect
    lea 2f(%rip), %rax
    mov %rax, resume
    mov UALIAS + 0x1000, %eax           /* SMAP: supervisor read of a user page */
2:  stac
    mov UALIAS + 0x1000, %eax           /* allowed with AC=1 */
    clac
    movl $14, expect
    lea 2f(%rip), %rax
    mov %rax, resume
    lea ret_insn(%rip), %rax
    add $UALIAS, %rax
    call *%rax                          /* SMEP: supervisor fetch from a user page */
2:  add $8, %rsp                        /* the faulting call left its return address */
    mov %cr4, %rax
    and $~(3 << 20), %rax
    mov %rax, %cr4
    mov $msg_smap, %esi
    call puts
    jmp sys_ret
1:  cmp $4, %eax
    jne 1f
    mov %cr4, %rax
    or $(1 << 22), %rax                 /* PKE */
    mov %rax, %cr4
    movabs $(0x87 | (1 << 59)), %rax    /* the user alias gets protection key 1 */
    mov %rax, pd + 8
    invlpg UALIAS
    mov $0x70000 + UALIAS - 8, %rax
    invlpg (%rax)
    xor %ecx, %ecx
    xor %edx, %edx
    mov $4, %eax                        /* AD for key 1 */
    wrpkru
    jmp sys_ret
1:  cmp $5, %eax
    jne 1f
    xor %ecx, %ecx
    xor %edx, %edx
    xor %eax, %eax
    wrpkru
    rdpkru
    call print_hex
    call newline
    jmp sys_ret
1:  mov $msg_bye, %esi                  /* 3: exit */
    call puts
    xor %eax, %eax
    out %al, $0xF4
    hlt
sys_ret:
    pop %r11
    pop %rcx
    mov %r12, %rsp
    sysretq
ret_insn:
    ret

/* exceptions: frame = vector, error, rip, cs, rflags, rsp, ss */
exc_common:
    push %rax
    push %rcx
    push %rdx
    push %rsi
    push %rdi
    mov $msg_exc, %esi
    call puts
    mov 40(%rsp), %rax
    call print_hex
    mov 48(%rsp), %rax
    call print_hex
    call newline
    mov 40(%rsp), %rax
    testb $3, 64(%rsp)
    jnz 1f
    cmp expect, %eax
    jne unexpected
    movl $-1, expect
    mov resume, %rax
    mov %rax, 56(%rsp)
    jmp 2f
1:  cmp %r14d, %eax                     /* user fault: r14 = vector, r15 = length */
    jne unexpected
    add %r15, 56(%rsp)
2:  pop %rdi
    pop %rsi
    pop %rdx
    pop %rcx
    pop %rax
    add $16, %rsp
    iretq
unexpected:
    mov $msg_unexp, %esi
    call puts
    mov $1, %al
    out %al, $0xF4
    hlt

/* ecx = vector, rax = handler, edx = IST index */
set_gate:
    mov %ecx, %edi
    shl $4, %edi
    add $idt, %edi
    mov %ax, (%rdi)
    movw $CODE64, 2(%rdi)
    mov %dl, 4(%rdi)
    movb $0x8E, 5(%rdi)
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
    push %rbx
    mov %rax, %rbx
    mov $16, %ecx
1:  rol $4, %rbx
    mov %ebx, %eax
    and $15, %eax
    mov hexdig(%rax), %al
    out %al, $0xE9
    loop 1b
    mov $' ', %al
    out %al, $0xE9
    pop %rbx
    pop %rax
    pop %rcx
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
    .long stub0, 0
    .long stub1, 0
    .long stub2, 0
    .long stub3, 0
    .long stub4, 0
    .long stub5, 0
    .long stub6, 0
    .long stub7, 0
    .long stub8, 0
    .long stub9, 0
    .long stub10, 0
    .long stub11, 0
    .long stub12, 0
    .long stub13, 0
    .long stub14, 0
    .long stub15, 0
    .long stub16, 0
    .long stub17, 0
    .long stub18, 0
    .long stub19, 0
    .long stub20, 0
    .long stub21, 0
    .long stub22, 0
    .long stub23, 0
    .long stub24, 0
    .long stub25, 0
    .long stub26, 0
    .long stub27, 0
    .long stub28, 0
    .long stub29, 0
    .long stub30, 0
    .long stub31, 0
expect: .long -1
    .p2align 3
resume: .quad 0
invpcid_desc: .quad 0, 0
hexdig: .ascii "0123456789abcdef"
msg_lm: .asciz "long mode, EFER "
msg_compat: .asciz "compatibility mode\n"
msg_back: .asciz "back in 64-bit mode\n"
msg_exc: .asciz "exception "
msg_sys: .asciz "syscall "
msg_smap: .asciz "smap/smep ok\n"
msg_bye: .asciz "bye\n"
msg_unexp: .asciz "UNEXPECTED\n"

    .p2align 3
gdt:
    .quad 0
    .quad 0x00CF9A000000FFFF    /* 0x08 code32 ring 0 */
    .quad 0x00CF92000000FFFF    /* 0x10 data ring 0 */
    .quad 0x00AF9A000000FFFF    /* 0x18 code64 ring 0 */
    .quad 0x00CF92000000FFFF    /* 0x20 data ring 0 */
    .quad 0x00CFFA000000FFFF    /* 0x28 code32 ring 3 */
    .quad 0x00CFF2000000FFFF    /* 0x30 data ring 3 */
    .quad 0x00AFFA000000FFFF    /* 0x38 code64 ring 3 */
    .quad 0x0000890000000067    /* 0x40 TSS64 (base filled in) */
    .quad 0
gdt_end:
gdt_desc:
    .word gdt_end - gdt - 1
    .long gdt, 0
idt_desc:
    .word 32 * 16 - 1
    .long idt, 0

    .bss
    .p2align 12
pml4: .space 4096
pdpt: .space 4096
pd: .space 4096
pt: .space 4096
spare: .space 4096
idt: .space 32 * 16
tss: .space 0x68
