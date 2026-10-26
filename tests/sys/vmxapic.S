/* Bare-metal test of the VMX features beyond the basics (see vmx.S for
 * those): one 64-bit guest under an identity EPT (2 MiB pages, 4 KiB pages
 * for 4-6 MiB, accessed/dirty flags), sharing the host's page tables and
 * IDT. The guest asks the host for each phase with VMCALL.
 *  - TPR shadow: CR8 reads and writes, the TPR threshold (after an
 *    instruction and right after VM entry), the APIC-access page
 *    (virtualized TPR, APIC-access exits).
 *  - Virtual-interrupt delivery with APIC-register virtualization: RVI,
 *    virtual EOIs, the EOI-exit bitmap, self IPIs, APIC-write exits, VPPR
 *    masking.
 *  - Posted interrupts: a notification vector raised in root operation is
 *    processed in the guest without a VM exit.
 *  - x2APIC-mode virtualization: TPR, register reads, self IPI, EOI, #GP.
 *  - The VMX-preemption timer (running and in HLT), pause-loop exiting,
 *    TSC scaling, page-modification logging (and the log-full exit), VM
 *    functions (EPTP switching) and VMCS shadowing.
 * Output goes to port 0xE9; the exit code is written to port 0xF4. */

#define CODE32 0x08
#define DATA0 0x10
#define CODE64 0x18
#define KDATA 0x20
#define TSS64 0x28
#define PI_VECTOR 0xF2

    .macro VMW field, val
    mov \val, %rax
    mov $\field, %edx
    vmwrite %rax, %rdx
    .endm
    .macro VMR field
    mov $\field, %edx
    vmread %rdx, %rax
    .endm
    .macro REPORT                       /* the host prints the guest's RBX */
    mov $0x10, %eax
    vmcall
    .endm
    .macro REQ n                        /* ask the host for phase n */
    mov $\n, %eax
    vmcall
    .endm
    .macro SETB field, bits             /* field |= bits (in the handler) */
    VMR \field
    or $\bits, %rax
    mov $\field, %edx
    vmwrite %rax, %rdx
    .endm
    .macro CLRB field, bits
    VMR \field
    and $~\bits, %rax
    mov $\field, %edx
    vmwrite %rax, %rdx
    .endm

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
    mov $0xFF, %al                      /* mask the 8259s */
    out %al, $0x21
    out %al, $0xA1
    mov $pml4, %edi                     /* identity map 1 GiB with 2 MiB pages, and the local APIC */
    mov $(4 * 4096 / 4), %ecx
    xor %eax, %eax
    rep stosl
    movl $(pdpt + 3), pml4
    movl $(pd + 3), pdpt
    mov $pd, %edi
    mov $0x83, %eax
    mov $512, %ecx
1:  mov %eax, (%edi)
    add $0x200000, %eax
    add $8, %edi
    loop 1b
    movl $(pd_apic + 3), pdpt + 3 * 8
    movl $0xFEE0009B, pd_apic + 0x1F7 * 8
    mov %cr4, %eax
    or $0x20, %eax
    mov %eax, %cr4
    mov $pml4, %eax
    mov %eax, %cr3
    mov $0xC0000080, %ecx
    rdmsr
    or $0x100, %eax
    wrmsr
    mov %cr0, %eax
    or $0x80010020, %eax                /* PG | WP | NE */
    mov %eax, %cr0
    ljmp $CODE64, $lm64

    .code64
lm64:
    mov $KDATA, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    mov $0x90000, %rsp
    xor %ecx, %ecx                      /* IDT: exception stubs, the guest's virtual interrupts */
1:  mov stub_table(,%rcx,8), %rax
    call set_gate
    inc %ecx
    cmp $32, %ecx
    jne 1b
    mov $vint_table, %esi
1:  mov (%rsi), %ecx
    test %ecx, %ecx
    jz 2f
    mov 4(%rsi), %eax
    call set_gate
    add $8, %esi
    jmp 1b
