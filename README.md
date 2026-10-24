# x86sim

An x86-64 CPU simulator written in C, with a web-based step debugger and a
terminal debugger. It runs compiled programs in 16-bit real mode (DOS-style)
and 32/64-bit Linux user mode, and emulates a full PC — firmware, interrupt
controllers, timers, serial port, keyboard, VGA text, IDE disk — that boots a
stock Linux kernel on up to 255 CPUs, optionally through a simulated MESI
cache hierarchy.

```
make                       # build ./x86sim (no dependencies beyond a C compiler)
make test                  # differential test suite (needs Rosetta on Apple Silicon)

./x86cc --run examples/greet.c          # compile C for the simulator and run it
./x86cc --ui  tests/c/algo.c            # ...or open it in the web debugger
./x86cc --run --16 examples/hello16.s   # 16-bit real-mode DOS program

./x86sim -u --smp 4 --cache   # empty 4-CPU PC: write assembly in the browser, load it on any CPU

./x86sim --kernel bzImage --initrd initrd.cpio          # boot Linux on the serial console
./x86sim --smp 2 --cache-stats --kernel bzImage ...     # two CPUs, L1/L2 caches, MESI
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
| `-m`, `--max N` | stop after N instructions (in PC mode: rounds, one instruction per CPU) |
| `--pc` | full-system PC: the program is a boot image started in real mode |
| `--ram MB` | PC memory size (default 256) |
| `--hda F`, `--fda F`, `--boot a\|c` | disk images and boot drive |
| `--kernel F`, `--initrd F`, `--append S` | boot a Linux bzImage (32-bit boot protocol) |
| `--smp N` | N CPUs (1-255) |
| `--cache` | simulate the cache hierarchy; `--cache-stats` also prints statistics at exit |

The options from `--pc` down imply PC mode. There the terminal is the serial
port and keyboard (Ctrl-] quits), port 0xE9 is a debug console and writing a
byte to port 0xF4 exits with that code.

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
- In PC mode: a **Screen** tab (VGA text and mode 13h; click it and type to
  send keys), control registers and the operating mode in the header, a **CPU
  switcher** (registers, disassembly, stack and memory follow the selected CPU;
  buttons up to 8 CPUs, a drop-down with a running/halted summary beyond)
  and, with `--cache`, a **Caches** tab: hit rates, MESI line counts per
  cache, bus traffic, the state of any line in every cache, a live log of
  coherence events, and the **contents** of any cache: a sets × ways grid
  of line addresses coloured by MESI state, marking each set's most recently
  used line and its next victim and flashing lines that just changed. It
  refreshes while the program runs, optionally in slow motion (10 to 100k
  instructions per frame). The L2 adds an overview map of all 4096 sets.
  Clicking a line shows its 64 bytes, highlighting bytes that differ from
  main memory, i.e. written but not yet written back.

The server binds to 127.0.0.1 only.

## Assembly editor

The disassembly panel has an **Editor** view for writing bare-metal programs
and loading them onto any CPU of a PC machine. `x86sim -u` without a program
(add `--smp N`, `--cache`) starts an empty PC and opens the editor; **New
machine** in the editor replaces the current machine with an empty one of any
size.

- Choose the **mode** (16-bit real, 32-bit protected, 64-bit long), the
  **syntax** (AT&T, or Intel via `.intel_syntax noprefix`), the **CPU** and
  the load address, then **Load** (ready to step, in the disassembly view)
  or **Load & Run** (Ctrl+Enter). **Assemble** only checks for errors, which
  are marked in the gutter; click one to jump to it.
- Programs are assembled by `clang` (the toolchain the project already uses;
  set `X86SIM_CLANG` to use another) and linked by the simulator's own linker.
  Entry is `_start`, or the first instruction.
- Every CPU has a default slot, with code at the bottom and the stack at the
  top: 32/64-bit at 0x200000 + cpu × 64 KiB, 16-bit at 0x10000 + cpu × 4 KiB.
  A load is refused if its code and stack would overlap another loaded
  program (reloading the same CPU replaces its old program). Nothing stops a
  running program from writing elsewhere in memory: like real bare metal,
  every CPU sees all of physical memory.
  The CPU starts directly in the chosen mode, with interrupts off:
  - 16-bit real mode: CS = DS = ES = SS = address / 16, BIOS services available.
  - 32-bit protected mode: flat segments (code 0x08, data 0x10).
  - 64-bit long mode: code segment 0x18, the first 4 GiB identity-mapped
    with 2 MiB pages.
  - The GDT, IDTs and page tables live at 0x100000–0x10FFFF.
- Until a program installs its own IDT, an exception stops the run with a
  report such as `cpu1: unhandled #UD invalid opcode at 0x210004` and halts
  only that CPU.
