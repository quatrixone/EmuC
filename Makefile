CC      = gcc
OBJCOPY = objcopy

CFLAGS  = -Os -ffreestanding -fno-stack-protector -fno-builtin \
          -fpie -mno-red-zone -fomit-frame-pointer -fcf-protection=none \
          -fno-exceptions -fno-unwind-tables -fno-asynchronous-unwind-tables \
          -Wall -Wno-unused-function -Isrc

LDFLAGS = -T linker.ld -nostdlib -nostartfiles -static \
          -Wl,--build-id=none -Wl,--no-dynamic-linker -Wl,-z,norelro -no-pie

# ── NES standalone emulator (original) ─────────────────────────────────────
NES_SRCS  = src/main.c src/bus.c src/cpu.c src/ppu.c src/apu.c src/ftp.c
NES_OBJS  = $(NES_SRCS:.c=.o)
NES_TARGET = nes_emu

# ── EmulationStation frontend (multi-system libretro frontend) ──────────────
ES_SRCS   = src/retro_main.c src/bus.c src/cpu.c src/ppu.c src/apu.c src/ftp.c
ES_OBJS   = src/retro_main.o src/bus.o src/cpu.o src/ppu.o src/apu.o src/ftp.o
ES_TARGET = es_frontend

.PHONY: all nes es cores clean

all: nes es

# ── NES emulator ────────────────────────────────────────────────────────────
nes: $(NES_TARGET).bin

$(NES_TARGET).bin: $(NES_TARGET).elf
	$(OBJCOPY) -O binary $< $@
	@echo "Built: $@ ($$(wc -c < $@) bytes)"

$(NES_TARGET).elf: $(NES_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(NES_OBJS)

# ── EmulationStation frontend ────────────────────────────────────────────────
es: $(ES_TARGET).bin

$(ES_TARGET).bin: $(ES_TARGET).elf
	$(OBJCOPY) -O binary $< $@
	@echo "Built: $@ ($$(wc -c < $@) bytes)"
	@echo "Run:   python3 gen_lua.py $(ES_TARGET).bin es.lua"

$(ES_TARGET).elf: $(ES_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(ES_OBJS)

# Separate rule for retro_main.o so it doesn't conflict with main.o
src/retro_main.o: src/retro_main.c src/core.h src/nes.h src/tables.h src/ftp.h src/libretro.h
	$(CC) $(CFLAGS) -c $< -o $@

# Generic rule for shared object files
%.o: %.c src/core.h src/nes.h src/tables.h src/ftp.h
	$(CC) $(CFLAGS) -c $< -o $@

# ── Libretro cores ────────────────────────────────────────────────────────────
cores:
	$(MAKE) -C cores nes

# ── Cleanup ───────────────────────────────────────────────────────────────────
clean:
	rm -f src/*.o $(NES_TARGET).elf $(NES_TARGET).bin \
	              $(ES_TARGET).elf  $(ES_TARGET).bin
	$(MAKE) -C cores clean 2>/dev/null || true