2:  lidt idt_desc
    mov $tss, %eax
    mov %ax, gdt + TSS64 + 2
    shr $16, %eax
    mov %al, gdt + TSS64 + 4
    mov %ah, gdt + TSS64 + 7
    mov $TSS64, %ax
    ltr %ax
    mov $0xFEE000F0, %edi
    orl $0x100, (%rdi)                  /* local APIC software enable */

    /* ---- the EPT: GPA = HPA ---- */
    movq $(ept_pdpt + 7), ept_pml4
    movq $(ept_pd + 7), ept_pdpt
    movq $(ept_pd3 + 7), ept_pdpt + 3 * 8
    mov $ept_pd, %edi                   /* 2 MiB pages, RWX, write-back */
    mov $0xB7, %eax
    mov $1024, %ecx                     /* ept_pd and ept_pd3 (0-1 GiB, 3-4 GiB) */
1:  mov %rax, (%rdi)
    add $0x200000, %rax
    add $8, %edi
    cmp $512 * 8 + ept_pd, %edi
    jne 2f
    mov $0xC00000B7, %eax
    mov $ept_pd3, %edi
2:  loop 1b
    movq $(ept_pt + 7), ept_pd + 2 * 8  /* 4-6 MiB: 4 KiB pages */
    mov $ept_pt, %edi
    mov $0x400037, %eax
    mov $512, %ecx
1:  mov %rax, (%rdi)
    add $0x1000, %eax
    add $8, %edi
    loop 1b
    movl $0x1111, 0x405000              /* VMFUNC: the second EPT maps GPA 0x405000 to 0x406000 */
    movl $0x2222, 0x406000
    mov $ept_pml4, %esi
    mov $ept2_pml4, %edi
    mov $(4 * 4096 / 8), %ecx           /* pml4, pdpt, pd, pt, then fix the links */
1:  mov (%rsi), %rax
    mov %rax, (%rdi)
    add $8, %esi
    add $8, %edi
    cmp $ept_pd3, %esi                  /* the copy skips ept_pd3 (shared) */
    jne 2f
    add $4096, %esi
2:  loop 1b
    movq $(ept2_pdpt + 7), ept2_pml4
    movq $(ept2_pd + 7), ept2_pdpt
    movq $(ept2_pt + 7), ept2_pd + 2 * 8
    movq $0x406037, ept2_pt + 5 * 8
    movq $(ept_pml4 + 0x5E), eptp_list
    movq $(ept2_pml4 + 0x5E), eptp_list + 8

    /* ---- VMXON and the guest's VMCS ---- */
    mov %cr4, %rax
    or $0x2000, %rax
    mov %rax, %cr4
    mov $0x3A, %ecx
    mov $5, %eax
    xor %edx, %edx
    wrmsr
    mov $0x480, %ecx
    rdmsr
    mov %eax, revision
    mov %eax, vmxon_region
    mov %eax, vmcs1
    mov $msg_caps, %esi                 /* secondary allowed-1, MISC, VMFUNC */
    call puts
    mov $0x48B, %ecx
    rdmsr
    mov %edx, %eax
    call print_hex
    mov $0x485, %ecx
    rdmsr
    call print_hex
    mov $0x491, %ecx
    rdmsr
    call print_hex
    call newline
    vmxon vmxon_ptr
    vmclear vmcs1_ptr
    vmptrld vmcs1_ptr
    call setup_host
    mov $0x48D, %ecx
    mov $0x89, %eax                     /* external-interrupt and NMI exiting, posted interrupts */
    call adjust
    mov %rax, %rbx
    VMW 0x4000, %rbx
    mov $0x48E, %ecx
    mov $0x90200080, %eax               /* HLT exiting, TPR shadow, MSR bitmaps, secondary controls */
    call adjust
    mov %rax, %rbx
    VMW 0x4002, %rbx
    VMW 0x401E, $3                      /* virtualize APIC accesses, EPT */
    VMW 0x201A, $(ept_pml4 + 0x5E)
    mov $0x490, %ecx
    mov $0x8200, %eax                   /* IA-32e mode guest, load IA32_EFER */
    call adjust
    mov %rax, %rbx
    VMW 0x4012, %rbx
    mov $0xC0000080, %ecx
    rdmsr
    mov %rax, %rbx
    VMW 0x2806, %rbx
    VMW 0x2004, $msr_bitmap
    VMW 0x2012, $vapic
    VMW 0x2014, $0xFEE00000             /* the APIC-access page: where the guest sees its APIC */
    VMW 0x2016, $pi_desc
    VMW 0x0002, $PI_VECTOR
    VMW 0x401C, $2                      /* TPR threshold */
    movl $0x30, vapic + 0x80            /* VTPR */
    movl $0x12000000, vapic + 0x20      /* the virtual APIC ID */
    VMW 0x200E, $pml_buf
    VMW 0x2024, $eptp_list
    VMW 0x4004, $0
    VMW 0x6000, $0
    VMW 0x6002, $0
    mov %cr0, %rbx
    VMW 0x6800, %rbx
    mov %cr3, %rbx
    VMW 0x6802, %rbx
    mov %cr4, %rbx
    VMW 0x6804, %rbx
    VMW 0x0800, $KDATA                  /* ES CS SS DS FS GS LDTR TR */
    VMW 0x0802, $CODE64
    VMW 0x0804, $KDATA
    VMW 0x0806, $KDATA
    VMW 0x0808, $0
    VMW 0x080A, $0
    VMW 0x080C, $0
    VMW 0x080E, $TSS64
    xor %ebx, %ebx
