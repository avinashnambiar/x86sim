/* Two-CPU test in 32-bit protected mode (run with --smp 2): the bootstrap
 * processor starts the application processor with INIT + STARTUP IPIs
 * (trampoline at 0x70000), both CPUs update shared counters (LOCK INC, a
 * spinlocked read-modify-write and an unprotected one that loses updates),
 * then they exchange fixed IPIs. With CACHE_TEST (smpcache.S, --cache) the
 * BSP also drives the MESI protocol through every transition on one line
 * and reads the line states back through the simulator's port 0xEC. */

#define CODE32 0x08
#define DATA0 0x10
#define LAPIC 0xFEE00000
#define N 2000
#define XLINE 0x200000                  /* the line the MESI test plays with */

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

/* the AP starts here in real mode, through the trampoline at 0x70000 */
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
    xor %ecx, %ecx                      /* IDT: exceptions, then the two IPI vectors */
1:  mov $exc_stub, %eax
    mov %ecx, %edx
    shl $4, %edx
    add %edx, %eax                      /* 16-byte stubs */
    call set_gate
    inc %ecx
    cmp $32, %ecx
    jne 1b
    mov $0x40, %ecx
    mov $ipi_to_ap, %eax
    call set_gate
    mov $0x41, %ecx
    mov $ipi_to_bsp, %eax
    call set_gate
    lidt idt_desc
    movl $0x1FF, LAPIC + 0xF0           /* software-enable the local APIC */

    mov $msg_bsp, %esi
    call puts
    mov LAPIC + 0x20, %eax              /* APIC ID */
    shr $24, %eax
    call print_hex
    call newline

    movb $0xEA, 0x70000                 /* trampoline: ljmp $0, $ap_entry16 */
    mov $ap_entry16, %eax
    mov %ax, 0x70001
    movw $0, 0x70003
    movl $0x01000000, LAPIC + 0x310     /* to APIC ID 1 */
    movl $0x00004500, LAPIC + 0x300     /* INIT, assert */
    movl $0x00004670, LAPIC + 0x300     /* STARTUP, vector 0x70 */
1:  pause
    cmpl $0, ap_ready
    je 1b
    mov $msg_ap, %esi
    call puts
    mov ap_id, %eax
    call print_hex
    call newline

    movl $1, go                         /* both CPUs update the counters */
    call work
1:  pause
    cmpl $2, done
    jne 1b
    mov $msg_cnt, %esi
    call puts
    mov counter1, %eax
    call print_hex
    mov counter2, %eax
    call print_hex
    call newline
    mov $msg_race, %esi
    call puts
    mov $msg_yes, %esi
    cmpl $2 * N, counter3
    jb 1f
    mov $msg_no, %esi
1:  call puts

    sti                                 /* IPI ping-pong */
    movl $0x01000000, LAPIC + 0x310
    movl $0x00000040, LAPIC + 0x300     /* fixed, vector 0x40, to the AP */
1:  hlt
    cmpl $0, ipi_back
    je 1b
    cli
    mov $msg_ipi, %esi
    call puts
    mov ipi_seen, %eax
    call print_hex
    mov ipi_back, %eax
    call print_hex
    call newline

#ifdef CACHE_TEST
    call mesi_test
#endif

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
    lidt idt_desc
    movl $0x1FF, LAPIC + 0xF0
    mov LAPIC + 0x20, %eax
    shr $24, %eax
    mov %eax, ap_id
    lock incl ap_ready
1:  pause
    cmpl $0, go
    je 1b
    call work
    sti
1:  hlt                                 /* wait for the BSP's IPI */
    cmpl $0, ipi_seen
    je 1b
    movl $0, LAPIC + 0x310
    movl $0x00000041, LAPIC + 0x300     /* answer: vector 0x41 to APIC ID 0 */
    cli
#ifdef CACHE_TEST
    jmp ap_server
#endif
1:  hlt
    jmp 1b

/* each CPU: N atomic increments, N under a spinlock, N racy ones */
work:
    mov $N, %ecx
1:  lock incl counter1
    call spin_lock
    mov counter2, %eax
    inc %eax
    mov %eax, counter2
    movl $0, lockv                      /* unlock */
    loop 1b
    mov $N, %ecx
1:  mov counter3, %eax                  /* unprotected read-modify-write */
    inc %eax
    mov %eax, counter3
    loop 1b
    lock incl done
    ret

