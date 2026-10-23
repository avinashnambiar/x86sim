# Shared counter. Load this on two or more CPUs, then press Run: each adds
# 100000 to one counter in shared memory with LOCK INC and prints what it
# saw when it finished. The counter keeps its value until New machine.
#
# Try it without "lock" (New machine first): updates get lost, because INC
# is a read, an add and a write that other CPUs can slip between. With
# --cache, open the Caches tab and watch the counter's line (0x1800040)
# bounce between the CPUs' L1Ds in state M.
CONSOLE = 0x1800000         # spinlock for the shared console
COUNTER = 0x1800040         # on its own 64-byte cache line

    .globl _start
_start:
    mov $100000, %ecx
1:  lock incl COUNTER
    loop 1b

    call lock_console
    lea msg1(%rip), %rsi
    call puts
    mov $1, %eax
    cpuid
    shr $24, %ebx
    mov %rbx, %rax
    call print_dec
    lea msg2(%rip), %rsi
    call puts
    mov COUNTER, %eax
    call print_dec
    mov $'\n', %al
    out %al, $0xE9
    call unlock_console
    cli
    hlt

lock_console:
    mov $1, %eax
1:  xchg %eax, CONSOLE
    test %eax, %eax
    jz 3f
2:  pause
    cmpl $0, CONSOLE
    jne 2b
    mov $1, %eax
    jmp 1b
3:  ret

unlock_console:
    movl $0, CONSOLE
    ret

puts:
1:  lodsb
    test %al, %al
    jz 2f
    out %al, $0xE9
    jmp 1b
2:  ret

print_dec:
    push %rcx
    push %rdx
    mov $10, %ecx
    push $-1
1:  xor %edx, %edx
    div %rcx
    push %rdx
    test %rax, %rax
    jnz 1b
2:  pop %rax
    cmp $-1, %rax
    je 3f
    add $'0', %al
    out %al, $0xE9
    jmp 2b
3:  pop %rdx
    pop %rcx
    ret

msg1: .asciz "APIC ID "
msg2: .asciz " finished, counter = "
