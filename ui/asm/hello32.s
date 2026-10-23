# Hello, protected mode. 32-bit code starts with flat 4 GiB segments
# (CS = 0x08, data = 0x10), no paging, interrupts off and the stack at the
# top of this CPU's slot.
    .globl _start
_start:
    mov $msg, %esi          # absolute address: the program is linked where it is loaded
1:  lodsb
    test %al, %al
    jz 2f
    out %al, $0xE9          # port 0xE9 prints a byte on the Console
    jmp 1b
2:  cli
    hlt

msg: .asciz "Hello from 32-bit protected mode!\n"
