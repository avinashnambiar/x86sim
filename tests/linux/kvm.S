/* /init for the KVM test: load irqbypass.ko, kvm.ko and kvm-intel.ko from
 * the initramfs, print which VMX features kvm_intel uses (its module
 * parameters, from sysfs), then be a minimal VMM through /dev/kvm: one VM
 * with an in-kernel irqchip, 64 KiB of memory with dirty logging, and one
 * vCPU starting in real mode at 0000:1000. The guest prints over port
 * 0x3F8 (KVM_EXIT_IO, echoed here), enters protected mode, takes a local
 * APIC timer interrupt and a self IPI (APIC virtualization: the APIC-access
 * page, virtual-interrupt delivery, posted interrupts), writes to an
 * address outside its memory slot (KVM_EXIT_MMIO) and reports it is done on
 * port 0x501. The VMM prints the dirty-page bitmap, which also cleans the
 * pages; the guest writes a page again (caught by page-modification
 * logging) and a new one, and the VMM prints the bitmap again and reboots. Position independent and relocation-free, like
 * init.S. */

#define SYS_write 1
#define SYS_open 2
#define SYS_close 3
#define SYS_read 0
#define SYS_mmap 9
#define SYS_ioctl 16
#define SYS_mkdir 83
#define SYS_mknod 133
#define SYS_mount 165
#define SYS_reboot 169
#define SYS_finit_module 313

#define KVM_GET_API_VERSION 0xAE00
#define KVM_CREATE_VM 0xAE01
#define KVM_GET_VCPU_MMAP_SIZE 0xAE04
#define KVM_CREATE_VCPU 0xAE41
#define KVM_GET_DIRTY_LOG 0x4010AE42
#define KVM_SET_USER_MEMORY_REGION 0x4020AE46
#define KVM_CREATE_IRQCHIP 0xAE60
#define KVM_RUN 0xAE80
#define KVM_SET_REGS 0x4090AE82
#define KVM_GET_SREGS 0x8138AE83
#define KVM_SET_SREGS 0x4138AE84

    .text
    .globl _start
_start:
    lea banner(%rip), %rsi
    call puts
    lea s_devkvm(%rip), %rdi            /* mknod /dev/kvm c 10 232 */
    mov $0x2180, %esi                   /* S_IFCHR | 0600 */
    mov $(10 << 8 | 232), %edx
    mov $SYS_mknod, %eax
    syscall

    lea m_irqbypass(%rip), %rdi
    call insmod
    lea m_kvm(%rip), %rdi
    call insmod
    lea m_kvm_intel(%rip), %rdi
    call insmod

    lea s_sys(%rip), %rdi               /* mount sysfs; print kvm_intel's parameters */
    mov $0755, %esi
    mov $SYS_mkdir, %eax
    syscall
    lea s_sysfs(%rip), %rdi
    lea s_sys(%rip), %rsi
    mov %rdi, %rdx
    xor %r10d, %r10d
    xor %r8d, %r8d
    mov $SYS_mount, %eax
    syscall
    lea s_params(%rip), %rsi
    call puts
    lea params(%rip), %rbx
1:  cmpb $0, (%rbx)
    je 3f
    call space                          /* " name=" */
    mov %rbx, %rsi
    call puts
    lea s_eq(%rip), %rsi
    call puts
    lea pathbuf(%rip), %rdi             /* path = prefix + name */
    lea s_parampath(%rip), %rsi
    call strcpy
    mov %rbx, %rsi
    call strcpy
    lea pathbuf(%rip), %rdi
    xor %esi, %esi
    mov $SYS_open, %eax
    syscall
    test %eax, %eax
    js 2f
    mov %eax, %ebp
    mov %eax, %edi
    lea numbuf(%rip), %rsi
    mov $15, %edx
    mov $SYS_read, %eax
    syscall
    lea numbuf(%rip), %rsi              /* up to the newline */
    xor %ecx, %ecx
4:  cmp %eax, %ecx
    jge 5f
    cmpb $10, (%rsi,%rcx)
    je 5f
    inc %ecx
    jmp 4b
5:  movb $0, (%rsi,%rcx)
    call puts
    mov %ebp, %edi
    mov $SYS_close, %eax
    syscall
2:  mov %rbx, %rsi                      /* next name */
6:  lodsb
    test %al, %al
    jnz 6b
    mov %rsi, %rbx
    jmp 1b
