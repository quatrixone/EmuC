#!/bin/bash
# build_retroarch.sh  –  Clone, patch and compile RetroArch for PS5
# Phase 2 of the EmuC EmulationStation port.
#
# Usage:
#   ./build_retroarch.sh [--skip-clone] [--clean]
#
# What this does:
#   1. Clones RetroArch (shallow) from GitHub
#   2. Applies PS5-specific patches on top of the orbis (PS4) platform
#   3. Attempts to build with -ffreestanding flags matching our shellcode
#   4. If successful: converts output to flat .bin via objcopy
#   5. Run gen_lua.py to embed the binary in es.lua
#
# Requirements:
#   apt install gcc g++ make git wget libssl-dev
#   (RetroArch also needs: libfreetype6-dev libzstd-dev zlib1g-dev)

set -euo pipefail

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
ok()   { echo -e "${GREEN}[OK]${NC}  $*"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $*"; }
err()  { echo -e "${RED}[ERR]${NC}  $*"; }
info() { echo -e "${CYAN}[>>]${NC}  $*"; }

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RA_DIR="$SCRIPT_DIR/retroarch_src"
RA_REPO="https://github.com/libretro/RetroArch"

SKIP_CLONE=0
DO_CLEAN=0

for arg in "$@"; do
  case $arg in
    --skip-clone) SKIP_CLONE=1 ;;
    --clean)      DO_CLEAN=1 ;;
  esac
done

echo ""
echo "═══════════════════════════════════════════════════════"
echo "  RetroArch → PS5 Port Builder"
echo "  EgyDevTeam EmulationStation PS5"
echo "═══════════════════════════════════════════════════════"
echo ""

# ── Step 1: Dependencies ────────────────────────────────────────────────────
info "Checking dependencies..."
for cmd in gcc g++ make git objcopy python3; do
  if command -v "$cmd" &>/dev/null; then
    ok "$cmd found: $(command -v $cmd)"
  else
    err "$cmd not found. Install with: apt install build-essential git"
    exit 1
  fi
done

# ── Step 2: Clone RetroArch ─────────────────────────────────────────────────
if [ "$SKIP_CLONE" -eq 0 ]; then
  if [ -d "$RA_DIR/.git" ]; then
    warn "RetroArch already cloned at $RA_DIR (use --skip-clone to reuse)"
    info "Pulling latest changes..."
    git -C "$RA_DIR" pull --depth=1 origin master || warn "Pull failed, using existing clone"
  else
    info "Cloning RetroArch (shallow)..."
    git clone --depth=1 "$RA_REPO" "$RA_DIR"
    ok "Cloned RetroArch to $RA_DIR"
  fi
else
  if [ ! -d "$RA_DIR" ]; then
    err "RetroArch source not found at $RA_DIR. Remove --skip-clone."
    exit 1
  fi
  ok "Using existing clone at $RA_DIR"
fi

if [ "$DO_CLEAN" -eq 1 ]; then
  info "Cleaning previous build..."
  make -C "$RA_DIR" clean 2>/dev/null || true
fi

# ── Step 3: Apply PS5 patches ───────────────────────────────────────────────
info "Applying PS5-specific patches..."

# 3a. Copy our libretro.h (type-compatible with RetroArch's internal version)
cp "$SCRIPT_DIR/src/libretro.h" "$RA_DIR/libretro-common/include/libretro_ps5.h"
ok "Copied libretro.h"

# 3b. Install PS5 platform backend
install -D "$SCRIPT_DIR/src/ps5_platform.c" \
           "$RA_DIR/frontend/drivers/platform_ps5.c"
ok "Installed platform_ps5.c"

# 3c. Create PS5 GFX driver (based on orbis gfx)
PS5_GFX="$RA_DIR/gfx/drivers/ps5_gfx.c"
if [ -f "$RA_DIR/gfx/drivers/orbis_gfx.c" ]; then
  sed 's/orbis/ps5/g; s/ORBIS/PS5/g' \
      "$RA_DIR/gfx/drivers/orbis_gfx.c" > "$PS5_GFX"
  ok "Created ps5_gfx.c from orbis_gfx.c"
else
  warn "orbis_gfx.c not found – ps5_gfx.c will need manual creation"
  # Create minimal stub
  cat > "$PS5_GFX" << 'GFXEOF'
