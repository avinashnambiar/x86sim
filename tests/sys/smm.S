/* System management mode (run with --smp 2). The BSP runs in 32-bit
 * protected mode with paging and raises SMIs through the APM port 0xB2;
 * one handler, copied to each SMBASE + 0x8000, finds its save area through
 * IA32_SMBASE and acts on the APM command:
 *   0x5A  record what SMM looks like, return a value in the caller's EAX,
 *         relocate SMBASE from 0x30000 to 0x60000
 *   0x5B  (now at 0x68000) protect 0x60000-0x6FFFF with the SMRRs and
 *         plant a value there
 *   0x5C  report the planted value, which a store from outside SMM must
 *         not have changed
 * Then an SMI IPI reaches the AP while it sits in HLT with interrupts off;
 * the handler sees the auto-HALT restart flag and clears it so the AP
 * carries on after its HLT. */

#define CODE32 0x08
#define DATA0 0x10
#define LAPIC 0xFEE00000
#define PD 0x10000
#define REC 0x5000                      /* the handler's records */

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

/* The SMI handler: 16-bit big real mode, DS = 0 with a 4 GiB limit, so
 * 32-bit address registers reach all of memory. */
smi_handler:
    mov $0x9E, %ecx                     /* IA32_SMBASE */
    rdmsr
    mov %eax, %ebx
    lock incl REC
    mov %cr0, %eax
    mov %eax, REC + 4
    mov 0xFFF8(%ebx), %eax       /* saved CR0 */
    mov %eax, REC + 8
    mov %ebx, REC + 0x18
    movzwl 0xFF02(%ebx), %eax    /* auto-HALT restart */
    mov %eax, REC + 0x14
    movw $0, 0xFF02(%ebx)        /* resume after the HLT */
    in $0xB2, %al
    movzbl %al, %eax
    mov %eax, REC + 0xC
    cmp $0x5A, %al
    jne 1f
    movl $0x1234, 0xFF5C(%ebx)   /* the caller's EAX */
    movl $0x60000, 0xFEF8(%ebx)  /* relocate SMBASE */
    rsm
1:  cmp $0x5B, %al
    jne 1f
    mov $0x1F2, %ecx                    /* SMRR: 0x60000, 64 KiB, write-back */
    mov $0x60006, %eax
    xor %edx, %edx
    wrmsr
    mov $0x1F3, %ecx
    mov $0xFFFF0800, %eax
    wrmsr
    xor %esi, %esi
    movl $0x5555, 0x60100(%esi)
    rsm
1:  cmp $0x5C, %al
    jne 1f
    xor %esi, %esi
    mov 0x60100(%esi), %eax
    mov %eax, REC + 0x10
1:  rsm
smi_handler_end:

    .code32
bsp32:
    mov $DATA0, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    mov $0x90000, %esp
    cld
    mov $smi_handler, %esi              /* handlers at both SMBASEs + 0x8000 */
    mov $0x38000, %edi
    mov $smi_handler_end - smi_handler, %ecx
    rep movsb
    mov $smi_handler, %esi
    mov $0x68000, %edi
    mov $smi_handler_end - smi_handler, %ecx
    rep movsb

    movl $0x83, PD                      /* paging: 4 MiB identity pages */
    movl $0xFEC00093, PD + 0x3FB * 4    /* the local APIC, uncached */
    mov %cr4, %eax
    or $0x10, %eax                      /* PSE */
    mov %eax, %cr4
    mov $PD, %eax
    mov %eax, %cr3
    mov %cr0, %eax
    or $0x80000000, %eax
    mov %eax, %cr0

    mov $msg_smi, %esi
    call puts
    mov $0x5A, %al
    out %al, $0xB2                      /* SMI */
    mov %eax, %edx
    mov REC, %eax                       /* count */
    call print_hex
    mov REC + 4, %eax                   /* CR0 in SMM: PE, PG clear */
    and $0x80000001, %eax
    call print_hex
    mov REC + 8, %eax                   /* saved CR0: PE, PG set */
    and $0x80000001, %eax
    call print_hex
    mov REC + 0xC, %eax                 /* the command */
    call print_hex
    mov %edx, %eax                      /* returned in EAX */
    call print_hex
    mov %cr0, %eax                      /* paging back on */
    and $0x80000001, %eax
    call print_hex
    call newline

    mov $msg_reloc, %esi
    call puts
    mov $0x5B, %al
    out %al, $0xB2
    mov REC, %eax
    call print_hex
    mov REC + 0x18, %eax                /* SMBASE the handler saw */
    call print_hex
    call newline

    mov $msg_smrr, %esi
    call puts
    mov 0x68000, %eax                   /* SMRAM from outside SMM */
    call print_hex
    movl $0xDEAD, 0x60100               /* dropped */
    mov $0x5C, %al
    out %al, $0xB2
    mov REC + 0x10, %eax                /* still 0x5555 */
    call print_hex
    call newline

    /* the AP: SMI IPI while it is halted */
    movb $0xEA, 0x70000
    mov $ap_entry16, %eax
    mov %ax, 0x70001
    movw $0, 0x70003
    movl $0x000C4500, LAPIC + 0x300
    movl $0x000C4670, LAPIC + 0x300
1:  pause
    cmpl $1, ap_state
    jne 1b
    mov $200, %ecx                      /* let it reach HLT */
1:  loop 1b
    movl $0x01000000, LAPIC + 0x310     /* destination: APIC ID 1 */
    movl $0x00004200, LAPIC + 0x300     /* delivery mode SMI */
1:  pause
    cmpl $2, ap_state
    jne 1b
    mov $msg_halt, %esi
    call puts
    mov REC + 0x14, %eax                /* auto-HALT restart was set */
    call print_hex
    mov REC + 0x18, %eax                /* the AP's own SMBASE */
    call print_hex
    mov ap_state, %eax
    call print_hex
    call newline

    mov $msg_count, %esi
    call puts
    mov $0x34, %ecx                     /* MSR_SMI_COUNT */
    rdmsr
    call print_hex
    call newline

    mov $msg_bye, %esi
    call puts
    xor %al, %al
    out %al, $0xF4
    hlt

ap32:
    mov $DATA0, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    mov $0x80000, %esp
    movl $1, ap_state
    hlt                                 /* interrupts are off: only the SMI wakes us */
    movl $2, ap_state
1:  hlt
    jmp 1b

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
ap_state: .long 0
hexdig: .ascii "0123456789abcdef"
msg_smi: .asciz "smi from paged pmode: count, cr0 in smm, saved cr0, command, eax back, cr0 after "
msg_reloc: .asciz "after relocation: count, smbase "
msg_smrr: .asciz "smrr: smram read outside smm, planted value "
msg_halt: .asciz "smi ipi to halted ap: auto-halt flag, its smbase, resumed after hlt "
msg_count: .asciz "smi count (bsp) "
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