3:  call newline

    lea s_devkvm(%rip), %rdi
    mov $2, %esi                        /* O_RDWR */
    mov $SYS_open, %eax
    syscall
    lea s_open(%rip), %rsi
    test %eax, %eax
    js fail
    mov %eax, %r12d                     /* r12 = /dev/kvm */
    mov %r12d, %edi
    mov $KVM_GET_API_VERSION, %esi
    call ioctl0
    lea s_api(%rip), %rsi
    call puts
    call print_dec
    call newline

    mov %r12d, %edi
    mov $KVM_CREATE_VM, %esi
    call ioctl0
    lea s_createvm(%rip), %rsi
    test %eax, %eax
    js fail
    mov %eax, %r13d                     /* r13 = VM */
    mov %r13d, %edi                     /* PIC, I/O APIC and local APICs in the kernel */
    mov $KVM_CREATE_IRQCHIP, %esi
    call ioctl0
    lea s_irqchip(%rip), %rsi
    test %eax, %eax
    jnz fail

    xor %edi, %edi                      /* guest memory: 64 KiB anonymous */
    mov $0x10000, %esi
    mov $3, %edx                        /* PROT_READ | PROT_WRITE */
    mov $0x22, %r10d                    /* MAP_PRIVATE | MAP_ANONYMOUS */
    mov $-1, %r8
    xor %r9d, %r9d
    mov $SYS_mmap, %eax
    syscall
    mov %rax, %r14                      /* r14 = guest memory */
    lea guest(%rip), %rsi
    lea 0x1000(%r14), %rdi
    mov $(guest_end - guest), %ecx
    rep movsb
    lea region(%rip), %rdx
    mov %r14, 24(%rdx)                  /* userspace_addr */
    mov %r13d, %edi
    mov $KVM_SET_USER_MEMORY_REGION, %esi
    mov $SYS_ioctl, %eax
    syscall
    lea s_region(%rip), %rsi
    test %eax, %eax
    jnz fail

    mov %r13d, %edi
    mov $KVM_CREATE_VCPU, %esi
    call ioctl0
    lea s_vcpu(%rip), %rsi
    test %eax, %eax
    js fail
    mov %eax, %r15d                     /* r15 = vCPU */
    mov %r12d, %edi
    mov $KVM_GET_VCPU_MMAP_SIZE, %esi
    call ioctl0
    mov %rax, %rsi
    xor %edi, %edi
    mov $3, %edx
    mov $1, %r10d                       /* MAP_SHARED */
    mov %r15, %r8
    xor %r9d, %r9d
    mov $SYS_mmap, %eax
    syscall
    mov %rax, %rbx                      /* rbx = struct kvm_run */

    mov %r15d, %edi                     /* CS = 0 (base 0) */
    mov $KVM_GET_SREGS, %esi
    lea sregs(%rip), %rdx
    mov $SYS_ioctl, %eax
    syscall
    lea sregs(%rip), %rdx
    movq $0, 0(%rdx)
    movw $0, 12(%rdx)
    mov %r15d, %edi
    mov $KVM_SET_SREGS, %esi
    mov $SYS_ioctl, %eax
    syscall
    lea regs(%rip), %rdx                /* IP = 0x1000, AX = 2, BX = 3 */
    movq $2, 0(%rdx)
    movq $3, 8(%rdx)
    movq $0x1000, 128(%rdx)
    movq $2, 136(%rdx)
    mov %r15d, %edi
    mov $KVM_SET_REGS, %esi
    mov $SYS_ioctl, %eax
    syscall
    lea s_running(%rip), %rsi
    call puts

run:
    mov %r15d, %edi
    mov $KVM_RUN, %esi
    call ioctl0
    lea s_run(%rip), %rsi
    test %eax, %eax
    jnz fail
    mov 8(%rbx), %eax                   /* exit_reason */
    cmp $2, %eax
    je exit_io
    cmp $6, %eax
    je exit_mmio
    lea s_reason(%rip), %rsi
    call puts
    call print_dec
    call newline
    jmp reboot
exit_io:
    cmpb $1, 32(%rbx)                   /* KVM_EXIT_IO_OUT */
    jne run
    cmpw $0x501, 34(%rbx)
    je guest_done
    cmpw $0x3F8, 34(%rbx)
    jne run
    mov 40(%rbx), %rsi                  /* data_offset */
    add %rbx, %rsi
    mov $1, %edx
    mov $1, %edi
    mov $SYS_write, %eax
    syscall
    jmp run
exit_mmio:
    lea s_mmio(%rip), %rsi
    call puts
    mov 32(%rbx), %rax                  /* phys_addr */
    call print_hex
    call space
    movzbl 40(%rbx), %eax               /* data[0] */
    call print_hex
    call space
    movzbl 52(%rbx), %eax               /* is_write */
    call print_dec
    call newline
    jmp run
