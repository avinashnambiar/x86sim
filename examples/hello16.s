# 16-bit real-mode DOS program
    .intel_syntax noprefix
    .code16
    .globl _start
    .text
_start:
    mov ah, 0x09
    mov dx, offset msg
    int 0x21
    mov cx, 5
    mov al, '0'
1:  mov dl, al
    mov ah, 0x02
    int 0x21
    inc al
    loop 1b
    mov dl, 10
    mov ah, 0x02
    int 0x21
    mov ax, 0x4C03
    int 0x21

msg:    .ascii "Hello from 16-bit real mode!\r\n$"