1:  lea 0x6806(,%rbx,2), %edx           /* bases 0, limits 4G */
    xor %eax, %eax
    vmwrite %rax, %rdx
    lea 0x4800(,%rbx,2), %edx
    mov $0xFFFFFFFF, %eax
    vmwrite %rax, %rdx
    inc %ebx
    cmp $6, %ebx
    jne 1b
    VMW 0x4814, $0xC093
    VMW 0x4816, $0xA09B
    VMW 0x4818, $0xC093
    VMW 0x481A, $0xC093
    VMW 0x481C, $0x10000
    VMW 0x481E, $0x10000
    VMW 0x4820, $0x10000
    VMW 0x4822, $0x8B
    VMW 0x480E, $0x67
    VMW 0x6814, $tss
    VMW 0x6816, $gdt
    VMW 0x4810, $(gdt_end - gdt - 1)
    VMW 0x6818, $idt
    VMW 0x4812, $0xFFF
    VMW 0x681C, $0x70000
    VMW 0x681E, $guest
    VMW 0x6820, $2
    VMW 0x2800, $-1
    VMW 0x4826, $0
    VMW 0x4824, $0

    mov $msg_badctl, %esi               /* posted interrupts need virtual-interrupt delivery */
    call puts
    vmlaunch
    call print_flags
    call print_insn_err
    CLRB 0x4000, 0x80
    VMW 0x401E, $0x201                  /* virtual-interrupt delivery needs the TPR shadow */
    CLRB 0x4002, 0x200000
    vmlaunch
    call print_flags
    call print_insn_err
    SETB 0x4002, 0x200000
    VMW 0x401E, $3
    call newline
    movq $guest_done, host_cont
    vmlaunch
    mov $msg_unexp, %esi
    call puts
    jmp fail

guest_done:
    mov $msg_stats, %esi
    call puts
    vmxoff
    call print_flags
    mov $msg_bye, %esi
    call puts
    xor %al, %al
    out %al, $0xF4
    hlt

fail:
    mov $1, %al
    out %al, $0xF4
    hlt

/* Host state for the current VMCS: this CPU as it is now. */
setup_host:
    mov %cr0, %rbx
    VMW 0x6C00, %rbx
    mov %cr3, %rbx
    VMW 0x6C02, %rbx
    mov %cr4, %rbx
    VMW 0x6C04, %rbx
    VMW 0x0C00, $KDATA
    VMW 0x0C02, $CODE64
    VMW 0x0C04, $KDATA
    VMW 0x0C06, $KDATA
    VMW 0x0C08, $0
    VMW 0x0C0A, $0
    VMW 0x0C0C, $TSS64
    VMW 0x6C06, $0
    VMW 0x6C08, $0
    VMW 0x6C0A, $tss
    VMW 0x6C0C, $gdt
    VMW 0x6C0E, $idt
    VMW 0x6C14, $0x8F000
    VMW 0x6C16, $vmexit
    mov $0x48F, %ecx
    mov $0x308200, %eax                 /* host address-space size, acknowledge interrupt, save/load EFER */
    call adjust
    mov %rax, %rbx
    VMW 0x400C, %rbx
    mov $0xC0000080, %ecx
    rdmsr
    mov %rax, %rbx
    VMW 0x2C02, %rbx
    ret