guest_done:
    incl rounds(%rip)
    cmpl $1, rounds(%rip)
    jne 1f
    lea s_done(%rip), %rsi
    call puts
    movzwl 0x1800(%r14), %eax           /* what the guest stored in its memory */
    call print_hex
    call newline
1:  lea dirtylog(%rip), %rdx            /* the pages the guest wrote */
    lea bitmap(%rip), %rax
    mov %rax, 8(%rdx)
    mov %r13d, %edi
    mov $KVM_GET_DIRTY_LOG, %esi
    mov $SYS_ioctl, %eax
    syscall
    lea s_dirtylog(%rip), %rsi
    test %eax, %eax
    jnz fail
    lea s_dirty(%rip), %rsi
    call puts
    mov bitmap(%rip), %rax
    call print_hex
    call newline
    cmpl $2, rounds(%rip)
    jne run
    jmp reboot

fail:                                   /* rsi = what failed, rax = -errno */
    call puts
    neg %eax
    call print_dec
    call newline
reboot:
    lea s_bye(%rip), %rsi
    call puts
    mov $0xfee1dead, %edi
    mov $0x28121969, %esi
    mov $0x01234567, %edx               /* LINUX_REBOOT_CMD_RESTART */
    mov $SYS_reboot, %eax
    syscall
    jmp .

/* rdi = module path: open, finit_module, report */
insmod:
    push %rdi
    xor %esi, %esi
    mov $SYS_open, %eax
    syscall
    mov %eax, %ebp
    mov %eax, %edi
    lea s_empty(%rip), %rsi
    xor %edx, %edx
    mov $SYS_finit_module, %eax
    syscall
    push %rax
    mov %ebp, %edi
    mov $SYS_close, %eax
    syscall
    lea s_insmod(%rip), %rsi
    call puts
    mov 8(%rsp), %rsi
    call puts
    call space
    pop %rax
    neg %eax
    call print_dec
    call newline
    pop %rdi
    ret

/* rdi = destination, rsi = NUL-terminated source; rdi ends at the NUL */
strcpy:
    lodsb
    mov %al, (%rdi)
    test %al, %al
    jz 1f
    inc %rdi
    jmp strcpy
1:  ret

ioctl0:
    xor %edx, %edx
    mov $SYS_ioctl, %eax
    syscall
    ret

/* rsi = NUL-terminated string; keeps rax */
puts:
    push %rax
    mov %rsi, %rdx
1:  cmpb $0, (%rdx)
    je 2f
    inc %rdx
    jmp 1b
2:  sub %rsi, %rdx
    mov $1, %edi
    mov $SYS_write, %eax
    syscall
    pop %rax
    ret
space:
    lea s_space(%rip), %rsi
    jmp puts
newline:
    lea s_nl(%rip), %rsi
    jmp puts

print_dec:                              /* eax, unsigned decimal */
    push %rax
    lea numbuf+15(%rip), %rsi
    movb $0, (%rsi)
    mov $10, %ecx
1:  xor %edx, %edx
    div %ecx
    add $'0', %dl
    dec %rsi
    mov %dl, (%rsi)
    test %eax, %eax
    jnz 1b
    call puts
    pop %rax
    ret

print_hex:                              /* rax */
    push %rax
    lea numbuf+17(%rip), %rsi
    movb $0, (%rsi)
    mov $16, %ecx
1:  mov %eax, %edx
    and $15, %edx
    lea hexdig(%rip), %rdi
    mov (%rdi,%rdx), %dl
    dec %rsi
    mov %dl, (%rsi)
    shr $4, %rax
    loop 1b
    call puts
    pop %rax
    ret

/* ---- the guest: real mode, loaded at 0000:1000; then 32-bit protected mode ---- */
#define G(x) ((x) - guest + 0x1000)
#define APIC 0xFEE00000
    .code16
guest:
    add %bl, %al                        /* 2 + 3 */
    add $'0', %al
    mov $0x3F8, %dx
    out %al, %dx
    mov $G(g_msg), %si
1:  lodsb
    test %al, %al
    jz 2f
    out %al, %dx
    jmp 1b
2:  lgdtl G(g_gdt_desc)
    mov %cr0, %eax
    or $1, %eax
    mov %eax, %cr0
    ljmpl $8, $G(g_pm)
    .code32
