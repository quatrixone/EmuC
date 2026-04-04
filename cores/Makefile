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

.PHONY: all nes snes gb gba md ps1 ps2 clean

all: nes gb gba snes ps1 ps2

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

# ── GB (self-contained stub) ────────────────────────────────────────────────

gb: gb_core.bin

gb_core.bin: gb_core.elf
	$(OBJCOPY) -O binary $< $@
	@echo "Built: $@ ($$(wc -c < $@) bytes)"

gb_core.elf: gb_core.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ gb_core.o

gb_core.o: gb_core.c
	$(CC) $(CFLAGS) -c $< -o $@

# ── GBA (self-contained stub) ───────────────────────────────────────────────

gba: gba_core.bin

gba_core.bin: gba_core.elf
	$(OBJCOPY) -O binary $< $@
	@echo "Built: $@ ($$(wc -c < $@) bytes)"

gba_core.elf: gba_core.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ gba_core.o

gba_core.o: gba_core.c
	$(CC) $(CFLAGS) -c $< -o $@

# ── SNES (self-contained stub) ──────────────────────────────────────────────

snes: snes_core.bin

snes_core.bin: snes_core.elf
	$(OBJCOPY) -O binary $< $@
	@echo "Built: $@ ($$(wc -c < $@) bytes)"

snes_core.elf: snes_core.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ snes_core.o

snes_core.o: snes_core.c
	$(CC) $(CFLAGS) -c $< -o $@

# ── PS1 (self-contained stub) ───────────────────────────────────────────────

ps1: ps1_core.bin

ps1_core.bin: ps1_core.elf
	$(OBJCOPY) -O binary $< $@
	@echo "Built: $@ ($$(wc -c < $@) bytes)"

ps1_core.elf: ps1_core.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ ps1_core.o

ps1_core.o: ps1_core.c
	$(CC) $(CFLAGS) -c $< -o $@

# ── PS2 (self-contained stub) ───────────────────────────────────────────────

ps2: ps2_core.bin

ps2_core.bin: ps2_core.elf
	$(OBJCOPY) -O binary $< $@
	@echo "Built: $@ ($$(wc -c < $@) bytes)"

ps2_core.elf: ps2_core.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ ps2_core.o

ps2_core.o: ps2_core.c
	$(CC) $(CFLAGS) -c $< -o $@

# ── Stub target (not yet ported) ────────────────────────────────────────────

md:
	@echo "NOTE: Genesis Plus GX not yet ported – stub target only."
	@echo "      See cores/build_cores.sh to build the upstream libretro .so."

# ── Clean ──────────────────────────────────────────────────────────────────

clean:
	rm -f *.o *.elf *.bin