/* eax = wanted controls, ecx = capability MSR: eax = (wanted | allowed-0) & allowed-1 */
adjust:
    push %rdx
    mov %eax, %r8d
    rdmsr
    or %eax, %r8d
    and %edx, %r8d
    mov %r8d, %eax
    pop %rdx
    ret

/* ---- the VM-exit handler ---- */
vmexit:
    push %rax
    push %rbx
    push %rcx
    push %rdx
    push %rsi
    push %rdi
    push %r8
    VMR 0x4402
    mov %eax, %ebx                      /* ebx = exit reason */
    cmp $18, %ebx                       /* VMCALLs print what they do themselves */
    je x_vmcall
    mov $msg_exit, %esi
    call puts
    call print_hex
    VMR 0x6400
    call print_hex
    call newline
    cmp $12, %ebx
    je to_host
    cmp $43, %ebx
    je x_tpr
    cmp $44, %ebx                       /* APIC access: fault-like */
    je skip
    cmp $45, %ebx                       /* virtualized EOI, APIC write: trap-like */
    je x_resume
    cmp $56, %ebx
    je x_resume
    cmp $52, %ebx
    je x_timer
    cmp $40, %ebx
    je x_ple
    cmp $62, %ebx
    je x_pml_full
    cmp $59, %ebx                       /* VMFUNC */
    je skip
    cmp $23, %ebx                       /* VMREAD */
    je skip
    mov $msg_unexp, %esi
    call puts
    jmp fail
skip:                                   /* instruction exits: step over it */
    VMR 0x440C
    mov %rax, %rbx
    VMR 0x681E
    add %rax, %rbx
    VMW 0x681E, %rbx
x_resume:
    pop %r8
    pop %rdi
    pop %rsi
    pop %rdx
    pop %rcx
    pop %rbx
    pop %rax
    vmresume
    mov $msg_vmrfail, %esi
    call puts
    call print_insn_err
    jmp fail
to_host:
    mov $0x90000, %rsp
    jmp *host_cont

x_tpr:                                  /* TPR below threshold: lower the threshold */
    VMW 0x401C, $0
    jmp x_resume
x_timer:                                /* the preemption timer: saved value, activity state */
    mov $msg_timer, %esi
    call puts
    VMR 0x482E
    call print_hex
    VMR 0x4826
    call print_hex
    call newline
    CLRB 0x4000, 0x40
    CLRB 0x400C, 0x400000
    VMR 0x4826
    test %eax, %eax
    jz 1f
    VMW 0x4826, $0                      /* halted: wake up after the HLT, which exits again */
    SETB 0x4002, 0x80
    jmp x_resume
1:  mov gresume, %rbx                   /* a loop: leave it */
    VMW 0x681E, %rbx
    jmp x_resume
x_ple:
    CLRB 0x401E, 0x400
    mov gresume, %rbx
    VMW 0x681E, %rbx
    jmp x_resume
x_pml_full:
    VMW 0x0812, $511
    jmp x_resume

x_vmcall:
    mov 48(%rsp), %rax                  /* the guest's RAX: a request */
    cmp $0x10, %eax
    jne 1f
    mov $msg_val, %esi                  /* report a value from the guest */
    call puts
    mov 40(%rsp), %rax                  /* guest RBX */
    call print_hex
    call newline
    jmp skip
1:  mov %eax, %ecx
    mov $msg_req, %esi
    call puts
    mov %ecx, %eax
    call print_hex
    call newline
    cmp $1, %ecx
    jne 1f
    SETB 0x401E, 0x300                  /* APIC-register virtualization, virtual-interrupt delivery */
    SETB 0x4000, 0x80                   /* posted interrupts */
    orl $(1 << 16), vapic + 0x220       /* VIRR: vector 0x50 */
    VMW 0x0810, $0x50                   /* RVI */
    jmp skip
1:  cmp $2, %ecx
    jne 1f
    VMW 0x201E, $(1 << 17)              /* EOI-exit bitmap: vector 0x51 */
    jmp skip