/* ps5_gfx.c – PS5 video driver stub (see src/ps5_platform.c for API docs) */
#include "../../deps/rcheevos/include/rc_client.h"
static void *ps5_gfx_init(const video_info_t *v, input_driver_t **i, void **id) { return (void*)1; }
static bool ps5_gfx_frame(void *d, const void *f, unsigned w, unsigned h, uint64_t c, unsigned p, const char *m, video_frame_info_t *i) { return true; }
static void ps5_gfx_free(void *d) {}
static void ps5_gfx_set_nonblock_state(void *d, bool b, bool f, unsigned l) {}
static bool ps5_gfx_alive(void *d) { return true; }
static bool ps5_gfx_focus(void *d) { return true; }
static bool ps5_gfx_suppress_screensaver(void *d, bool b) { return false; }
static bool ps5_gfx_has_windowed(void *d) { return false; }
static void ps5_gfx_viewport_info(void *d, struct video_viewport *v) {}
video_driver_t video_ps5 = {
  ps5_gfx_init, ps5_gfx_frame, ps5_gfx_set_nonblock_state,
  ps5_gfx_alive, ps5_gfx_focus, ps5_gfx_suppress_screensaver,
  ps5_gfx_has_windowed, NULL, ps5_gfx_free, "ps5",
  ps5_gfx_viewport_info,
};
GFXEOF
  warn "Created ps5_gfx.c stub – needs full implementation"
fi

# 3d. Create PS5 audio driver
PS5_AUD="$RA_DIR/audio/drivers/ps5_audio.c"
if [ -f "$RA_DIR/audio/drivers/orbis_audio.c" ]; then
  sed 's/orbis/ps5/g; s/ORBIS/PS5/g' \
      "$RA_DIR/audio/drivers/orbis_audio.c" > "$PS5_AUD"
  ok "Created ps5_audio.c from orbis_audio.c"
else
  warn "orbis_audio.c not found – creating stub"
  cat > "$PS5_AUD" << 'AUDEOF'
/* ps5_audio.c – PS5 audio driver stub (see src/ps5_platform.c for API docs) */
static void *ps5_audio_init(const char *d, unsigned r, unsigned l, bool bt, unsigned *nf) { return (void*)1; }
static void ps5_audio_free(void *d) {}
static ssize_t ps5_audio_write(void *d, const void *b, size_t s) { return (ssize_t)s; }
static bool ps5_audio_stop(void *d) { return true; }
static bool ps5_audio_start(void *d, bool m) { return true; }
static bool ps5_audio_alive(void *d) { return true; }
static void ps5_audio_set_nonblock_state(void *d, bool b) {}
static bool ps5_audio_use_float(void *d) { return false; }
audio_driver_t audio_ps5 = { ps5_audio_init, ps5_audio_write, ps5_audio_stop, ps5_audio_start, ps5_audio_alive, ps5_audio_set_nonblock_state, ps5_audio_free, ps5_audio_use_float, "ps5" };
AUDEOF
fi

# 3e. Create PS5 input driver
PS5_INP="$RA_DIR/input/drivers/ps5_input.c"
if [ -f "$RA_DIR/input/drivers/ps4_input.c" ]; then
  # PS4 input → PS5: update button mapping
  sed 's/ps4/ps5/g; s/PS4/PS5/g' \
      "$RA_DIR/input/drivers/ps4_input.c" > "$PS5_INP"
  # Patch DualSense button layout (see ps5_platform.c Section 3)
  python3 - << 'PYEOF'
import re, sys
with open(sys.argv[1]) as f: s = f.read()
# Replace DualShock 4 button defines with DualSense values
ds4_to_ds5 = {
    'CELL_PAD_CTRL_SQUARE':   '0x00008000',
    'CELL_PAD_CTRL_CROSS':    '0x00004000',
    'CELL_PAD_CTRL_CIRCLE':   '0x00002000',
    'CELL_PAD_CTRL_TRIANGLE': '0x00001000',
    'CELL_PAD_CTRL_SELECT':   '0x00000001',
    'CELL_PAD_CTRL_START':    '0x00000008',
}
for old, new in ds4_to_ds5.items():
    s = s.replace(old, new)
with open(sys.argv[1], 'w') as f: f.write(s)
PYEOF
  python3 -c "
