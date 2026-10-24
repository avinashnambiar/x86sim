CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
CFLAGS  += -DUI_DIR='"$(CURDIR)/ui"'

SRCS := src/mem.c src/decode.c src/disasm.c src/cpu.c src/sse.c src/fpu.c src/softfloat.c src/mmu.c src/system.c src/pc.c src/apic.c src/bios.c src/cet.c src/cache.c src/pmu.c src/machine.c src/loader.c src/asm.c src/os.c src/server.c src/main.c
OBJS := $(SRCS:src/%.c=build/%.o)

x86sim: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

build/%.o: src/%.c src/*.h | build
	$(CC) $(CFLAGS) -c -o $@ $<

build:
	mkdir -p build

test: x86sim
	./tests/run_tests.sh

clean:
	rm -rf build x86sim tests/out

.PHONY: test clean
