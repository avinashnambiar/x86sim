# Hello, long mode. 64-bit code starts with flat segments, paging that
# identity-maps the first 4 GiB, interrupts off and the stack at the top of
# this CPU's slot. Writing a byte to port 0xE9 prints it on the Console.
    .globl _start
_start:
    lea msg(%rip), %rsi     # RIP-relative: works wherever it is loaded
1:  lodsb                   # al = [rsi++]
    test %al, %al
    jz 2f
    out %al, $0xE9
    jmp 1b
2:  cli
    hlt                     # this CPU stops here

msg: .asciz "Hello from 64-bit mode!\n"
