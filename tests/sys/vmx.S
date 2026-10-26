/* Bare-metal VMX test: a 64-bit host (identity-mapped 1 GiB, 2 MiB pages)
 * enters VMX operation and runs two guests.
 *  - VMXON preconditions (#UD without CR4.VMXE, #GP without
 *    IA32_FEATURE_CONTROL), VMfailInvalid/VMfailValid and VM-instruction
 *    error numbers, VMPTRST, launch state, bad controls (VMfailValid 7),
 *    invalid guest state (VM-entry failure exit).
 *  - Guest 1: 64-bit, sharing the host's page tables, GDT and IDT. Exits:
 *    CPUID, HLT, I/O bitmap, MSR bitmaps (low/high, read/write), CR0
 *    guest/host mask and read shadow, CR3-target list, INT3 and #PF via the
 *    exception bitmap with error-code mask/match (CR2 untouched), event
 *    injection, interrupt window after STI, monitor trap flag, external
 *    interrupt with acknowledge on exit, NMI exiting, virtual NMIs with the
 *    NMI window, triple fault.
 *  - Guest 2: unrestricted real-mode guest under EPT (4 KiB EPT pages,
 *    A/D flags) with a VPID. EPT violations (not present, write to a
 *    read-only page, during a guest page walk), an EPT misconfiguration,
 *    then the guest enters protected mode and turns on paging itself.
 * Output goes to port 0xE9; the exit code is written to port 0xF4. */

#define CODE32 0x08
#define DATA0 0x10
#define CODE64 0x18
#define KDATA 0x20
#define TSS64 0x28
#define GCODE32 0x08 /* guest 2's own GDT */
#define GDATA32 0x10

#define G2_HPA 0x400000 /* guest 2 physical memory starts here in host memory */

    .macro VMW field, val
    mov \val, %rax
    mov $\field, %edx
    vmwrite %rax, %rdx
    .endm
    .macro VMR field
    mov $\field, %edx
    vmread %rdx, %rax
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
    mov $pml4, %edi                     /* identity map 1 GiB with 2 MiB pages */
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
    movl $0x3FE00081, pd + 511 * 8      /* last 2 MiB read-only */
    movl $(pd_apic + 3), pdpt + 3 * 8
    movl $0xFEE0009B, pd_apic + 0x1F7 * 8 /* the local APIC, uncached */
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
    xor %ecx, %ecx                      /* IDT: exception stubs, vector 0x40 */
1:  mov stub_table(,%rcx,8), %rax
    call set_gate
    inc %ecx
    cmp $32, %ecx
    jne 1b
    mov $0x40, %ecx
    mov $int40, %eax
    call set_gate
    lidt idt_desc
    mov $tss, %eax
    mov %ax, gdt + TSS64 + 2
    shr $16, %eax
    mov %al, gdt + TSS64 + 4
    mov %ah, gdt + TSS64 + 7
    mov $TSS64, %ax
    ltr %ax
    mov $0xFEE000F0, %edi
    orl $0x100, (%rdi)                  /* local APIC software enable */

    /* ---- capabilities and VMXON ---- */
    mov $msg_cpuid, %esi
    call puts
    mov $1, %eax
    cpuid
    mov %ecx, %eax
    shr $5, %eax
    and $1, %eax
    call print_hex
    call newline

    movl $6, expect                     /* VMXON without CR4.VMXE: #UD */
    movq $1f, resume
    vmxon vmxon_ptr
1:  mov %cr4, %rax
    or $0x2000, %rax
    mov %rax, %cr4
    movl $13, expect                    /* IA32_FEATURE_CONTROL not locked: #GP */
    movq $1f, resume
    vmxon vmxon_ptr
1:  mov $0x3A, %ecx
    mov $5, %eax                        /* lock | VMX outside SMX */
    xor %edx, %edx
    wrmsr
    movl $13, expect                    /* locked: writes #GP */
    movq $1f, resume
    wrmsr