1:  cmp $3, %ecx
    jne 1f
    orl $2, pi_desc + 12                /* PIR: vector 0x61 */
    orb $1, pi_desc + 32                /* outstanding notification */
    mov $0xFEE00000, %edi               /* the notification: a self IPI, pending while IF = 0 */
    movl $(0x40000 | PI_VECTOR), 0x300(%rdi)
    jmp skip
1:  cmp $4, %ecx
    jne 1f
    VMR 0x401E                          /* virtualize x2APIC mode instead of APIC accesses */
    and $~1, %eax
    or $0x10, %eax
    mov %rax, %rbx
    VMW 0x401E, %rbx
    jmp skip
1:  cmp $5, %ecx
    jne 1f
    SETB 0x4000, 0x40                   /* preemption timer, saved on exit */
    SETB 0x400C, 0x400000
    VMW 0x482E, $100
    jmp skip
1:  cmp $6, %ecx
    jne 1f
    SETB 0x4000, 0x40
    SETB 0x400C, 0x400000
    VMW 0x482E, $50
    CLRB 0x4002, 0x80                   /* HLT runs in the guest */
    jmp skip
1:  cmp $7, %ecx
    jne 1f
    SETB 0x401E, 0x400                  /* pause-loop exiting */
    VMW 0x4020, $128
    VMW 0x4022, $2000
    jmp skip
1:  cmp $8, %ecx
    jne 1f
    SETB 0x4002, 8                      /* TSC offsetting and scaling: multiplier 0 */
    SETB 0x401E, 0x2000000
    VMW 0x2032, $0
    VMW 0x2010, $0x12345678
    jmp skip
1:  cmp $9, %ecx
    jne 1f
    CLRB 0x4002, 8
    CLRB 0x401E, 0x2000000
    jmp skip
1:  cmp $10, %ecx
    jne 1f
    mov $ept_pt, %edi                   /* clean EPT pages 4-6 MiB, then log */
    mov $512, %ecx
2:  andq $~0x200, (%rdi)
    add $8, %edi
    loop 2b
    mov $2, %ecx
    invept invept_desc, %rcx
    SETB 0x401E, 0x20000
    VMW 0x0812, $511
    jmp skip
1:  cmp $11, %ecx
    je 2f
    cmp $14, %ecx
    jne 1f
2:  mov $msg_pml, %esi                  /* PML index, the last two entries */
    call puts
    VMR 0x0812
    call print_hex
    mov pml_buf + 511 * 8, %rax
    call print_hex
    mov pml_buf + 510 * 8, %rax
    call print_hex
    call newline
    cmp $14, %ecx
    je 2f
    VMW 0x0812, $0xFFFF                 /* full */
    jmp skip
2:  CLRB 0x401E, 0x20000
    jmp skip
1:  cmp $12, %ecx
    jne 1f
    SETB 0x401E, 0x2000                 /* VM functions: EPTP switching */
    VMW 0x2018, $1
    jmp skip
1:  cmp $13, %ecx
    jne 1f
    mov revision, %eax                  /* a shadow VMCS */
    or $0x80000000, %eax
    mov %eax, vmcs_sh
    vmptrld vmcs_sh_ptr
    VMW 0x0000, $0x77
    mov $msg_shlaunch, %esi             /* it can't be launched */
    call puts
    vmlaunch
    call print_flags
    call newline
    vmptrld vmcs1_ptr
    VMW 0x2800, $vmcs_sh
    VMW 0x2026, $vmread_bm
    VMW 0x2028, $vmwrite_bm
    orb $4, vmread_bm + 0x4402 / 8      /* VMREAD of the exit reason exits */
    SETB 0x401E, 0x4000
    jmp skip
1:  cmp $15, %ecx
    jne 1f
    VMW 0x401C, $5                      /* threshold above VTPR: an exit right after VM entry */
    jmp skip
1:  cmp $17, %ecx
    jne 1f
    mov $msg_posted, %esi               /* PIR, ON, the local APIC's ISR and IRR for the notification */
    call puts
    mov pi_desc + 12, %eax
    call print_hex
    movzbl pi_desc + 32, %eax
    call print_hex
    mov $0xFEE00000, %edi
    mov 0x170(%rdi), %eax
    call print_hex
    mov 0x270(%rdi), %eax
    call print_hex
    call newline
    jmp skip
1:  mov $msg_unexp, %esi
    call puts
    jmp fail

