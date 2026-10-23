# 64-bit Linux hello world
    .intel_syntax noprefix
    .globl _start
    .text
_start:
    mov eax, 1              # write
    mov edi, 1              # stdout
    lea rsi, [rip + msg]
    mov edx, offset msg_len
    syscall
    mov eax, 60             # exit
    mov edi, 42
    syscall

    .section .rodata
msg:    .ascii "Hello from x86-64!\n"
    .set msg_len, . - msg
