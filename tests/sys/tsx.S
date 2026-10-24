/* Restricted transactional memory (run with --smp 2 --cache): CPUID and
 * XTEST, a committed transaction, XABORT with register rollback, aborts by
 * CPUID, by an exception (not delivered: there is no IDT), and by capacity,
 * nesting, a conflict from the other CPU, isolation (the other CPU never sees
 * a value stored inside a transaction), and a lock-elision counter. */

#define CODE32 0x08
#define DATA0 0x10
#define LAPIC 0xFEE00000
#define N 2000
#define BIG 0x400000

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

    mov $msg_cpuid, %esi                /* CPUID.7.EBX bit 11, XTEST outside: ZF=1 */
    call puts
    mov $7, %eax
    xor %ecx, %ecx
    cpuid
    shr $11, %ebx
    and $1, %ebx
    mov %ebx, %eax
    call print_hex
    xtest
    setz %al
    movzx %al, %eax
    call print_hex
    call newline

    /* commit: the store is visible inside, XTEST says ZF=0, then committed */
    mov $msg_commit, %esi
    call puts
    movl $0, var_a
    xbegin 1f
    movl $5, var_a
    mov var_a, %edx
    xtest
    setnz %cl
    xend
    movzx %cl, %eax
    call print_hex
    mov %edx, %eax
    call print_hex
    mov var_a, %eax
    call print_hex
    call newline
    jmp 2f
1:  mov $msg_unexpected, %esi
    call puts
    call print_hex
    call newline
2:

    /* XABORT: status 0x42000001, store discarded, EBX rolled back */
    mov $msg_xabort, %esi
    call puts
    movl $1, var_a
    mov $7, %ebx
    xbegin 1f
    movl $9, var_a
    mov $0x99, %ebx
    xabort $0x42
1:  call print_hex
    mov var_a, %eax
    call print_hex
    mov %ebx, %eax
    call print_hex
    call newline

    /* CPUID cannot run in a transaction: status 0 */
    mov $msg_cpuidab, %esi
    call puts
    xbegin 1f
    movl $3, var_a
    cpuid
    xend
1:  call print_hex
    mov var_a, %eax
    call print_hex
    call newline

    /* divide by zero: aborts, the #DE is never delivered */
    mov $msg_exc, %esi
    call puts
    xbegin 1f
    xor %ecx, %ecx
    div %ecx
    xend
1:  call print_hex
    call newline

    /* nesting: inner XEND keeps the transaction open; abort inside the
     * inner level sets bit 5 and goes to the outer fallback */
    mov $msg_nest, %esi
    call puts
    xbegin 1f
    xbegin 3f
    xend
    xtest
    setnz %cl
    xend
    movzx %cl, %eax
    call print_hex
    xbegin 1f
    xbegin 3f
    xabort $1
1:  call print_hex
    call newline
    jmp 4f
3:  mov $msg_unexpected, %esi           /* the inner fallback is never used */
    call puts
    call newline
4:

    /* capacity: storing to 600 lines overflows the 512-line buffer */
    mov $msg_cap, %esi
    call puts
    xbegin 1f
    mov $BIG, %edi
    mov $600, %ecx
2:  movl $1, (%edi)
    add $64, %edi
    loop 2b
    xend
1:  call print_hex
    mov BIG, %eax                       /* nothing was written */
    call print_hex
    call newline

    /* start the AP */
    movb $0xEA, 0x70000
    mov $ap_entry16, %eax
    mov %ax, 0x70001
    movw $0, 0x70003
    movl $0x000C4500, LAPIC + 0x300
    movl $0x000C4670, LAPIC + 0x300
1:  pause
    cmpl $1, ap_phase
    jne 1b

    /* conflict: we read var_x in a transaction and wait; the AP writes it */
    mov $msg_conf, %esi
    call puts
    xbegin 1f
    mov var_x, %eax
    movl $1, go_conflict_dummy          /* (buffered: the AP cannot see it) */