print_flags:                            /* CF and ZF after a VMX instruction */
    push %rax
    pushf
    pop %rax
    and $0x41, %eax
    call print_hex
    pop %rax
    ret

print_insn_err:
    push %rax
    push %rdx
    VMR 0x4400
    call print_hex
    pop %rdx
    pop %rax
    ret

/* ---- the guest ---- */
guest:
    mov $msg_tpr, %esi
    call puts
    mov %cr8, %rbx                      /* VTPR 0x30 */
    REPORT
    mov $1, %ecx
    mov %rcx, %cr8                      /* below the threshold (2): exit 43 */
    mov $0xFEE00000, %edi
    mov 0x80(%rdi), %ebx                /* virtualized TPR read */
    REPORT
    mov 0x20(%rdi), %ebx                /* no APIC-register virtualization: exit 44 */
    movl $0x40, 0x80(%rdi)              /* virtualized TPR write */
    mov %cr8, %rbx
    REPORT
    movl $0, 0xB0(%rdi)                 /* EOI without virtual-interrupt delivery: exit 44 */
    REQ 15                              /* exit 43 right after the entry */
    xor %ecx, %ecx
    mov %rcx, %cr8

    mov $msg_vid, %esi
    call puts
    REQ 1
    sti                                 /* vector 0x50 from RVI */
    nop
    cli
    mov 0x20(%rdi), %ebx                /* virtualized ID read */
    REPORT
    movl $0x10040, 0x320(%rdi)          /* LVT timer: APIC-write exit */
    REQ 2
    movl $0x00044051, 0x300(%rdi)       /* self IPI 0x51; its EOI exits */
    sti
    nop
    cli
    movl $0x52, 0x300(%rdi)             /* not a self IPI: APIC-write exit */
    mov $6, %ecx
    mov %rcx, %cr8
    movl $0x00044055, 0x300(%rdi)       /* 0x55 waits behind VTPR 0x60 */
    sti
    nop
    cli
    mov $msg_tpr0, %esi
    call puts
    xor %ecx, %ecx
    mov %rcx, %cr8
    sti
    nop
    cli

    mov $msg_pi, %esi
    call puts
    REQ 3
    nop                                 /* the notification arrives: no exit */
    sti
    nop
    cli
    REQ 17

    mov $msg_x2, %esi
    call puts
    REQ 4
    movl $1, x2mode
    mov $0x808, %ecx                    /* TPR */
    rdmsr
    mov %eax, %ebx
    REPORT
    mov $0x808, %ecx
    mov $0x30, %eax
    xor %edx, %edx
    wrmsr
    mov %cr8, %rbx
    REPORT
    mov $0x802, %ecx                    /* ID, from the virtual-APIC page */
    rdmsr
    mov %eax, %ebx
    REPORT
    mov $0x808, %ecx
    xor %eax, %eax
    wrmsr
    mov $0x83F, %ecx                    /* SELF IPI */
    mov $0x56, %eax
    wrmsr
    sti
    nop
    cli
    movl $13, expect                    /* reserved TPR bits: #GP in the guest */
    movq $1f, resume
    mov $0x808, %ecx
    mov $0x100, %eax
    wrmsr
1:  movl $0, x2mode

    mov $msg_timer_g, %esi
    call puts
    REQ 5
    movq $1f, gresume
2:  jmp 2b                              /* until the preemption timer expires */
1:  REQ 6
    hlt                                 /* woken by the timer */
    mov $msg_ple, %esi
    call puts
    REQ 7
    movq $1f, gresume
2:  pause
    jmp 2b
