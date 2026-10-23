# Hello, long mode, in Intel syntax (the editor adds .intel_syntax noprefix):
# destination first, no % on registers, [brackets] for memory. Comments
# start with # (a ; separates two statements on one line).
    .globl _start
_start:
    lea rsi, [rip + msg]
next:
    lodsb
    test al, al
    jz done
    out 0xE9, al            # port 0xE9 prints a byte on the Console
    jmp next
done:
    cli
    hlt

msg: .asciz "Hello from 64-bit mode, Intel syntax!\n"