1:  mov $0x480, %ecx                    /* IA32_VMX_BASIC */
    rdmsr
    mov %eax, revision
    shl $32, %rdx
    or %rdx, %rax
    mov $msg_basic, %esi
    call puts
    call print_hex
    call newline

    mov $msg_vmxon, %esi                /* bad revision: VMfailInvalid, then success, then VMfailInvalid again */
    call puts
    vmxon vmxon_ptr
    call print_flags
    mov revision, %eax
    mov %eax, vmxon_region
    mov %eax, vmcs1
    mov %eax, vmcs2
    vmxon vmxon_ptr
    call print_flags
    vmxon vmxon_ptr
    call print_flags
    call newline

    /* ---- VMCS pointer instructions and errors ---- */
    mov $msg_vmcs, %esi
    call puts
    VMR 0x4400                          /* no current VMCS: VMfailInvalid */
    call print_flags
    vmptrld vmxon_ptr                   /* the VMXON pointer: VMfailValid? no current VMCS -> invalid */
    call print_flags
    vmptrld vmcs1_ptr
    call print_flags
    vmptrst scratch
    mov scratch, %rax
    call print_hex
    vmptrld vmxon_ptr                   /* now VMfailValid, error 10 */
    call print_flags
    call print_insn_err
    VMR 0x1234                          /* unsupported field: error 12 */
    call print_flags
    call print_insn_err
    VMW 0x4402, $0                      /* an exit-information field: allowed (VMWRITE to any field) */
    call print_flags
    call print_insn_err
    vmclear vmcs1_ptr
    vmptrld vmcs1_ptr
    vmresume                            /* not launched: error 5 */
    call print_flags
    call print_insn_err
    call newline

    /* ---- guest 1: 64-bit ---- */
    call setup_host
    mov $msr_bitmap, %edi               /* MSR bitmaps: write 0x174, read 0xC0000080 */
    orb $0x10, 2048 + 0x174 / 8(%rdi)
    orb $0x01, 1024 + 0x80 / 8(%rdi)
    orb $0x01, io_bitmap_a + 0x80 / 8   /* I/O bitmap: port 0x80 */
    mov $0x481, %ecx
    mov $0x29, %eax                     /* external-interrupt and NMI exiting, virtual NMIs */
    call adjust
    mov %rax, %rbx
    VMW 0x4000, %rbx
    mov $0x48E, %ecx
    mov $0x92008080, %eax               /* HLT, CR3-load, I/O and MSR bitmaps, secondary controls */
    call adjust
    mov %rax, %rbx
    VMW 0x4002, %rbx
    VMW 0x401E, $0
    mov $0x490, %ecx
    mov $0x8200, %eax                   /* IA-32e mode guest, load IA32_EFER */
    call adjust
    mov %rax, %rbx
    VMW 0x4012, %rbx
    mov $0xC0000080, %ecx
    rdmsr
    mov %rax, %rbx
    VMW 0x2806, %rbx
    VMW 0x2000, $io_bitmap_a
    VMW 0x2002, $io_bitmap_b
    VMW 0x2004, $msr_bitmap
    VMW 0x4004, $0x4008                 /* exceptions: #BP, #PF */
    VMW 0x4006, $1                      /* #PF: exit only for protection violations (P = 1) */
    VMW 0x4008, $1
    VMW 0x6000, $8                      /* CR0.TS belongs to the host, reads as 1 */
    VMW 0x6004, $8
    VMW 0x400A, $1                      /* CR3-target list: the current CR3 */
    mov %cr3, %rbx
    VMW 0x6008, %rbx
    mov %cr0, %rbx
    VMW 0x6800, %rbx
    mov %cr3, %rbx
    VMW 0x6802, %rbx
    mov %cr4, %rbx
    VMW 0x6804, %rbx
    VMW 0x6006, $0
    VMW 0x6002, $0
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
    VMW 0x681E, $guest1
    VMW 0x6820, $0                      /* invalid: RFLAGS bit 1 clear */
    VMW 0x2800, $-1
    VMW 0x6824, $0x5555                 /* guest SYSENTER_ESP */
    VMW 0x4826, $0
    VMW 0x4824, $0

    mov $msg_badctl, %esi               /* pin-based controls without their default1 bits */
    call puts
    VMW 0x4000, $0x29
    vmlaunch
    call print_flags
    call print_insn_err
    mov $0x481, %ecx
    mov $0x29, %eax
    call adjust
    mov %rax, %rbx
    VMW 0x4000, %rbx
    call newline
    movq $1f, host_cont                 /* invalid guest state: VM-entry failure exit */
    vmlaunch
    mov $msg_unexp, %esi
    call puts
