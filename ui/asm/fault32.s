# Exceptions. Until you install your own IDT, every vector goes to a
# handler that reports the exception, stops the run and halts the CPU.
# Run this to get a divide error; then try "ud2" or "int3" instead,
# or install your own handler with LIDT.
    .globl _start
_start:
    mov $10, %eax
    xor %ecx, %ecx
    xor %edx, %edx
    div %ecx                # 10 / 0: #DE
    cli
    hlt
