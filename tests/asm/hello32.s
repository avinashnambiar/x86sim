# 32-bit Linux hello world (int 0x80)
    .intel_syntax noprefix
    .globl _start
    .text
_start:
    mov eax, 4              # write
    mov ebx, 1
    mov ecx, offset msg
    mov edx, offset msg_len
    int 0x80
    mov eax, 1              # exit
    mov ebx, 7
    int 0x80

    .section .rodata
msg:    .ascii "Hello from i386!\n"
    .set msg_len, . - msg