1:  VMW 0x6820, $2
    movq $guest1_done, host_cont
    vmlaunch
    mov $msg_unexp, %esi
    call puts
    jmp fail

guest1_done:                            /* the guest triple-faulted */
    VMR 0x681E
    sub $guest1_tf, %rax
    mov $msg_tfrip, %esi
    call puts
    call print_hex
    call newline
    vmlaunch                            /* launched: error 4 */
    call print_flags
    call print_insn_err
    call newline

    /* ---- guest 2: unrestricted real mode under EPT ---- */
    mov $msg_g2, %esi
    call puts
    mov $guest2, %esi                   /* guest code at guest-physical 0x1000 */
    mov $(G2_HPA + 0x1000), %edi
    mov $(guest2_end - guest2), %ecx
    rep movsb
    movl $0xBEEF, G2_HPA + 0x30000      /* behind a page the EPT does not map yet */
    movl $0x2003, G2_HPA + 0x7000       /* guest page table for linear 4 MiB: page 0 -> GPA 0x2000 */
    movq $(ept_pdpt + 7), ept_pml4
    movq $(ept_pd + 7), ept_pdpt
    movq $(ept_pt + 7), ept_pd
    mov $ept_pt, %edi                   /* GPA 0-0x1FFFF -> HPA G2_HPA + GPA, RWX, write-back */
    mov $(G2_HPA + 0x37), %eax
    mov $32, %ecx
1:  mov %rax, (%rdi)
    add $0x1000, %eax
    add $8, %edi
    loop 1b
    movq $(G2_HPA + 0x5031), ept_pt + 5 * 8 /* read/execute only */
    movq $(G2_HPA + 0x6032), ept_pt + 6 * 8 /* write without read: misconfigured */
    movq $0, ept_pt + 7 * 8                 /* not present */

    vmptrld vmcs2_ptr
    call setup_host
    mov $0x481, %ecx
    mov $0x01, %eax
    call adjust
    mov %rax, %rbx
    VMW 0x4000, %rbx
    mov $0x48E, %ecx
    mov $0x80000080, %eax               /* HLT exiting, secondary controls */
    call adjust
    mov %rax, %rbx
    VMW 0x4002, %rbx
    VMW 0x401E, $0xA2                   /* EPT, VPID, unrestricted guest */
    VMW 0x0000, $1
    VMW 0x201A, $(ept_pml4 + 0x5E)      /* write-back, 4 levels, accessed/dirty flags */
    mov $0x490, %ecx
    mov $0x8000, %eax                   /* load IA32_EFER (0: LME clear for the guest's own paging) */
    call adjust
    mov %rax, %rbx
    VMW 0x4012, %rbx
    VMW 0x2806, $0
    VMW 0x4004, $0
    VMW 0x6800, $0x30                   /* ET | NE: real mode, paging off */
    VMW 0x6802, $0
    VMW 0x6804, $0x2000
    VMW 0x6002, $0x2000                 /* CR4.VMXE belongs to the host, reads as 0 */
    VMW 0x6006, $0
    VMW 0x6000, $0
    xor %ebx, %ebx
