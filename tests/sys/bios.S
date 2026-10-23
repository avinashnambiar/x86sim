/* PC platform test: BIOS services (video teletype, memory size, E820, disk
 * reads by CHS and by packet, drive parameters, keyboard, time, A20, PCI
 * BIOS), the timer tick (PIT -> PIC -> INT 08h -> BIOS data area), the ATA
 * disk through its ports, CMOS, the UART in loopback, PCI configuration
 * ports, and in protected mode the local APIC timer (one-shot and TSC
 * deadline) and an I/O APIC route for the PIT. The disk image is this
 * object file itself (see bios.args); keystrokes come from bios.in. */

    .code16
    .globl _start
_start:
    cli
    xor %ax, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    mov $0x7C00, %sp

    mov $msg_tty, %si           /* INT 10h teletype (echoed to the console) */
1:  lodsb
    test %al, %al
    jz 2f
    mov $0x0E, %ah
    int $0x10
    jmp 1b
2:
    int $0x12                   /* conventional memory in KiB */
    call hex16
    xor %ebx, %ebx              /* E820 entries */
    xor %bp, %bp
1:  mov $0xE820, %eax
    mov $20, %ecx
    mov $0x534D4150, %edx
    mov $buf, %di
    int $0x15
    jc 2f
    inc %bp
    test %ebx, %ebx
    jnz 1b
2:  mov %bp, %ax
    call hex16
    call nl

    mov $0x0201, %ax            /* INT 13h: read sector 0 by CHS */
    mov $0x0001, %cx
    mov $0x0080, %dx
    mov $buf, %bx
    int $0x13
    jc fail
    mov buf, %eax
    call hex32
    movw $16, dap               /* extended read of sector 1 */
    movw $1, dap + 2
    movw $buf, dap + 4
    movw $0, dap + 6
    movl $1, dap + 8
    movl $0, dap + 12
    mov $0x42, %ah
    mov $0x80, %dl
    mov $dap, %si
    int $0x13
    jc fail
    mov buf, %esi
    mov $0x0201, %ax            /* the same sector by CHS (sector 2) */
    mov $0x0002, %cx
    mov $0x0080, %dx
    mov $buf, %bx
    int $0x13
    jc fail
    cmp buf, %esi
    sete %al
    call hex8
    mov $0x08, %ah              /* drive parameters */
    mov $0x80, %dl
    int $0x13
    jc fail
    mov %dx, %ax
    call hex16
    call nl

    /* ATA through the ports: IDENTIFY, then READ SECTORS (LBA 0) */
    mov $0x1F6, %dx
    mov $0xA0, %al
    out %al, %dx
    mov $0x1F7, %dx
    mov $0xEC, %al
    out %al, %dx
    call ata_wait
    mov $buf, %di
    mov $256, %cx
    mov $0x1F0, %dx
    rep insw
    mov buf + 54, %ax           /* model, first two characters (byte-swapped) */
    xchg %al, %ah
    call hex16
    mov buf + 120, %eax         /* LBA28 sector count */
    and $0xFFFFFF00, %eax
    test %eax, %eax
    setz %al
    movzbw %al, %ax
    call hex16
    mov $0x1F2, %dx
    mov $1, %al
    out %al, %dx
    inc %dx
    xor %al, %al
    out %al, %dx                /* LBA 0 */
    inc %dx
    out %al, %dx
    inc %dx
    out %al, %dx
    inc %dx
    mov $0xE0, %al
    out %al, %dx
    inc %dx
    mov $0x20, %al
    out %al, %dx
    call ata_wait
    mov $buf, %di
    mov $256, %cx
    mov $0x1F0, %dx
    rep insw
    mov buf, %eax
    call hex32
    mov $0x1F7, %dx
    in %dx, %al                 /* no DRQ after the whole sector */
    and $0x08, %al
    call hex8
    call nl

    mov $0x0B, %al              /* CMOS status register B */
    out %al, $0x70
    in $0x71, %al
    call hex8
    mov $0x07, %al              /* UART: clear the FIFOs (keystrokes arrive there too) */
    mov $0x3FA, %dx
    out %al, %dx
    mov $0x10, %al              /* loopback */
    mov $0x3FC, %dx
    out %al, %dx
    mov $0x3F8, %dx
    mov $'Z', %al
    out %al, %dx
    mov $0x3FD, %dx
    in %dx, %al
    and $1, %al
    call hex8
    mov $0x3F8, %dx
    in %dx, %al
    call hex8
    mov $0x3FC, %dx
    xor %al, %al
    out %al, %dx
    mov $0x2402, %ax            /* A20 state */
    int $0x15
    call hex8
    mov $0xB101, %ax            /* PCI BIOS */
    int $0x1A
    mov %edx, %eax
    call hex32
    mov $0xCF8, %dx             /* config read 00:00.0 register 0 */
    mov $0x80000000, %eax
    out %eax, %dx
    mov $0xCFC, %dx
    in %dx, %eax
    call hex32
    call nl

    /* timer tick and keyboard: interrupts on */
    sti
    mov 0x46C, %ebx
1:  hlt
    cmp 0x46C, %ebx
    je 1b
    mov $msg_tick, %si
    call puts
    mov $3, %cx
1:  xor %ah, %ah
    int $0x16
    call hex16
    loop 1b
    mov $1, %ah                 /* buffer now empty: ZF set */
    int $0x16
    setz %al
    call hex8
    call nl
    cli

    /* protected mode, flat */
    mov $0xFF, %al              /* mask both 8259s: the APIC takes over */
    out %al, $0x21
    out %al, $0xA1
    lgdtl gdt_desc
    lidtl idt_desc
    mov %cr0, %eax
    or $1, %eax
    mov %eax, %cr0
    ljmpl $0x08, $pm

