/* x2APIC and VT-d interrupt remapping (run with --smp 4).
 *   x2APIC: CPUID, enabling through IA32_APIC_BASE, the MSR registers (ID,
 *   LDR, version), #GP for an illegal mode switch, a read-only register
 *   and a register x2APIC mode removed, the MMIO page going dark, AP
 *   bring-up through the 64-bit ICR, self IPI, physical, logical-cluster
 *   and broadcast IPIs with 32-bit destinations.
 *   VT-d: version and capabilities, a queued-invalidation wait descriptor
 *   writing its status, the interrupt-remapping table, a compatibility-
 *   format I/O APIC interrupt blocked until CFI is set, and a remappable
 *   one steered by its IRTE to APIC ID 3. The PIT (IRQ 0, I/O APIC pin 2)
 *   raises the interrupts. */

#define CODE32 0x08
#define DATA0 0x10
#define IOAPIC 0xFEC00000
#define DMAR 0xFED90000
#define IQ 0x20000                      /* invalidation queue */
#define IRT 0x21000                     /* interrupt-remapping table */

#define WRMSR(idx, hi, lo) mov $idx, %ecx; mov $hi, %edx; mov $lo, %eax; wrmsr

    .code16
    .globl _start
_start:
    cli
    xor %ax, %ax
    mov %ax, %ds
    mov %ax, %ss
    mov $0x7C00, %sp
    lgdtl gdt_desc
    mov %cr0, %eax
    or $1, %eax
    mov %eax, %cr0
    ljmpl $CODE32, $bsp32

ap_entry16:
    cli
    xor %ax, %ax
    mov %ax, %ds
    lgdtl gdt_desc
    mov %cr0, %eax
    or $1, %eax
    mov %eax, %cr0
    ljmpl $CODE32, $ap32

    .code32
/* the gate for vector \v -> \handler */
.macro GATE v, handler
    mov $\handler, %eax
    mov %ax, idt + \v * 8
    movw $CODE32, idt + \v * 8 + 2
    movw $0x8E00, idt + \v * 8 + 4
    shr $16, %eax
    mov %ax, idt + \v * 8 + 6
.endm

bsp32:
    mov $DATA0, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    mov $0x90000, %esp
    GATE 13, gp_handler
    GATE 0x40, ipi_handler
    GATE 0x50, irq50_handler
    GATE 0x51, irq51_handler
    lidt idt_desc
    mov $0xFF, %al                      /* the 8259s stay out of it */
    out %al, $0x21
    out %al, $0xA1

    mov $msg_cpuid, %esi
    call puts
    mov $1, %eax
    cpuid
    shr $21, %ecx
    and $1, %ecx
    mov %ecx, %eax
    call print_hex
    call newline

    mov $0x1B, %ecx                     /* enable x2APIC */
    rdmsr
    or $0x400, %eax
    wrmsr
    mov $msg_regs, %esi
    call puts
    mov $0x802, %ecx                    /* ID */
    rdmsr
    call print_hex
    mov $0x80D, %ecx                    /* LDR: cluster 0, bit 0 */
    rdmsr
    call print_hex
    mov $0x803, %ecx                    /* version */
    rdmsr
    call print_hex
    mov 0xFEE00030, %eax                /* the MMIO page reads as zero now */
    call print_hex
    call newline

    mov $msg_gp, %esi
    call puts
    movl $1f, recover                   /* x2APIC -> xAPIC directly: illegal */
    mov $0x1B, %ecx
    rdmsr
    and $~0x400, %eax
    wrmsr
1:  movl $1f, recover                   /* the ID is read-only */
    WRMSR(0x802, 0, 5)
1:  movl $1f, recover                   /* no DFR in x2APIC mode */
    mov $0x80E, %ecx
    rdmsr
1:  mov gp_count, %eax
    call print_hex
    call newline

    WRMSR(0x80F, 0, 0x1FF)              /* software enable */
    WRMSR(0x835, 0, 0x10000)            /* LINT0 masked */
    movb $0xEA, 0x70000                 /* trampoline: ljmp $0, $ap_entry16 */
    mov $ap_entry16, %eax
    mov %ax, 0x70001
    movw $0, 0x70003
    WRMSR(0x830, 0, 0x000C4500)         /* INIT, all excluding self */
    WRMSR(0x830, 0, 0x000C4670)         /* STARTUP 0x70 */
