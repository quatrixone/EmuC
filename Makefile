CC      = gcc
OBJCOPY = objcopy

CFLAGS  = -Os -ffreestanding -fno-stack-protector -fno-builtin \
          -fpie -mno-red-zone -fomit-frame-pointer -fcf-protection=none \
          -fno-exceptions -fno-unwind-tables -fno-asynchronous-unwind-tables \
          -Wall -Wno-unused-function -Isrc

LDFLAGS = -T linker.ld -nostdlib -nostartfiles -static \
          -Wl,--build-id=none -Wl,--no-dynamic-linker -Wl,-z,norelro -no-pie

SRCS    = src/main.c src/bus.c src/cpu.c src/ppu.c src/apu.c src/ftp.c
OBJS    = $(SRCS:.c=.o)
TARGET  = nes_emu

all: $(TARGET).bin

%.o: %.c src/core.h src/nes.h src/tables.h src/ftp.h
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET).elf: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS)

$(TARGET).bin: $(TARGET).elf
	$(OBJCOPY) -O binary $< $@
	@echo "Built: $@ ($$(wc -c < $@) bytes)"

clean:
	rm -f src/*.o $(TARGET).elf $(TARGET).bin

.PHONY: all clean