g_pm:
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    mov $0x8000, %esp
    lidt G(g_idt_desc)
    movl $0x1FF, APIC + 0xF0            /* software enable */
    movl $0xB, APIC + 0x3E0             /* divide by 1 */
    movl $0x40, APIC + 0x320            /* one-shot timer, vector 0x40 */
    movl $100000, APIC + 0x380
    sti
1:  hlt                                 /* until the timer interrupt */
    cmpl $0, G(g_ticks)
    je 1b
    movl $0x00044041, APIC + 0x300      /* self IPI, vector 0x41 */
1:  cmpl $0, G(g_ipis)
    je 1b
    cli
    movw $0x4B56, 0x1800                /* "VK" into guest memory */
    movb $0x5A, 0x20000                 /* no memory slot -> MMIO */
    mov $0x501, %dx                     /* done */
    out %al, %dx
    movw $0x4B57, 0x1800                /* again: a clean page, now through the PML */
    movb $1, 0x3000                     /* a new page */
    out %al, %dx
    hlt
g_timer:
    push %esi
    mov $G(g_msg_timer), %esi
    call g_puts
    incl G(g_ticks)
    movl $0, APIC + 0xB0                /* EOI */
    pop %esi
    iret
g_ipi:
    push %esi
    mov $G(g_msg_ipi), %esi
    call g_puts
    incl G(g_ipis)
    movl $0, APIC + 0xB0
    pop %esi
    iret
g_puts:
    push %eax
    push %edx
    mov $0x3F8, %dx
1:  lodsb
    test %al, %al
    jz 2f
    out %al, %dx
    jmp 1b
2:  pop %edx
    pop %eax
    ret
g_msg: .asciz " = 2 + 3, hello from a KVM guest on x86sim\n"
g_msg_timer: .asciz "guest: local APIC timer interrupt\n"
g_msg_ipi: .asciz "guest: self IPI\n"
    .p2align 2
g_ticks: .long 0
g_ipis: .long 0
    .p2align 3
g_gdt:
    .quad 0
    .quad 0x00CF9A000000FFFF
    .quad 0x00CF92000000FFFF
g_gdt_desc:
    .word 23
    .long G(g_gdt)
g_idt_desc:
    .word 0x42 * 8 - 1
    .long G(g_idt)
    .p2align 3
g_idt:
    .fill 0x40, 8, 0
    .word G(g_timer), 8, 0x8E00, 0
    .word G(g_ipi), 8, 0x8E00, 0
guest_end:
    .code64

banner: .asciz "\n*** x86sim: KVM test ***\n"
s_devkvm: .asciz "/dev/kvm"
m_irqbypass: .asciz "/irqbypass.ko"
m_kvm: .asciz "/kvm.ko"
m_kvm_intel: .asciz "/kvm-intel.ko"
s_insmod: .asciz "insmod "
s_empty: .asciz ""
s_open: .asciz "open /dev/kvm failed: "
s_api: .asciz "KVM API version "
s_createvm: .asciz "KVM_CREATE_VM failed: "
s_region: .asciz "KVM_SET_USER_MEMORY_REGION failed: "
s_vcpu: .asciz "KVM_CREATE_VCPU failed: "
s_run: .asciz "KVM_RUN failed: "
s_running: .asciz "running the guest\n"
s_reason: .asciz "unexpected exit reason "
s_mmio: .asciz "KVM_EXIT_MMIO "
s_done: .asciz "guest done, guest memory[0x1800] = "
s_bye: .asciz "rebooting\n"
s_irqchip: .asciz "KVM_CREATE_IRQCHIP failed: "
s_dirtylog: .asciz "KVM_GET_DIRTY_LOG failed: "
s_dirty: .asciz "dirty pages "
s_sys: .asciz "/sys"
s_sysfs: .asciz "sysfs"
s_params: .asciz "kvm_intel:"
s_eq: .asciz "="
s_parampath: .asciz "/sys/module/kvm_intel/parameters/"
params: .asciz "ept", "unrestricted_guest", "enable_apicv", "flexpriority", "vpid", "pml", "preemption_timer", "ple_gap", "enable_shadow_vmcs", "nested"
    .byte 0
s_space: .asciz " "
s_nl: .asciz "\n"
hexdig: .ascii "0123456789abcdef"
    .p2align 3
region: .long 0, 1                      /* slot, flags: KVM_MEM_LOG_DIRTY_PAGES */
    .quad 0, 0x10000, 0                 /* guest_phys_addr, memory_size, userspace_addr */
numbuf: .space 24
dirtylog: .long 0, 0                    /* slot, padding, bitmap pointer */
    .quad 0
bitmap: .quad 0
rounds: .long 0
pathbuf: .space 128
sregs: .space 320
regs: .space 144