1:  pause
    cmpl $3, arrived
    jne 1b
    mov $msg_aps, %esi
    call puts
    mov bitmap, %eax
    call print_hex
    call newline
    sti

    WRMSR(0x83F, 0, 0x40)               /* self IPI */
1:  cmpl $1, cnt40
    jne 1b
    WRMSR(0x830, 2, 0x40)               /* physical: APIC ID 2 */
1:  cmpl $1, cnt40 + 8
    jne 1b
    WRMSR(0x830, 0xA, 0x840)            /* logical: cluster 0, IDs 1 and 3 */
1:  cmpl $1, cnt40 + 4
    jne 1b
1:  cmpl $1, cnt40 + 12
    jne 1b
    WRMSR(0x830, 0xFFFFFFFF, 0x40)      /* broadcast */
    mov $3, %ebx
1:  cmpl $2, cnt40(,%ebx,4)
    jne 1b
    dec %ebx
    jns 1b
    mov $msg_ipi, %esi
    call puts
    xor %ebx, %ebx
1:  mov cnt40(,%ebx,4), %eax
    call print_hex
    inc %ebx
    cmp $4, %ebx
    jne 1b
    call newline

    /* ---- VT-d ---- */
    mov $msg_vtd, %esi
    call puts
    mov DMAR, %eax                      /* version */
    call print_hex
    mov DMAR + 0x10, %eax               /* ECAP: QI, IR, EIM */
    and $0x1A, %eax
    call print_hex
    movl $IQ, DMAR + 0x90               /* queued invalidation */
    movl $0, DMAR + 0x94
    movl $0x04000000, DMAR + 0x18       /* QIE */
1:  testl $0x04000000, DMAR + 0x1C
    jz 1b
    movl $0x25, IQ                      /* wait descriptor, status write */
    movl $0x600DF00D, IQ + 4
    movl $qi_status, IQ + 8
    movl $0, IQ + 12
    movl $0x10, DMAR + 0x88             /* tail */
    mov qi_status, %eax
    call print_hex
    mov DMAR + 0x80, %eax               /* head caught up */
    call print_hex
    call newline

    movl $0x00500001, IRT + 5 * 16      /* IRTE 5: present, vector 0x50 */
    movl $3, IRT + 5 * 16 + 4           /* destination: x2APIC ID 3 */
    movl $IRT | 0x800 | 7, DMAR + 0xB8  /* 256 entries, extended (32-bit) IDs */
    movl $0, DMAR + 0xBC
    movl $0x05000000, DMAR + 0x18       /* QIE + SIRTP */
1:  testl $0x01000000, DMAR + 0x1C
    jz 1b
    movl $0x06000000, DMAR + 0x18       /* QIE + IRE */
1:  testl $0x02000000, DMAR + 0x1C
    jz 1b

    mov $msg_ir, %esi
    call puts
    mov $0x51, %eax                     /* pin 2, compatibility format, to the BSP */
    xor %edx, %edx
    call set_pin2
    call pit_fire
    mov cnt51, %eax                     /* blocked: CFI is off */
    call print_hex
    mov $0x50, %eax                     /* remappable format, index 5 */
    mov $(5 << 17) | (1 << 16), %edx
    call set_pin2
    call pit_fire
1:  cmpl $1, cnt50 + 12
    jne 1b
    mov cnt50 + 12, %eax                /* APIC ID 3 took it */
    call print_hex
    movl $0x06800000, DMAR + 0x18       /* + CFI */
1:  testl $0x00800000, DMAR + 0x1C
    jz 1b
    mov $0x51, %eax
    xor %edx, %edx
    call set_pin2
    call pit_fire
1:  cmpl $1, cnt51
    jne 1b
    mov cnt51, %eax                     /* now it passes */
    call print_hex
    call newline

    mov $msg_bye, %esi
    call puts
    xor %al, %al
    out %al, $0xF4
    hlt

