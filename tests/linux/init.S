/* A minimal /init for booting Linux in x86sim: prints a banner, uname and
 * the start of /proc/cpuinfo, then echoes input lines until "exit", and
 * reboots (the 8042 reset ends the simulation). Position independent and
 * relocation-free: mkinitramfs.py wraps .text into a static ELF. */

#define SYS_read 0
#define SYS_write 1
#define SYS_open 2
#define SYS_close 3
#define SYS_mount 165
#define SYS_reboot 169
#define SYS_uname 63

    .text
    .globl _start
_start:
    lea banner(%rip), %rsi
    call puts

    lea uts(%rip), %rdi
    mov $SYS_uname, %eax
    syscall
    lea uts(%rip), %rsi          /* sysname */
    call puts
    call space
    lea uts+130(%rip), %rsi      /* release */
    call puts
    call space
    lea uts+260(%rip), %rsi      /* machine */
    call puts
    call newline

    lea s_proc(%rip), %rdi       /* mount("proc", "/proc", "proc", 0, 0) */
    lea s_procdir(%rip), %rsi
    mov %rdi, %rdx
    xor %r10d, %r10d
    xor %r8d, %r8d
    mov $SYS_mount, %eax
    syscall
    lea s_cpuinfo(%rip), %rdi
    xor %esi, %esi
    mov $SYS_open, %eax
    syscall
    test %eax, %eax
    js 1f
    mov %eax, %ebx
    mov %eax, %edi
    lea buf(%rip), %rsi
    mov $160, %edx
    mov $SYS_read, %eax
    syscall
    mov %eax, %edx
    lea buf(%rip), %rsi
    mov $1, %edi
    mov $SYS_write, %eax
    syscall
    call newline
    mov %ebx, %edi
    mov $SYS_close, %eax
    syscall
1:
prompt:
    lea s_prompt(%rip), %rsi
    call puts
    xor %edi, %edi
    lea buf(%rip), %rsi
    mov $200, %edx
    mov $SYS_read, %eax
    syscall
    test %eax, %eax
    jle reboot
    mov %eax, %r12d
    cmpl $0x74697865, buf(%rip)  /* "exit" */
    je reboot
    lea s_echo(%rip), %rsi
    call puts
    mov $1, %edi
    lea buf(%rip), %rsi
    mov %r12d, %edx
    mov $SYS_write, %eax
    syscall
    jmp prompt

reboot:
    lea s_bye(%rip), %rsi
    call puts
    mov $0xfee1dead, %edi
    mov $0x28121969, %esi
    mov $0x01234567, %edx        /* LINUX_REBOOT_CMD_RESTART */
    mov $SYS_reboot, %eax
    syscall
    jmp .

/* rsi = NUL-terminated string */
puts:
    mov %rsi, %rdx
1:  cmpb $0, (%rdx)
    je 2f
    inc %rdx
    jmp 1b
2:  sub %rsi, %rdx
    mov $1, %edi
    mov $SYS_write, %eax
    syscall
    ret
space:
    lea s_space(%rip), %rsi
    jmp puts
newline:
    lea s_nl(%rip), %rsi
    jmp puts

banner: .asciz "\n*** x86sim: hello from /init ***\n"
s_space: .asciz " "
s_nl: .asciz "\n"
s_proc: .asciz "proc"
s_procdir: .asciz "/proc"
s_cpuinfo: .asciz "/proc/cpuinfo"
s_prompt: .asciz "init> "
s_echo: .asciz "you typed: "
s_bye: .asciz "rebooting\n"
    .p2align 4
uts: .space 390
buf: .space 256
