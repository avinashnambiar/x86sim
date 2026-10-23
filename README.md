# x86sim

An x86 CPU simulator written in C, with a web-based step debugger and a
terminal debugger. It runs real compiled programs in 16-bit real mode
(DOS-style), 32-bit and 64-bit Linux user mode, and is being built towards a
complete modern x86-64 implementation.

```
make                       # build ./x86sim (no dependencies beyond a C compiler)
make test                  # differential test suite (needs Rosetta on Apple Silicon)

./x86cc --run examples/greet.c          # compile C for the simulator and run it
./x86cc --ui  tests/c/algo.c            # ...or open it in the web debugger
./x86cc --run --16 examples/hello16.s   # 16-bit real-mode DOS program
```

## Running programs

`x86sim [options] <program> [more.o ...] [-- program args]`

The program can be:

- **ELF relocatable objects (`.o`)** — linked in-process by a built-in linker,
  so `clang -target x86_64-linux-gnu -c` (or `i386-linux-gnu`) is all you need;
  no cross-linker required. Entry point is `_start`, or `main` via a generated stub.
- **Static ELF executables** (32/64-bit, including static-pie) — e.g. musl
  `-static` binaries. Dynamically linked executables are rejected.
- **Flat binaries** — loaded as a DOS `.COM` image at `1000:0100` in real mode.

| option | |
|---|---|
| `-u`, `--ui` | web debugger at http://127.0.0.1:8086 (`-p` to change port) |
| `-d`, `--debug` | terminal debugger (`help` lists commands) |
| `-b`, `--bits 16\|32\|64` | force the CPU mode |
| `-t`, `--trace` | print every instruction executed |
| `-s`, `--strace` | print system calls |
| `-m`, `--max N` | stop after N instructions |

`x86cc` wraps clang with the right flags and links the tiny runtime in
`runtime/` (`print_str`, `print_u64`, `sim_read`, `memcpy`, ...).

## Web debugger

- **Disassembly** follows RIP; click the gutter (or double-click a line) to
  toggle a breakpoint, click a jump target to navigate, type an address/symbol
  to browse.
- **Step** (F11 / S), **Step over** (F10 / N), **Step out** (Shift+F11 / F),
  **Run** (F5 / C), **Pause** (F6), **Reset** (Ctrl+R), **Load** another program.
- **Registers** highlight what the last step changed; click a register to edit
  it, click a flag to toggle it. XMM registers on demand.
- **Stack** view with return addresses resolved to `function+offset`.
- **Memory** hex view; accepts hex, symbols, registers and `reg+off`.
- **Console** shows program output and takes stdin — the program pauses in
  `read()` until you type a line.
- **System calls**, **Trace** (last executed instructions), **Memory map**,
  **Symbols** tabs.

The server binds to 127.0.0.1 only.

## Architecture

```
src/decode.[ch]   table-driven decoder: legacy/REX/VEX/EVEX prefixes, ModRM/SIB,
                  1-byte, 0F, 0F38, 0F3A maps, groups, mandatory-prefix tables
src/disasm.c      Intel-syntax disassembler (driven by the same tables)
src/cpu.[ch]      architectural state + integer/system execution unit
src/cpu_internal.h  helpers shared by execution units
src/sse.c         SSE/SSE2 execution unit
src/mem.[ch]      sparse 64-bit paged memory with R/W/X protection, lazy pages
src/machine.[ch]  machine = CPU + memory + program + debugger state (breakpoints,
                  step/over/out, trace, symbols)
src/loader.c      ELF executables, in-process .o linker, flat binaries, stacks/auxv
src/os.c          Linux syscalls (x86-64 `syscall`, i386 `int 0x80`), DOS/BIOS services
src/server.c      HTTP + JSON API for the web UI
src/main.c        command line and terminal debugger
ui/               web debugger (plain HTML/CSS/JS, no build step)
runtime/          minimal libc for guest programs
tests/            differential test suite
```

Exceptions are precise: a fault aborts the instruction via `longjmp`, leaving
RIP on the faulting instruction. `rdtsc` returns the instruction count, so
runs are deterministic.

## Testing

`tests/run_tests.sh` compiles each C test natively for x86-64 macOS (run under
Rosetta as the reference) and as freestanding Linux objects in five
configurations (x86-64 -O0/-O2/-O2 PIE, i386 -O0/-O2), runs them in the
simulator, and requires byte-identical output and exit codes. `tests/c/flags.c`
executes individual ALU/shift/rotate instructions at every operand width over
edge-case values and compares every architecturally defined flag.

## Status

Implemented:

- 16/32/64-bit decoding of all encodings (VEX/EVEX decoded for length)
- General-purpose integer instruction set: arithmetic, logic, shifts/rotates
  (incl. RCL/RCR, SHLD/SHRD), multiply/divide, bit ops (BT*, BSF/BSR, TZCNT,
  LZCNT, POPCNT, BSWAP), string ops with REP prefixes, CMPXCHG/8B/16B, XADD,
  control flow, BCD, CPUID, RDTSC/RDTSCP, RDRAND/RDSEED, XGETBV
- Real mode: segmentation, far calls/jumps, IVT interrupts, IRET
- SSE/SSE2: all moves, logic, the full packed-integer set, shuffles/unpacks
- Linux user mode (≈45 syscalls incl. mmap/brk/arch_prctl), DOS int 21h and
  BIOS int 10h/16h/1Ah

Roadmap:

1. **FPU and SIMD floating point** — x87 (80-bit), SSE/SSE2 FP, SSE3/SSSE3/SSE4.x
2. **AVX/AVX2/FMA**, BMI1/2, then **AVX-512** (opmask registers, EVEX)
3. **System architecture** — protected mode with GDT/IDT, paging (4/5-level),
   privilege levels, exceptions/interrupt delivery, long-mode entry, MSRs
4. **Machine** — firmware, PIC/APIC, PIT, serial, keyboard, disk, to boot an OS
5. **Extensions** — VMX/SVM, CET, MPK, AES-NI/SHA/AMX
6. Performance — decoded-instruction cache, lazy flags

Anything not yet implemented stops with a clear
`instruction not implemented yet: ...` message rather than misbehaving.
