/* Disk boot test (see diskboot.disk): the BIOS loads sector 0 at 0x7C00
 * from the hard disk; it reads the second stage with an INT 13h extended
 * read and jumps to it. The second stage checks drive parameters, BIOS
 * memory services, keyboard input from diskboot.in, the A20 gate (address
 * wrap-around with A20 off) and a timed wait. Addresses are written as
 * label - _start + 0x7C00 so the program needs no linker. */

#define A(x) ((x) - _start + 0x7C00)

    .code16
    .text
    .globl _start
_start:
    cli
    xor %ax, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    mov $0x7C00, %sp
    mov %dl, A(bootdrv)
    mov $A(msg_mbr), %si
    call tty
    mov $0x41, %ah              /* extended disk services present? */
    mov $0x55AA, %bx
    mov A(bootdrv), %dl
    int $0x13
    jc fail
    cmp $0xAA55, %bx
    jne fail
    mov $0x42, %ah              /* read sectors 1-4 to 0x7E00 */
    mov A(bootdrv), %dl
    mov $A(dap), %si
    int $0x13
    jc fail
    jmp stage2

fail:
    mov $A(msg_fail), %si
    call tty
    mov $1, %al
    out %al, $0xF4
    hlt

/* teletype output through the video BIOS */
tty:
    lodsb
    test %al, %al
    jz 1f
    mov $0x0E, %ah
    xor %bx, %bx
    int $0x10
    jmp tty
1:  ret

    .p2align 2
dap:
    .byte 16, 0
    .word 4                     /* sectors */
    .word 0x7E00, 0             /* buffer */
    .quad 1                     /* LBA */
bootdrv: .byte 0
msg_mbr: .asciz "mbr: loading stage 2\r\n"
msg_fail: .asciz "FAIL\r\n"

    .org 510
    .word 0xAA55

/* ---- sector 1 onwards: loaded at 0x7E00 ---- */
stage2:
    mov $A(msg_s2), %si
    call tty
    mov A(bootdrv), %al
    call hex8
    mov $0x08, %ah              /* geometry: max head, sectors/track */
    mov A(bootdrv), %dl
    int $0x13
    jc fail
    mov %dh, %al
    call hex8
    mov %cl, %al
    and $0x3F, %al
    call hex8
    int $0x11                   /* equipment word */
    call hex16
    mov $0xE801, %ax            /* memory 1-16 MiB in KiB, above 16 MiB in 64 KiB */
    int $0x15
    jc fail
    push %bx                    /* the print helpers use BX */
    call hex16
    pop %ax
    call hex16
    call crlf

    sti
    mov $A(msg_key), %si
    call tty
1:  xor %ah, %ah                /* echo keys until Enter */
    int $0x16
    cmp $0x0D, %al
    je 2f
    mov $0x0E, %ah
    xor %bx, %bx
    int $0x10
    jmp 1b
2:  call crlf

    /* A20: with the gate off, FFFF:0510 wraps to 0000:0500 */
    movw $0x1234, 0x500
    mov $0xFFFF, %ax
    mov %ax, %es
    in $0x92, %al
    and $0xFD, %al
    out %al, $0x92
    movw $0x5678, %es:0x510
    mov 0x500, %ax
    call hex16
    in $0x92, %al
    or $2, %al
    out %al, $0x92
    movw $0x9ABC, %es:0x510
    mov 0x500, %ax
    call hex16
    xor %ax, %ax
    mov %ax, %es

    mov 0x46C, %ebx             /* INT 15h AH=86h: wait 0.2 s, ticks advance */
    mov $0x0003, %cx
    mov $0x0D40, %dx
    mov $0x86, %ah
    int $0x15
    jc fail
    sti
    hlt
    hlt
    mov 0x46C, %eax
    sub %ebx, %eax
    cmp $1, %eax
    setae %al
    call hex8
    call crlf
    mov $A(msg_bye), %si
    call tty
    xor %al, %al
    out %al, $0xF4
    hlt

crlf:
    mov $A(msg_crlf), %si
    jmp tty

hex16:
    push %ax
    mov %ah, %al
    call hex8n
    pop %ax
hex8:
    call hex8n
    mov $' ', %al
    jmp putc
hex8n:
    push %ax
    shr $4, %al
    call nib
    pop %ax
nib:
    and $15, %al
    add $'0', %al
    cmp $'9', %al
    jbe putc
    add $7, %al
putc:
    mov $0x0E, %ah
    xor %bx, %bx
    int $0x10
    ret

msg_s2: .asciz "stage 2 from drive "
msg_key: .asciz "type: "
msg_bye: .asciz "bye\r\n"
msg_crlf: .asciz "\r\n"
