# Hello, real mode. 16-bit code runs in its own segment: CS = DS = ES = SS =
# load address / 16, so labels are offsets from the start of the program.
# The BIOS services work: int 0x10, AH = 0x0E prints a character on the
# Screen tab (and the Console). Port 0xE9 would print on the Console only.
    .globl _start
_start:
    mov $msg, %si
1:  lodsb
    test %al, %al
    jz 2f
    mov $0x0E, %ah          # BIOS teletype output
    int $0x10
    jmp 1b
2:  cli
    hlt

msg: .asciz "Hello from 16-bit real mode!\r\n"