1:  lea 0x0800(,%rbx,2), %edx           /* every segment: selector 0, base 0, limit 64K */
    xor %eax, %eax
    vmwrite %rax, %rdx
    lea 0x6806(,%rbx,2), %edx
    vmwrite %rax, %rdx
    lea 0x4800(,%rbx,2), %edx
    mov $0xFFFF, %eax
    vmwrite %rax, %rdx
    lea 0x4814(,%rbx,2), %edx
    mov $0x93, %eax
    vmwrite %rax, %rdx
    inc %ebx
    cmp $8, %ebx
    jne 1b
    VMW 0x4816, $0x9B
    VMW 0x4820, $0x82
    VMW 0x4822, $0x8B
    VMW 0x6816, $0
    VMW 0x4810, $0xFFFF
    VMW 0x6818, $0
    VMW 0x4812, $0x3FF
    VMW 0x681C, $0x8000
    VMW 0x681E, $0x1000
    VMW 0x6820, $2
    VMW 0x2800, $-1
    VMW 0x4826, $0
    VMW 0x4824, $0
    movl $2, phase
    movq $guest2_done, host_cont
    vmlaunch
    mov $msg_unexp, %esi
    call puts
    jmp fail

guest2_done:
    mov $msg_ad, %esi                   /* EPT accessed/dirty: code page A, data page A+D, unused page neither */
    call puts
    mov ept_pt + 1 * 8, %rax
    and $0x300, %eax
    call print_hex
    mov ept_pt + 2 * 8, %rax
    and $0x300, %eax
    call print_hex
    mov ept_pt + 0x10 * 8, %rax
    and $0x300, %eax
    call print_hex
    call newline
    mov $msg_g2mem, %esi
    call puts
    mov G2_HPA + 0x2000, %eax
    call print_hex
    mov G2_HPA + 0x5000, %eax
    call print_hex
    call newline
    mov $msg_stats, %esi
    call puts
    vmxoff
    call print_flags
    mov %cr4, %rax                      /* VMXE may be cleared again */
    and $~0x2000, %rax
    mov %rax, %cr4
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
    mov $msg_exit, %esi
    call puts
    call print_hex
    VMR 0x6400
    call print_hex
    cmp $1, %ebx
    ja 1f
    VMR 0x4404                          /* interruption information */
    call print_hex
1:  cmp $48, %ebx
    jb 1f
    cmp $49, %ebx
    ja 1f
    VMR 0x2400                          /* guest-physical address */
    call print_hex
1:  call newline
    cmp $0x80000021, %ebx
    je to_host
    cmp $2, %ebx
    je to_host
    cmp $0, %ebx
    je x_exception
    cmp $7, %ebx
    je x_intwin
    cmp $8, %ebx
    je x_nmiwin
    cmp $12, %ebx
    je x_hlt
    cmp $18, %ebx
    je x_vmcall
    cmp $37, %ebx
    je x_mtf
    cmp $48, %ebx
    je x_eptv
    cmp $49, %ebx
    je x_eptm
    cmp $1, %ebx
    je x_resume
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

x_exception:
    VMR 0x4404
    cmp $0x80000b0e, %eax               /* #PF: continue the guest where it says */
    jne 1f
    VMR 0x4406
    call print_hex
    call newline
    mov gresume, %rbx
    VMW 0x681E, %rbx
    jmp x_resume
1:  shr $8, %eax
    and $7, %eax
    cmp $6, %eax                        /* software exception: skip INT3 */
    je skip
    jmp x_resume                        /* NMI */
x_intwin:
    mov $~4, %ebx
    jmp clear_proc
x_nmiwin:
    mov $~(1 << 22), %ebx
    jmp clear_proc
x_mtf:
    mov $~(1 << 27), %ebx
clear_proc:
    VMR 0x4002
    and %rax, %rbx
    VMW 0x4002, %rbx
    mov $msg_grip, %esi
    call puts
    VMR 0x681E
    sub $guest1, %rax
    call print_hex
    call newline
    jmp x_resume
x_hlt:
    cmpl $2, phase
    je to_host
    jmp skip
x_vmcall:
    mov 48(%rsp), %rax                  /* the guest's RAX: a request */
    cmp $1, %eax
    jne 1f
    VMW 0x4016, $0x80000040             /* inject external interrupt 0x40 */
    jmp skip
1:  cmp $2, %eax
    jne 1f
    mov $4, %ebx                        /* interrupt-window exiting */
    jmp set_proc
1:  cmp $3, %eax
    jne 1f
    mov $(1 << 27), %ebx                /* monitor trap flag */
    jmp set_proc