1:  mov $msg_tsc, %esi
    call puts
    REQ 8
    rdtsc
    mov %eax, %ebx
    REPORT
    REQ 9

    mov $msg_pml_g, %esi
    call puts
    REQ 10
    movl $1, 0x401000
    movl $2, 0x402000
    movl $3, 0x401008                   /* already dirty */
    mov 0x403000, %ebx                  /* a read */
    REQ 11
    movl $4, 0x404000                   /* the log is full: exit, then logged */
    REQ 14

    mov $msg_vmfunc, %esi
    call puts
    REQ 12
    mov 0x405000, %ebx
    REPORT
    xor %eax, %eax
    mov $1, %ecx
    vmfunc
    mov 0x405000, %ebx
    REPORT
    xor %eax, %eax
    xor %ecx, %ecx
    vmfunc
    mov 0x405000, %ebx
    REPORT
    xor %eax, %eax
    mov $2, %ecx                        /* an invalid EPTP: exit 59 */
    vmfunc
    mov $1, %eax                        /* function 1 is not enabled: exit 59 */
    vmfunc

    mov $msg_shadow, %esi
    call puts
    REQ 13
    xor %edx, %edx                      /* VPID, from the shadow VMCS */
    vmread %rdx, %rbx
    REPORT
    mov $0x99, %ecx
    xor %edx, %edx
    vmwrite %rcx, %rdx
    xor %edx, %edx
    vmread %rdx, %rbx
    REPORT
    mov $0x4402, %edx                   /* exits: the bitmap bit is set */
    vmread %rdx, %rbx
    hlt

/* the guest's virtual interrupts: frame = vector, rip, cs, rflags, rsp, ss */
vint_common:
    push %rax
    push %rcx
    push %rdx
    push %rsi
    mov $msg_vint, %esi
    call puts
    mov 32(%rsp), %rax
    call print_hex
    call newline
    cmpl $0, x2mode
    jne 1f
    mov $0xFEE00000, %eax
    movl $0, 0xB0(%rax)                 /* virtual EOI */
    jmp 2f
1:  mov $0x80B, %ecx
    xor %eax, %eax
    xor %edx, %edx
    wrmsr
2:  pop %rsi
    pop %rdx
    pop %rcx
    pop %rax
    add $8, %rsp
    iretq

#define VSTUB(n) vstub##n: push $0x##n; jmp vint_common
VSTUB(50)
VSTUB(51)
VSTUB(52)
VSTUB(55)
VSTUB(56)
VSTUB(61)

/* ecx = vector, rax = handler */
set_gate:
    mov %ecx, %edi
    shl $4, %edi
    add $idt, %edi
    mov %ax, (%rdi)
    movw $CODE64, 2(%rdi)
    movw $0x8E00, 4(%rdi)
    shr $16, %rax
    mov %ax, 6(%rdi)
    movq $0, 8(%rdi)
    ret

/* exceptions: frame = vector, error, rip, cs, rflags, rsp, ss */
exc_common:
    push %rax
    push %rsi
    mov $msg_exc, %esi
    call puts
    mov 16(%rsp), %rax
    call print_hex
    mov 24(%rsp), %rax
    call print_hex
    call newline
    mov 16(%rsp), %rax
    cmp expect, %eax
    jne unexpected
    movl $-1, expect
    mov resume, %rax
    mov %rax, 32(%rsp)
    pop %rsi
    pop %rax
    add $16, %rsp
    iretq
unexpected:
    mov $msg_unexp, %esi
    call puts
    jmp fail

puts:
    push %rax
1:  lodsb
    test %al, %al
    jz 2f
    out %al, $0xE9
    jmp 1b
2:  pop %rax
    ret

newline:
    push %rax
    mov $'\n', %al
    out %al, $0xE9
    pop %rax
    ret

print_hex:                              /* eax, as 8 digits */
    push %rcx
    push %rax
    push %rbx
    mov %eax, %ebx
    mov $8, %ecx
1:  rol $4, %ebx
    mov %ebx, %eax
    and $15, %eax
    mov hexdig(%rax), %al
    out %al, $0xE9
    loop 1b
    mov $' ', %al
    out %al, $0xE9
    pop %rbx
    pop %rax
    pop %rcx
    ret

#define STUB(n) stub##n: push $0; push $n; jmp exc_common
#define STUBE(n) stub##n: push $n; jmp exc_common
STUB(0)
STUB(1)
STUB(2)
STUB(3)
STUB(4)
STUB(5)
STUB(6)
STUB(7)
STUBE(8)
STUB(9)
STUBE(10)
STUBE(11)
STUBE(12)
STUBE(13)
STUBE(14)
STUB(15)
STUB(16)
STUBE(17)
STUB(18)
STUB(19)
STUB(20)
STUBE(21)
STUB(22)
STUB(23)
STUB(24)
STUB(25)
STUB(26)
STUB(27)
STUB(28)
STUB(29)
STUB(30)
STUB(31)

    .data
    .p2align 3
