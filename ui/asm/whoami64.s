# Which CPU am I? Load this on several CPUs (each gets its own slot, see
# "at"), then press Run. Every CPU prints its APIC ID. They share the
# console, so a spinlock in shared memory keeps their lines apart.
CONSOLE = 0x1800000         # shared spinlock: 0 = free

    .globl _start
_start:
    mov $1, %eax
    cpuid
    shr $24, %ebx           # CPUID.1:EBX[31:24] = initial APIC ID
    call lock_console
    lea msg(%rip), %rsi
    call puts
    mov %rbx, %rax
    call print_dec
    mov $'\n', %al
    out %al, $0xE9
    call unlock_console
    cli
    hlt

lock_console:
    mov $1, %eax
1:  xchg %eax, CONSOLE      # XCHG with memory is always atomic
    test %eax, %eax
    jz 3f
2:  pause                   # spin on a plain read until it looks free
    cmpl $0, CONSOLE
    jne 2b
    mov $1, %eax
    jmp 1b
3:  ret

unlock_console:
    movl $0, CONSOLE
    ret

puts:                       # rsi = string
1:  lodsb
    test %al, %al
    jz 2f
    out %al, $0xE9
    jmp 1b
2:  ret

print_dec:                  # rax = unsigned number
    push %rcx
    push %rdx
    mov $10, %ecx
    push $-1                # end marker
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

msg: .asciz "Hello from the CPU with APIC ID "