/* I/O APIC pin 2 (IRQ 0): low dword EAX, high dword EDX, edge, unmasked */
set_pin2:
    movl $0x15, IOAPIC
    mov %edx, IOAPIC + 0x10
    movl $0x14, IOAPIC
    mov %eax, IOAPIC + 0x10
    ret

/* PIT channel 0, mode 0: IRQ 0 rises after 100 ticks; then wait a while */
pit_fire:
    mov $0x30, %al
    out %al, $0x43
    mov $100, %al
    out %al, $0x40
    xor %al, %al
    out %al, $0x40
    mov $200000, %ecx
1:  loop 1b
    ret

ap32:
    mov $DATA0, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    mov $0x1B, %ecx
    rdmsr
    or $0x400, %eax
    wrmsr
    mov $0x802, %ecx
    rdmsr
    mov %eax, %ebx
    mov %eax, %esp                      /* a 1 KiB stack per APIC ID */
    shl $10, %esp
    neg %esp
    add $0x80000, %esp
    lidt idt_desc
    WRMSR(0x80F, 0, 0x1FF)
    lock bts %ebx, bitmap
    lock incl arrived
    sti
1:  hlt
    jmp 1b

/* interrupt handlers: count per APIC ID, EOI through the MSR */
.macro COUNTER name, table
\name:
    push %eax
    push %ecx
    push %edx
    mov $0x802, %ecx
    rdmsr
    lock incl \table(,%eax,4)
    WRMSR(0x80B, 0, 0)
    pop %edx
    pop %ecx
    pop %eax
    iret
.endm
    COUNTER ipi_handler, cnt40
    COUNTER irq50_handler, cnt50
    COUNTER irq51_handler, cnt51

gp_handler:                             /* count, resume at 'recover' */
    lock incl gp_count
    add $4, %esp                        /* error code */
    push %eax
    mov recover, %eax
    mov %eax, 4(%esp)
    pop %eax
    iret

puts:
    push %eax
1:  lodsb
    test %al, %al
    jz 2f
    out %al, $0xE9
    jmp 1b
2:  pop %eax
    ret

newline:
    push %eax
    mov $'\n', %al
    out %al, $0xE9
    pop %eax
    ret

print_hex:
    push %ecx
    push %eax
    mov $8, %ecx
1:  rol $4, %eax
    push %eax
    and $15, %eax
    mov hexdig(%eax), %al
    out %al, $0xE9
    pop %eax
    loop 1b
    mov $' ', %al
    out %al, $0xE9
    pop %eax
    pop %ecx
    ret

    .data
    .p2align 6
arrived: .long 0
bitmap: .long 0
gp_count: .long 0
recover: .long 0
qi_status: .long 0
    .p2align 6
cnt40: .long 0, 0, 0, 0
cnt50: .long 0, 0, 0, 0
cnt51: .long 0, 0, 0, 0
hexdig: .ascii "0123456789abcdef"
msg_cpuid: .asciz "cpuid x2apic "
msg_regs: .asciz "x2apic id, ldr, version, mmio read "
msg_gp: .asciz "#gp: bad mode switch, read-only id, no dfr "
msg_aps: .asciz "aps up through the x2apic icr "
msg_ipi: .asciz "self, physical, logical, broadcast ipis per cpu "
msg_vtd: .asciz "vt-d version, ecap qi/ir/eim, qi wait status, queue head "
msg_ir: .asciz "ioapic: compat blocked, remapped to id 3, compat with cfi "
msg_bye: .asciz "bye\n"
    .p2align 3
gdt:
    .quad 0
    .quad 0x00CF9A000000FFFF            /* 0x08 code32 */
    .quad 0x00CF92000000FFFF            /* 0x10 data */
gdt_end:
gdt_desc:
    .word gdt_end - gdt - 1
    .long gdt
idt_desc:
    .word 0x52 * 8 - 1
    .long idt

    .bss
    .p2align 3
idt: .space 0x52 * 8