stub_table:
    .long stub0, 0, stub1, 0, stub2, 0, stub3, 0, stub4, 0, stub5, 0, stub6, 0, stub7, 0
    .long stub8, 0, stub9, 0, stub10, 0, stub11, 0, stub12, 0, stub13, 0, stub14, 0, stub15, 0
    .long stub16, 0, stub17, 0, stub18, 0, stub19, 0, stub20, 0, stub21, 0, stub22, 0, stub23, 0
    .long stub24, 0, stub25, 0, stub26, 0, stub27, 0, stub28, 0, stub29, 0, stub30, 0, stub31, 0
vint_table:
    .long 0x50, vstub50, 0x51, vstub51, 0x52, vstub52, 0x55, vstub55, 0x56, vstub56, 0x61, vstub61, 0, 0
expect: .long -1
x2mode: .long 0
revision: .long 0
    .p2align 3
resume: .quad 0
gresume: .quad 0
host_cont: .quad 0
vmxon_ptr: .long vmxon_region, 0
vmcs1_ptr: .long vmcs1, 0
vmcs_sh_ptr: .long vmcs_sh, 0
invept_desc: .quad 0, 0
hexdig: .ascii "0123456789abcdef"
msg_caps: .asciz "secondary controls, misc, vmfunc "
msg_badctl: .asciz "bad controls "
msg_exit: .asciz "exit "
msg_req: .asciz "request "
msg_val: .asciz "  value "
msg_vint: .asciz "  virtual interrupt "
msg_timer: .asciz "  timer value, activity "
msg_pml: .asciz "  PML index, entries 511 510 "
msg_posted: .asciz "  PIR, ON, APIC ISR, IRR "
msg_shlaunch: .asciz "  VMLAUNCH of a shadow VMCS "
msg_tpr: .asciz "\nTPR shadow and the APIC-access page\n"
msg_vid: .asciz "\nvirtual-interrupt delivery\n"
msg_tpr0: .asciz "VTPR 0\n"
msg_pi: .asciz "\nposted interrupts\n"
msg_x2: .asciz "\nx2APIC virtualization\n"
msg_timer_g: .asciz "\npreemption timer\n"
msg_ple: .asciz "\npause-loop exiting\n"
msg_tsc: .asciz "\nTSC scaling\n"
msg_pml_g: .asciz "\npage-modification logging\n"
msg_vmfunc: .asciz "\nEPTP switching\n"
msg_shadow: .asciz "\nVMCS shadowing\n"
msg_stats: .asciz "vmxoff "
msg_vmrfail: .asciz "VMRESUME failed "
msg_exc: .asciz "exception "
msg_bye: .asciz "\nbye\n"
msg_unexp: .asciz "UNEXPECTED\n"

    .p2align 3
gdt:
    .quad 0
    .quad 0x00CF9A000000FFFF    /* 0x08 code32 */
    .quad 0x00CF92000000FFFF    /* 0x10 data */
    .quad 0x00AF9A000000FFFF    /* 0x18 code64 */
    .quad 0x00CF92000000FFFF    /* 0x20 data */
    .quad 0x0000890000000067    /* 0x28 TSS64 (base filled in) */
    .quad 0
gdt_end:
gdt_desc:
    .word gdt_end - gdt - 1
    .long gdt, 0
idt_desc:
    .word 0xFFF
    .long idt, 0

    .bss
    .p2align 12
pml4: .space 4096
pdpt: .space 4096
pd: .space 4096
pd_apic: .space 4096
idt: .space 4096
vmxon_region: .space 4096
vmcs1: .space 4096
vmcs_sh: .space 4096
msr_bitmap: .space 4096
vapic: .space 4096
pml_buf: .space 4096
eptp_list: .space 4096
vmread_bm: .space 4096
vmwrite_bm: .space 4096
ept_pml4: .space 4096                   /* ept_pml4..ept_pt are copied to ept2_* in order */
ept_pdpt: .space 4096
ept_pd: .space 4096
ept_pd3: .space 4096
ept_pt: .space 4096
ept2_pml4: .space 4096
ept2_pdpt: .space 4096
ept2_pd: .space 4096
ept2_pt: .space 4096
pi_desc: .space 64
tss: .space 0x68