import sys
with open('$PS5_INP') as f: s = f.read()
" "$PS5_INP" 2>/dev/null || true
  ok "Created ps5_input.c from ps4_input.c with DualSense button mapping"
else
  warn "ps4_input.c not found – creating minimal stub"
  echo "/* ps5_input.c stub */" > "$PS5_INP"
fi

# 3f. Register PS5 drivers in RetroArch driver tables
info "Registering PS5 drivers in RetroArch..."
# Patch gfx/drivers/gfx_drivers.c to include video_ps5
for drv_file in "$RA_DIR/gfx/drivers/gfx_drivers.c" "$RA_DIR/griffin/griffin.c"; do
  if [ -f "$drv_file" ]; then
    if ! grep -q "ps5_gfx" "$drv_file" 2>/dev/null; then
      # Add include near other orbis/ps includes
      sed -i '/orbis_gfx/a #include "../gfx/drivers/ps5_gfx.c"' "$drv_file" 2>/dev/null || true
    fi
  fi
done
warn "Driver registration may need manual editing of griffin.c or equivalent"

# ── Step 4: Create PS5 Makefile ─────────────────────────────────────────────
info "Creating Makefile.ps5..."
cat > "$RA_DIR/Makefile.ps5" << 'MAKEEOF'
# Makefile.ps5  –  Build RetroArch for PS5 (shellcode flat binary)
CC      = gcc
CXX     = g++
OBJCOPY = objcopy

# Same hardened flags as EmuC shellcode
CFLAGS  = -Os -ffreestanding -fno-stack-protector -fno-builtin \
          -fpie -mno-red-zone -fomit-frame-pointer -fcf-protection=none \
          -fno-exceptions -fno-unwind-tables -fno-asynchronous-unwind-tables \
          -DORBIS -DPS5 -DRARCH_INTERNAL \
          -Ilibretro-common/include -Ideps -I.

LDFLAGS = -T ../linker.ld -nostdlib -nostartfiles -static \
          -Wl,--build-id=none -Wl,--no-dynamic-linker -Wl,-z,norelro -no-pie

# Note: full RetroArch build requires many more source files.
# This Makefile is a starting point – expect link errors that need
# resolving by providing stubs for unsupported features.

SRCS = frontend/drivers/platform_ps5.c \
       gfx/drivers/ps5_gfx.c \
       audio/drivers/ps5_audio.c \
       input/drivers/ps5_input.c

OBJS = $(SRCS:.c=.o)

all: retroarch.bin

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

retroarch.elf: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS)

retroarch.bin: retroarch.elf
	$(OBJCOPY) -O binary $< $@
	@echo "Built: $@ ($$(wc -c < $@) bytes)"
	@echo "Next:  python3 ../gen_lua.py retroarch.bin ../es.lua"

clean:
	rm -f $(OBJS) retroarch.elf retroarch.bin
MAKEEOF
ok "Created Makefile.ps5"

# ── Step 5: Attempt build ────────────────────────────────────────────────────
info "Attempting RetroArch PS5 build (stub drivers only)..."
if make -C "$RA_DIR" -f Makefile.ps5 2>&1 | tail -20; then
  ok "Build succeeded!"
  if [ -f "$RA_DIR/retroarch.bin" ]; then
    info "Generating es.lua..."
    python3 "$SCRIPT_DIR/gen_lua.py" "$RA_DIR/retroarch.bin" "$SCRIPT_DIR/es.lua"
    ok "es.lua generated! Ready to launch."
  fi
else
  warn "Build failed (expected – full RetroArch needs more porting work)"
  echo ""
  echo "Next steps to complete the port:"
  echo "  1. Review link errors above"
  echo "  2. Provide stubs for missing symbols (printf, malloc, etc.) via musl-libc"
  echo "  3. Implement ps5_gfx.c using the API documented in src/ps5_platform.c"
  echo "  4. Implement ps5_audio.c and ps5_input.c"
  echo "  5. Disable RetroArch features that need kernel (networking, shaders...)"
  echo "  6. Re-run this script"
  echo ""
  echo "Meanwhile, the EmuC EmulationStation frontend (es_frontend.bin)"
  echo "provides a working multi-system libretro launcher. Build it with:"
  echo "  make es && python3 gen_lua.py es_frontend.bin es.lua"
fi

echo ""
echo "═══════════════════════════════════════════════════════"
echo "  Done!"
echo "═══════════════════════════════════════════════════════"