spin_lock:
1:  mov $1, %eax
    xchg %eax, lockv
    test %eax, %eax
    jz 3f
2:  pause
    cmpl $0, lockv
    jne 2b
    jmp 1b
3:  ret

ipi_to_ap:
    push %eax
    mov LAPIC + 0x20, %eax
    shr $24, %eax
    inc %eax                            /* records APIC ID + 1 of the receiver */
    mov %eax, ipi_seen
    movl $0, LAPIC + 0xB0               /* EOI */
    pop %eax
    iret
ipi_to_bsp:
    push %eax
    mov LAPIC + 0x20, %eax
    shr $24, %eax
    inc %eax
    mov %eax, ipi_back
    movl $0, LAPIC + 0xB0
    pop %eax
    iret

#ifdef CACHE_TEST
/* The AP executes commands for the MESI test: 1 = read XLINE, 2 = write it. */
ap_server:
1:  mov cmd, %eax
    test %eax, %eax
    jz 1b
    cmp $1, %eax
    jne 2f
    mov XLINE, %edx
    jmp 3f
2:  movl $0xA9A9A9A9, XLINE
3:  movl $0, cmd
    jmp 1b

ap_do:                                  /* eax = command; returns when the AP is done */
    mov %eax, cmd
1:  cmpl $0, cmd
    jne 1b
    ret

/* print the states of XLINE: L1D/L1I of cpu0, cpu1, then the L2 */
states:
    push %eax
    mov $XLINE, %eax
    out %eax, $0xEC
    in $0xEC, %eax
    call print_hex
    pop %eax
    ret

mesi_test:
    mov $msg_mesi, %esi
    call puts
    call newline
    mov $1, %eax                        /* AP reads: E in its L1D, clean in the L2 */
    call ap_do
    call states
    mov XLINE, %eax                     /* BSP reads: both S */
    call states
    movl $0x12345678, XLINE             /* BSP writes: BusUpgr, BSP M, AP I */
    call states
    mov $1, %eax                        /* AP reads: BSP flushes (intervention), both S, L2 dirty */
    call ap_do
    call states
    call newline
    mov $2, %eax                        /* AP writes: BusUpgr, AP M, BSP I */
    call ap_do
    call states
    mov XLINE, %eax                     /* BSP reads AP's data from the AP's M copy */
    call print_hex
    call states
    clflush XLINE                       /* written back and dropped everywhere */
    call states
    mov XLINE, %eax                     /* refill from memory: the data survived */
    call print_hex
    call states
    wbinvd
    call states
    call newline
    ret
#endif

exc_common:
    mov $msg_exc, %esi
    call puts
    pop %eax
    call print_hex
    call newline
    mov $1, %al
    out %al, $0xF4
    hlt
    .p2align 4
exc_stub:
    .irp v, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    .p2align 4
    push $\v
    jmp exc_common
    .endr

/* ecx = vector, eax = handler: 32-bit interrupt gate */
set_gate:
    mov %ax, idt(,%ecx,8)
    movw $CODE32, idt + 2(,%ecx,8)
    movw $0x8E00, idt + 4(,%ecx,8)
    shr $16, %eax
    mov %ax, idt + 6(,%ecx,8)
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

    .data
    .p2align 6                          /* shared variables on separate lines */
ap_ready: .long 0
    .p2align 6
go: .long 0
    .p2align 6
done: .long 0
    .p2align 6
counter1: .long 0
    .p2align 6
counter2: .long 0
    .p2align 6
counter3: .long 0
    .p2align 6
lockv: .long 0
    .p2align 6
cmd: .long 0
    .p2align 6
ap_id: .long 0
ipi_seen: .long 0
ipi_back: .long 0
hexdig: .ascii "0123456789abcdef"
msg_bsp: .asciz "bsp apic id "
msg_ap: .asciz "ap started, apic id "
msg_cnt: .asciz "counters (lock inc, spinlock) "
msg_race: .asciz "unprotected counter lost updates: "
msg_yes: .asciz "yes\n"
msg_no: .asciz "no\n"
msg_ipi: .asciz "ipis received (apic id + 1) "
msg_mesi: .asciz "mesi states of the test line (cpu0 L1D/L1I in bits 0-3, cpu1 in 4-7, L2 in 16-17):"
msg_exc: .asciz "UNEXPECTED exception "
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
    .word 0x42 * 8 - 1
    .long idt

    .bss
    .p2align 3
idt: .space 0x42 * 8