- Port 0xE9 prints to the Console; the CPUs share memory, so programs on
  different CPUs can communicate. `cli; hlt` stops a CPU, and the run ends
  when every CPU has stopped.
- **Memory only** writes the code without starting a CPU, for example as the
  target of your own INIT/STARTUP IPIs.
- Reset restarts the machine and reloads every editor program. The draft is
  kept in the browser.
- Examples: hello in each mode and in Intel syntax, "which CPU am I?",
  a shared counter with `LOCK` and a spinlock (load it on several CPUs and
  watch the line in the Caches tab), and an exception.

## Multiprocessor and caches

`--smp N` gives the PC N CPUs, from 1 to 255. CPU 0 boots; the others wait in the
wait-for-SIPI state until the bootstrap processor sends INIT and STARTUP
IPIs, exactly as on real hardware, and the ACPI MADT and MP table list them
all. Each CPU has its own local APIC (APIC ID = CPU number) with fixed,
lowest-priority, NMI, INIT and STARTUP IPIs to physical or logical (flat and
cluster) destinations; the I/O APIC routes device interrupts to any of them.
CPUs execute in a deterministic round robin, one instruction each per round,
so LOCK-prefixed instructions are atomic and runs are reproducible, while
plain read-modify-write sequences still race (see `tests/sys/smp.S`).
CPUID reports the topology (leaf 1, leaf 0xB) and the caches (leaf 4).

The limit of 255 is the xAPIC architecture's: APIC IDs are 8 bits and 0xFF
means broadcast. Flat logical destinations address at most 8 CPUs and
cluster mode 60 (15 clusters of 4), so larger systems use physical
destinations; going further would need x2APIC (32-bit IDs) and interrupt
remapping. Each CPU costs about 30 KiB, plus 80 KiB of L1 with `--cache`.
Simulation time grows linearly with the number of running CPUs (halted ones
are cheap): Linux boots on 8 CPUs in about 45 s and on 64 in about 3 minutes.
`tests/sys/smpmany.S` wakes all APs at once with a broadcast INIT/STARTUP and
checks their APIC IDs, an atomic counter and a broadcast IPI.

`--cache` puts every memory access through a simulated hierarchy:

| | per CPU | shared |
|---|---|---|
| L1 data | 48 KiB, 12-way, 64 sets | |
| L1 instruction | 32 KiB, 8-way, 64 sets | |
| L2 | | 3 MiB, 12-way, 4096 sets, inclusive |

All lines are 64 bytes, write-back, write-allocate, LRU. The caches hold the
data — memory is only updated on writeback — so a coherence bug would corrupt
the guest. The L1s stay coherent with **MESI** on a snooping bus:

- **BusRd** (read miss): a Modified copy elsewhere is flushed to the L2
  (cache-to-cache intervention); all copies become Shared. The requester
  gets Exclusive when nobody else has the line.
- **BusRdX** (write miss): other copies are flushed if Modified and
  invalidated; the requester gets Modified.
- **BusUpgr** (write hit on Shared): other copies are invalidated without a
  data transfer. A write hit on Exclusive becomes Modified silently.
- **BusWB**: a Modified line evicted from an L1 goes to the L2. Evicting an L2
  line back-invalidates the L1 copies (inclusion).

L1I lines count as sharers, so a store invalidates stale code in every
instruction cache. CLFLUSH/CLFLUSHOPT write a line back and drop it
everywhere; WBINVD and INVD flush everything. Legacy video memory, the BIOS
ROM and MMIO are uncached. A guest can query a line's states through port
0xEC (write the physical address as a dword, read back two bits per cache:
cpu first+k's L1D in bits 4k–4k+1, its L1I in 4k+2–4k+3, the L2 in bits
16–17). Four cores fit in a reply; writing a byte to port 0xED selects the
first core (default 0). With many sharers the event log names four snooped
copies and counts the rest.

The terminal debugger has `cpu [n]`, `cache [addr]` and `cachelog [n]`.

## Architecture