1:  cmp $4, %eax
    jne 1f
    VMW 0x4016, $0x80000202             /* inject an NMI */
    jmp skip
1:  cmp $5, %eax
    jne 1f
    mov $(1 << 22), %ebx                /* NMI-window exiting */
    jmp set_proc
1:  mov $msg_val, %esi                  /* report a value from the guest */
    call puts
    mov 40(%rsp), %rax                  /* guest RBX */
    call print_hex
    call newline
    jmp skip
set_proc:
    VMR 0x4002
    or %rax, %rbx
    VMW 0x4002, %rbx
    jmp skip
x_eptv:                                 /* map the page (writable) and resume */
    VMR 0x2400
    shr $12, %eax
    and $0x1FF, %eax
    lea ept_pt(,%rax,8), %rdi
    shl $12, %eax
    add $(G2_HPA + 0x37), %eax
    mov %rax, (%rdi)
    jmp invept_resume
x_eptm:
    VMR 0x2400
    shr $12, %eax
    and $0x1FF, %eax
    orq $1, ept_pt(,%rax,8)             /* readable now */
invept_resume:
    mov $2, %ecx                        /* all-context INVEPT */
    invept invept_desc, %rcx
    jmp x_resume

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

/* ---- guest 1 (64-bit) ---- */
guest1:
    mov $msg_g1, %esi
    call puts
    mov $0x1111, %ebx
    cpuid                               /* exit 10; RBX survives the host */
    mov $0x1111, %ebx
    mov $0x10, %eax
    vmcall                              /* exit 18: report RBX */
    hlt                                 /* exit 12 */
    out %al, $0x80                      /* exit 30 */
    out %al, $0x81                      /* not in the bitmap */
    mov $0x174, %ecx
    wrmsr                               /* exit 32 */
    mov $0xC0000080, %ecx
    rdmsr                               /* exit 31 */
    mov $0x175, %ecx
    rdmsr                               /* passes through: the guest's SYSENTER_ESP */
    mov %eax, %ebx
    mov $0x10, %eax
    vmcall
    mov %cr0, %rbx                      /* TS from the read shadow */
    and $8, %ebx
    mov $0x10, %eax
    vmcall
    mov %cr0, %rbx
    or $8, %rbx
    mov %rbx, %cr0                      /* matches the shadow: no exit */
    and $~8, %rbx
    mov %rbx, %cr0                      /* changes an owned bit: exit 28 */
    mov %cr3, %rax
    mov %rax, %cr3                      /* a CR3 target: no exit */
    mov $0x12345000, %ecx
    mov %rcx, %cr3                      /* exit 28 (skipped by the host) */
    int3                                /* exit 0, software exception */
    movl $6, expect                     /* #UD is not in the bitmap: the guest IDT */
    movq $1f, resume
    ud2
1:  movl $14, expect                    /* not present: error code doesn't match, delivered */
    movq $1f, resume
    mov $0x80000000, %ebx
    mov (%rbx), %eax
1:  movq $1f, gresume                   /* protection violation: exits, CR2 untouched */
    movl $1, 0x3FE00000
1:  mov %cr2, %rbx
    mov $0x10, %eax
    vmcall
    movl $0, counter40
    mov $1, %eax                        /* injected interrupt 0x40 (IF = 0) */
    vmcall
    mov counter40, %ebx
    mov $0x10, %eax
    vmcall
    mov $2, %eax                        /* interrupt window */
    vmcall
    nop
    sti
    nop                                 /* the window opens after this one (STI shadow) */
guest1_win:
    nop
    cli
    mov $3, %eax                        /* monitor trap flag: one instruction */
    vmcall
    nop
    mov $0xFEE00300, %edi
    movl $0x00044050, (%rdi)            /* self IPI: external-interrupt exit */
    nop
    movl $0, 0x10(%rdi)                 /* NMI to APIC 0: NMI exit */
    movl $0x00000400, (%rdi)
    nop
    movl $2, expect                     /* injected NMI: virtual-NMI blocking until IRET */
    movq $1f, resume
    mov $4, %eax
    vmcall