fail:
    mov $msg_fail, %si
    call puts
    mov $1, %al
    out %al, $0xF4
    hlt

ata_wait:
    mov $0x1F7, %dx
1:  in %dx, %al
    test $0x80, %al
    jnz 1b
    test $0x08, %al
    jz fail
    ret

puts:
    lodsb
    test %al, %al
    jz 1f
    out %al, $0xE9
    jmp puts
1:  ret

nl:
    mov $'\n', %al
    out %al, $0xE9
    ret

hex32:
    push %eax
    shr $16, %eax
    call hex16n
    pop %eax
hex16:
    call hex16n
    mov $' ', %al
    out %al, $0xE9
    ret
hex16n:
    push %ax
    mov %ah, %al
    call hex8n
    pop %ax
hex8n:
    push %ax
    shr $4, %al
    call nib
    pop %ax
nib:
    push %ax
    and $15, %al
    add $'0', %al
    cmp $'9', %al
    jbe 1f
    add $7, %al
1:  out %al, $0xE9
    pop %ax
    ret
hex8:
    call hex8n
    mov $' ', %al
    out %al, $0xE9
    ret

    .code32
pm:
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    mov $0x90000, %esp
    mov $0x40, %ecx
    mov $isr_timer, %eax
    call set_gate
    mov $0x41, %ecx
    mov $isr_deadline, %eax
    call set_gate
    mov $0x42, %ecx
    mov $isr_pit, %eax
    call set_gate
    mov $0xFF, %ecx
    mov $isr_spurious, %eax
    call set_gate

    movl $0x1FF, 0xFEE000F0     /* APIC on, spurious vector 0xFF */
    mov 0xFEE00030, %eax        /* version */
    and $0xFF, %eax
    call phex
    movl $0xB, 0xFEE003E0       /* divide by 1 */
    movl $0x40, 0xFEE00320      /* one-shot, vector 0x40 */
    movl $100000, 0xFEE00380
    sti
1:  hlt
    cmpl $0, done
    je 1b
    cli
    movl $0, done
    mov 0xFEE00390, %eax        /* current count after expiry */
    call phex

    movl $0x41 | (2 << 17), 0xFEE00320 /* TSC-deadline mode */
    rdtsc
    add $50000, %eax
    adc $0, %edx
    mov $0x6E0, %ecx
    wrmsr
    sti
1:  hlt
    cmpl $0, done
    je 1b
    cli
    movl $0, done
    movl $0x10000, 0xFEE00320   /* timer masked */

    movl $0x14, 0xFEC00000      /* I/O APIC pin 2 (PIT) -> vector 0x42, edge */
    movl $0x42, 0xFEC00010
    movl $0x15, 0xFEC00000
    movl $0, 0xFEC00010
    movl $0x01, 0xFEC00000      /* version register */
    mov 0xFEC00010, %eax
    call phex
    sti
1:  hlt
    cmpl $0, done
    je 1b
    cli
    mov $msg_bye, %esi
1:  lodsb
    test %al, %al
    jz 2f
    out %al, $0xE9
    jmp 1b
2:  xor %al, %al
    out %al, $0xF4
    hlt

isr_timer:
    push %esi
    mov $msg_lapic, %esi
    jmp isr_common
isr_deadline:
    push %esi
    mov $msg_deadline, %esi
    jmp isr_common
isr_pit:
    push %esi
    movl $0x14, 0xFEC00000      /* mask the pin again */
    movl $0x10042, 0xFEC00010
    mov $msg_pit, %esi
isr_common:
    push %eax
1:  lodsb
    test %al, %al
    jz 2f
    out %al, $0xE9
    jmp 1b
2:  movl $1, done
    movl $0, 0xFEE000B0         /* EOI */
    pop %eax
    pop %esi
    iret
isr_spurious:
    iret

/* ecx = vector, eax = handler: 32-bit interrupt gate */
set_gate:
    lea idt(,%ecx,8), %edi
    mov %ax, (%edi)
    movw $0x08, 2(%edi)
    movw $0x8E00, 4(%edi)
    shr $16, %eax
    mov %ax, 6(%edi)
    ret

phex:
    push %ecx
    push %eax
    mov $8, %ecx
1:  rol $4, %eax
    push %eax
    and $15, %al
    add $'0', %al
    cmp $'9', %al
    jbe 2f
    add $7, %al
2:  out %al, $0xE9
    pop %eax
    loop 1b
    mov $'\n', %al
    out %al, $0xE9
    pop %eax
    pop %ecx
    ret

    .data
msg_tty: .asciz "bios: hello\r\n"
msg_tick: .asciz "tick "
msg_fail: .asciz "FAIL\n"
msg_lapic: .asciz "lapic timer\n"
msg_deadline: .asciz "tsc deadline\n"
msg_pit: .asciz "ioapic pit\n"
msg_bye: .asciz "bye\n"
done: .long 0
    .p2align 3
gdt:
    .quad 0
    .quad 0x00CF9A000000FFFF
    .quad 0x00CF92000000FFFF
gdt_end:
gdt_desc:
    .word gdt_end - gdt - 1
    .long gdt
idt_desc:
    .word 256 * 8 - 1
    .long idt

    .bss
    .p2align 4
dap: .space 16
buf: .space 512
idt: .space 256 * 8