```
src/decode.[ch]   table-driven decoder: legacy/REX/VEX/EVEX prefixes, ModRM/SIB,
                  1-byte, 0F, 0F38, 0F3A maps, groups, mandatory-prefix tables
src/disasm.c      Intel-syntax disassembler (driven by the same tables)
src/cpu.[ch]      architectural state + integer/system execution unit
src/cpu_internal.h  helpers shared by execution units
src/sse.c         SSE..SSE4.2, AES-NI, PCLMULQDQ, SHA execution unit
src/fpu.c, softfloat.[ch]  x87 (80-bit, correctly rounded), FXSAVE/XSAVE
src/system.c      segmentation, privilege levels, gates, task switches, VM86,
                  interrupt/exception delivery, SYSCALL/SYSENTER, MSRs, CRs/DRs
src/mmu.c         paging (32-bit, PAE, 4/5-level), PCID TLB, SMEP/SMAP/PKU,
                  CET shadow-stack pages
src/cet.c         CET shadow stacks and indirect branch tracking
src/cache.[ch]    L1/L2 cache hierarchy with MESI coherence
src/pmu.c         performance counters (architectural perfmon v4), RDPMC, PMIs
src/pc.c          PC machine: clock, 8259 PIC, 8254 PIT, CMOS/RTC, 8042, 16550,
                  VGA, PCI, IDE; SMP bring-up
src/apic.c        local APICs (timer, IPIs) and the I/O APIC
src/bios.c        HLE BIOS services, ACPI/MP tables, disk and Linux boot
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
RIP on the faulting instruction. Time is virtual (one instruction round =
1 ns, skipping ahead while every CPU is halted), so runs are deterministic.

## Testing

`tests/run_tests.sh` compiles each C test natively for x86-64 macOS (run under
Rosetta as the reference) and as freestanding Linux objects in five
configurations (x86-64 -O0/-O2/-O2 PIE, i386 -O0/-O2), runs them in the
simulator, and requires byte-identical output and exit codes. `tests/c/flags.c`
executes individual ALU/shift/rotate instructions at every operand width over
edge-case values and compares every architecturally defined flag.

- `tests/sys/run.sh` — bare-metal programs booted in PC mode: protected and
  long mode, paging, task switches, VM86, BIOS services and devices, disk and
  floppy boot, CET, SMP bring-up with IPIs and shared counters, the MESI
  transitions of one line (`smpcache`), 32 CPUs started at once
  (`smpmany`), the performance counters with an overflow interrupt
  (`pmu`), and self- and cross-modifying code with and without the caches
  (`smc`, `smcnocache`).
- `tests/asm/run.sh` — the assembly editor's API: every example in each
  mode, multi-CPU loads, reset replay, diagnostics and refused addresses
  (including overlapping programs).
- `tests/softfloat/run.sh` — x87 arithmetic and transcendentals against
  reference results.
- `tests/linux/run.sh` — boots Alpine's Linux 6.6 kernel (downloaded once) on
  one CPU, on two CPUs, and on two and eight CPUs with the caches; types a line into
  `/init` and checks the echo and the clean reboot.

## Status

Implemented:

- 16/32/64-bit decoding of all encodings (VEX/EVEX decoded for length)
- The general-purpose integer instruction set, ADX, MOVBE, CRC32
- x87, MMX, SSE through SSE4.2, AES-NI, PCLMULQDQ, SHA
- System architecture: real/protected/long/VM86 modes, paging with PCID,
  SMEP/SMAP, protection keys, hardware task switching, SYSCALL/SYSENTER,
  XSAVE, FSGSBASE, UMIP, LA57, CET (shadow stacks, IBT)
- Performance counters: architectural perfmon v4 (3 fixed + 8 general 48-bit
  counters, global control/status, RDPMC, overflow interrupts as a vector or
  NMI). Events: instructions, cycles (one per instruction), branches, and
  with `--cache` L1D/L1I/L2 misses and cross-core modified-line transfers.
  The simulator models behavior, not microarchitecture: there is no branch
  predictor or pipeline timing, and the LBR MSRs hold values but record nothing
- A PC that boots disks, floppies and Linux, with SMP (up to 255 CPUs) and a
  MESI cache hierarchy
- Linux user mode (≈45 syscalls), DOS int 21h and BIOS services

Not yet: AVX/AVX2/FMA and AVX-512, BMI1/2, VMX. Anything not implemented
stops with a clear `instruction not implemented yet: ...` message rather than
misbehaving.