1:  mov $5, %eax                        /* NMI window: open at once */
    vmcall
    lidt null_idt                       /* triple fault */
guest1_tf:
    ud2

int40:
    incl counter40
    iretq

/* ---- guest 2 (copied to guest-physical 0x1000) ---- */
#define G2(x) ((x) - guest2 + 0x1000)
    .code16
guest2:
    mov $G2(g2_msg), %si
1:  lodsb
    test %al, %al
    jz 2f
    out %al, $0xE9                      /* not intercepted */
    jmp 1b
2:  movw $0x1234, 0x2000
    mov $0x3000, %ax                    /* EPT violation: not present */
    mov %ax, %ds
    mov 0, %bx
    xor %ax, %ax
    mov %ax, %ds
    mov $0x10, %ax
    vmcall
    movw $0x5555, 0x5000                /* EPT violation: write to a read-only page */
    mov 0x6000, %bx                     /* EPT misconfiguration */
    mov $0x10, %ax
    vmcall
    mov %cr4, %ebx                      /* VMXE hidden by the read shadow */
    mov $0x10, %ax
    vmcall
    lgdtl G2(g2_gdt_desc)               /* protected mode, then paging (4 MiB pages) */
    mov %cr0, %eax
    or $1, %eax
    mov %eax, %cr0
    ljmpl $GCODE32, $G2(g2_pm)
    .code32
g2_pm:
    mov $GDATA32, %ax
    mov %ax, %ds
    mov %ax, %ss
    movl $0x83, 0x9000                  /* PDE 0: 4 MiB page at 0 */
    movl $0x7003, 0x9004                /* PDE 1: page table at GPA 0x7000 (not in the EPT yet) */
    mov %cr4, %eax
    or $0x10, %eax
    mov %eax, %cr4
    mov $0x9000, %eax
    mov %eax, %cr3
    mov %cr0, %eax
    or $0x80000000, %eax
    mov %eax, %cr0
    mov 0x400000, %ebx                  /* EPT violation in the guest page walk; then GPA 0x2000 */
    mov $0x10, %eax
    vmcall
    hlt
g2_msg: .asciz "real-mode guest\n"
    .p2align 3
g2_gdt:
    .quad 0
    .quad 0x00CF9A000000FFFF
    .quad 0x00CF92000000FFFF
g2_gdt_desc:
    .word 23
    .long G2(g2_gdt)
guest2_end:

    .code64
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
expect: .long -1
phase: .long 1
counter40: .long 0
revision: .long 0
    .p2align 3
resume: .quad 0
gresume: .quad 0
host_cont: .quad 0
scratch: .quad 0
vmxon_ptr: .long vmxon_region, 0
vmcs1_ptr: .long vmcs1, 0
vmcs2_ptr: .long vmcs2, 0
invept_desc: .quad 0, 0
null_idt: .word 0
    .quad 0
hexdig: .ascii "0123456789abcdef"
msg_cpuid: .asciz "cpuid.1:ecx.vmx "
msg_basic: .asciz "\nvmx_basic "
msg_vmxon: .asciz "vmxon: bad revision, ok, again "
msg_vmcs: .asciz "vmcs: no vmcs, vmxon ptr, vmptrld, vmptrst / errors "
msg_badctl: .asciz "bad controls "
msg_exit: .asciz "exit "
msg_g1: .asciz "64-bit guest\n"
msg_g2: .asciz "\nunrestricted guest under EPT\n"
msg_grip: .asciz "  guest rip +"
msg_val: .asciz "  value "
msg_tfrip: .asciz "triple fault at guest1_tf +"
msg_ad: .asciz "EPT A/D bits "
msg_g2mem: .asciz "guest memory "
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
vmcs2: .space 4096
msr_bitmap: .space 4096
io_bitmap_a: .space 4096
io_bitmap_b: .space 4096
ept_pml4: .space 4096
ept_pdpt: .space 4096
ept_pd: .space 4096
ept_pt: .space 4096
tss: .space 0x68
