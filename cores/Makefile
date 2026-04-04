CC      = gcc
OBJCOPY = objcopy

CFLAGS  = -Os -ffreestanding -fno-stack-protector -fno-builtin \
          -fpie -mno-red-zone -fomit-frame-pointer -fcf-protection=none \
          -fno-exceptions -fno-unwind-tables -fno-asynchronous-unwind-tables \
          -Wall -Wno-unused-function -I../src

# Use linker_core.ld (ENTRY=_core_start, base=0) so stored function
# addresses equal their byte offsets – the frontend adds the mapped base
# address at load time.
LDFLAGS = -T linker_core.ld -nostdlib -nostartfiles -static \
          -Wl,--build-id=none -Wl,--no-dynamic-linker -Wl,-z,norelro -no-pie

# NES core: nes_core.c wraps the EmuC NES emulator behind the libretro API.
# Each source file is compiled separately; object names are kept flat (no
# subdirectory prefix) to avoid path issues at link time.
NES_SRCS = nes_core.c ../src/bus.c ../src/cpu.c ../src/ppu.c ../src/apu.c
NES_OBJS = nes_core.o bus.o cpu.o ppu.o apu.o

# ── Targets ────────────────────────────────────────────────────────────────

.PHONY: all nes snes gb gba md clean

all: nes_core.bin

# ── NES ────────────────────────────────────────────────────────────────────

nes: nes_core.bin

nes_core.bin: nes_core.elf
	$(OBJCOPY) -O binary $< $@
	@echo "Built: $@ ($$(wc -c < $@) bytes)"

nes_core.elf: $(NES_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(NES_OBJS)

# Compile nes_core.c from this directory (no path prefix needed).
nes_core.o: nes_core.c
	$(CC) $(CFLAGS) -c $< -o $@

# Compile shared emulator sources from the parent src/ directory.
# The object file names are kept flat so the linker line above is simple.
bus.o: ../src/bus.c
	$(CC) $(CFLAGS) -c $< -o $@

cpu.o: ../src/cpu.c
	$(CC) $(CFLAGS) -c $< -o $@

ppu.o: ../src/ppu.c
	$(CC) $(CFLAGS) -c $< -o $@

apu.o: ../src/apu.c
	$(CC) $(CFLAGS) -c $< -o $@

# ── Stub targets (not yet ported) ──────────────────────────────────────────

snes:
	@echo "NOTE: snes9x not yet ported – stub target only."
	@echo "      See cores/build_cores.sh to build the upstream libretro .so."

gb:
	@echo "NOTE: gambatte not yet ported – stub target only."
	@echo "      See cores/build_cores.sh to build the upstream libretro .so."

gba:
	@echo "NOTE: mGBA not yet ported – stub target only."
	@echo "      See cores/build_cores.sh to build the upstream libretro .so."

md:
	@echo "NOTE: Genesis Plus GX not yet ported – stub target only."
	@echo "      See cores/build_cores.sh to build the upstream libretro .so."

# ── Clean ──────────────────────────────────────────────────────────────────

clean:
	rm -f *.o *.elf *.bin
