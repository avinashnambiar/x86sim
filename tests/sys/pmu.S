/* Performance counters (run with --cache): CPUID leaf 0xA, a fixed counter
 * counting instructions over a known loop, general counters for branches
 * (all and conditional), a user-only counter that must stay at zero in ring 0, L1D
 * and L2 misses over 64 fresh cache lines, RDPMC, and a counter overflow
 * that raises a performance-monitoring interrupt through the local APIC. */

#define CODE32 0x08
#define DATA0 0x10
#define LAPIC 0xFEE00000
#define BUF 0x400000

#define WRMSR(idx, lo) mov $idx, %ecx; mov $lo, %eax; xor %edx, %edx; wrmsr
#define RDMSR_PRINT(idx) mov $idx, %ecx; rdmsr; call print_hex

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
    ljmpl $CODE32, $pm32

    .code32
pm32:
    mov $DATA0, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    mov $0x90000, %esp
    mov $pmi_handler, %eax              /* vector 0x50 */
    mov %ax, idt + 0x50 * 8
    movw $CODE32, idt + 0x50 * 8 + 2
    movw $0x8E00, idt + 0x50 * 8 + 4
    shr $16, %eax
    mov %ax, idt + 0x50 * 8 + 6
    lidt idt_desc
    movl $0x1FF, LAPIC + 0xF0

    mov $msg_cpuid, %esi                /* version 4, 8 x 48-bit, 7 events; mispredicts unavailable; 3 fixed x 48-bit */
    call puts
    mov $0xA, %eax
    cpuid
    push %edx
    push %ebx
    call print_hex
    pop %eax
    call print_hex
    pop %eax
    call print_hex
    call newline

    /* fixed counter 0 (instructions, ring 0) over enable + 201 + 3 disable setup */
    WRMSR(0x38D, 0x001)
    WRMSR(0x38F, 0)
    mov $0x38F, %ecx
    xor %eax, %eax
    mov $1, %edx                        /* global enable: fixed counter 0 */
    wrmsr
    mov $100, %ebx
1:  dec %ebx
    jnz 1b
    WRMSR(0x38F, 0)
    mov $msg_fixed, %esi
    call puts
    RDMSR_PRINT(0x309)
    mov $0x40000000, %ecx               /* rdpmc of fixed counter 0 */
    rdpmc
    call print_hex
    call newline

    /* branches, mispredictions, user-only instructions */
    WRMSR(0xC1, 0)
    WRMSR(0xC2, 0)
    WRMSR(0xC3, 0)
    WRMSR(0x186, 0x4300C4)              /* BR_INST_RETIRED.ALL, usr+os */
    WRMSR(0x187, 0x4301C4)              /* BR_INST_RETIRED.CONDITIONAL */
    WRMSR(0x188, 0x4100C0)              /* INST_RETIRED, user only */
    WRMSR(0x38F, 7)
    mov $100, %ebx
1:  dec %ebx
    jnz 1b
    WRMSR(0x38F, 0)
    mov $msg_br, %esi
    call puts
    RDMSR_PRINT(0xC1)
    RDMSR_PRINT(0xC2)
    RDMSR_PRINT(0xC3)
    call newline

    /* cache misses over 64 lines nobody touched */
    WRMSR(0xC1, 0)
    WRMSR(0xC2, 0)
    WRMSR(0x186, 0x430151)              /* L1D.REPLACEMENT */
    WRMSR(0x187, 0x43412E)              /* LONGEST_LAT_CACHE.MISS */
    WRMSR(0x38F, 3)
    mov $BUF, %esi
    mov $64, %ebx
1:  mov (%esi), %eax
    add $64, %esi
    dec %ebx
    jnz 1b
    mov $BUF, %esi                      /* again: all hits now */
    mov $64, %ebx
1:  mov (%esi), %eax
    add $64, %esi
    dec %ebx
    jnz 1b
    WRMSR(0x38F, 0)
    mov $msg_cache, %esi
    call puts
    RDMSR_PRINT(0xC1)
    RDMSR_PRINT(0xC2)
    call newline

    /* overflow after 10 instructions with the interrupt enabled */
    movl $0x50, LAPIC + 0x340           /* LVT performance: fixed, vector 0x50 */
    WRMSR(0xC1, 0xFFFFFFF6)             /* -10, sign-extended to 48 bits */
    WRMSR(0x186, 0x5300C0)              /* INST_RETIRED, usr+os, INT */
    sti
    WRMSR(0x38F, 1)
    mov $100, %ebx
1:  dec %ebx
    jnz 1b
    WRMSR(0x38F, 0)
    cli
    mov $msg_pmi, %esi
    call puts
    mov pmis, %eax
    call print_hex
    mov pmi_status, %eax
    call print_hex
    RDMSR_PRINT(0x38E)                  /* cleared by the handler */
    mov LAPIC + 0x340, %eax             /* masked by the delivery */
    call print_hex
    call newline

    mov $msg_bye, %esi
    call puts
    xor %al, %al
    out %al, $0xF4
    hlt

pmi_handler:
    push %eax
    push %ecx
    push %edx
    incl pmis
    mov $0x38E, %ecx
    rdmsr
    mov %eax, pmi_status
    mov $0x390, %ecx                    /* reset the status bits */
    wrmsr
    movl $0, LAPIC + 0xB0               /* EOI */
    pop %edx
    pop %ecx
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
pmis: .long 0
pmi_status: .long 0
hexdig: .ascii "0123456789abcdef"
msg_cpuid: .asciz "cpuid 0xa eax ebx edx "
msg_fixed: .asciz "fixed0 instructions, rdpmc "
msg_br: .asciz "branches, conditional, user-only "
msg_cache: .asciz "l1d misses, l2 misses "
msg_pmi: .asciz "pmis, status in handler, status after, lvt "
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
    .word 0x51 * 8 - 1
    .long idt

    .bss
    .p2align 3
idt: .space 0x51 * 8