2:  pause
    jmp 2b                              /* only the conflict gets us out */
1:  call print_hex
    call newline
    movl $2, ap_phase                   /* tell the AP we saw it */

    /* isolation: we store var_z = 0x77 transactionally and stall; the AP
     * keeps reading var_z and must never see 0x77; its read aborts us */
1:  pause
    cmpl $3, ap_phase
    jne 1b
    mov $msg_iso, %esi
    call puts
    xbegin 1f
    movl $0x77, var_z
    mov $100000, %ecx
2:  loop 2b
    xend
1:  movl $1, iso_done
1:  pause
    cmpl $4, ap_phase
    jne 1b
    call print_hex                      /* our status: conflict */
    mov ap_max_z, %eax
    call print_hex
    call newline

    /* lock elision: both CPUs add N to a counter, transactionally when
     * possible, otherwise under a spinlock the transaction checks */
    movl $1, go_elide
    call elide_loop
1:  pause
    cmpl $5, ap_phase
    jne 1b
    mov $msg_elide, %esi
    call puts
    mov counter, %eax
    call print_hex
    xor %eax, %eax
    cmpl $0, commits
    setne %al
    call print_hex
    call newline

    mov $msg_bye, %esi
    call puts
    xor %al, %al
    out %al, $0xF4
    hlt

/* N increments of counter: try a transaction (abort if the lock is held),
 * fall back to taking the lock */
elide_loop:
    mov $N, %esi
1:  xbegin 3f
    cmpl $0, lock
    je 2f
    xabort $0xFF
2:  incl counter
    xend
    lock incl commits
    jmp 5f
3:  lock btsl $0, lock                  /* fallback: spinlock */
    jnc 4f
    pause
    jmp 3b
4:  incl counter
    movl $0, lock
5:  dec %esi
    jnz 1b
    ret

ap32:
    mov $DATA0, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    mov $0x80000, %esp
    movl $1, ap_phase
    mov $200, %ecx                      /* let the BSP get into its transaction */
1:  loop 1b
    movl $1, var_x                      /* conflict */
1:  pause
    cmpl $2, ap_phase
    jne 1b
    movl $3, ap_phase
1:  mov var_z, %eax                     /* isolation: watch var_z */
    cmp ap_max_z, %eax
    jbe 2f
    mov %eax, ap_max_z
2:  cmpl $0, iso_done
    je 1b
    movl $4, ap_phase
1:  pause
    cmpl $0, go_elide
    je 1b
    call elide_loop
    movl $5, ap_phase
1:  hlt
    jmp 1b

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
var_a: .long 0
    .p2align 6
var_x: .long 0
    .p2align 6
var_z: .long 0
    .p2align 6
go_conflict_dummy: .long 0
    .p2align 6
ap_phase: .long 0
    .p2align 6
iso_done: .long 0
    .p2align 6
ap_max_z: .long 0
    .p2align 6
go_elide: .long 0
    .p2align 6
lock: .long 0
    .p2align 6
counter: .long 0
    .p2align 6
commits: .long 0
    .p2align 6
hexdig: .ascii "0123456789abcdef"
msg_cpuid: .asciz "rtm cpuid, xtest outside "
msg_commit: .asciz "commit: xtest inside, read inside, after "
msg_unexpected: .asciz "unexpected abort "
msg_xabort: .asciz "xabort: status, memory, ebx "
msg_cpuidab: .asciz "cpuid aborts: status, memory "
msg_exc: .asciz "#de aborts, not delivered: status "
msg_nest: .asciz "nested: xtest after inner xend, abort status "
msg_cap: .asciz "capacity: status, memory "
msg_conf: .asciz "conflict from the other cpu: status "
msg_iso: .asciz "isolation: status, max value the other cpu saw "
msg_elide: .asciz "lock elision: counter, some commits "
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
