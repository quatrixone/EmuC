# EmuC – EmulationStation PS5

**Multi-system libretro frontend for PS5 via the LuaC0re exploit.**

> No kernel exploit required. Runs entirely in PS5 userland using the
> Star Wars Racer Revenge (CUSA03474 / CUSA03492) JIT exploit, all firmwares up to 13.00.

---

## What's included

| File | Description |
|------|-------------|
| `es.lua` | **Main payload** – send this to PS5 port 9026 |
| `es_launcher.py` | PC-side launcher: sends payload + uploads ROMs/cores via FTP |
| `es_frontend.bin` | Compiled EmulationStation frontend shellcode |
| `cores/nes_core.bin` | NES libretro core (built-in emulator exposed as libretro API) |
| `nes.lua` / `nes_launcher.py` | Original standalone NES emulator (still works) |
| `build_retroarch.sh` | Phase 2: clone + patch RetroArch orbis→PS5 |
| `cores/build_cores.sh` | Phase 3: download + build real libretro cores |
| `gen_lua.py` | Regenerate `es.lua` after recompiling C code |

---

## Quick Start

### Requirements
- PS5 with LuaC0re set up (Star Wars Racer Revenge)
- Python 3 on your PC
- ROMs in `roms/` subdirectories (see structure below)

### Launch
```bash
# Send ES payload + upload ROMs + cores
python3 es_launcher.py <PS5_IP>

# Upload only NES ROMs
python3 es_launcher.py <PS5_IP> --system nes

# Skip ROM upload (just launch)
python3 es_launcher.py <PS5_IP> --skip-upload
```

### ROM directory structure
```
roms/
├── nes/    ← .nes .rom files
├── snes/   ← .sfc .smc files
├── md/     ← .md .bin .gen files
├── gb/     ← .gb .gbc files
└── gba/    ← .gba files

cores/
├── nes_core.bin     ← built-in (NES emulator)
├── snes_core.bin    ← see cores/build_cores.sh
├── md_core.bin
├── gb_core.bin
└── gba_core.bin
```

---

## Systems & Cores

| System | Extension | Core file | Status |
|--------|-----------|-----------|--------|
| Nintendo Entertainment System | `.nes` `.rom` | `nes_core.bin` | ✅ Built-in |
| Super Nintendo | `.sfc` `.smc` | `snes_core.bin` | ⏳ snes9x port needed |
| Sega Mega Drive | `.md` `.bin` | `md_core.bin` | ⏳ GenesisPlusGX port needed |
| Game Boy / Color | `.gb` `.gbc` | `gb_core.bin` | ⏳ gambatte port needed |
| Game Boy Advance | `.gba` | `gba_core.bin` | ⏳ mGBA port needed |

**NES** works out of the box with the built-in emulator (no core .bin needed).  
Other systems show "CORE NOT LOADED" until you upload their core binary via FTP.

---

## Controller

| DualSense | Action |
|-----------|--------|
| D-Pad | Navigate menus / game d-pad |
| Cross | Confirm / A button |
| Circle | B button |
| Square | X button |
| Triangle | Y button |
| L1 | Back to previous menu |
| R1 | Exit |
| Options | Start |
| Share | Select |

Web controller also available at `http://<PS5_IP>:9030`

---

## Building from source

```bash
# Build both NES emulator and ES frontend
make all

# Build only ES frontend
make es

# Build NES libretro core
make cores

# Regenerate es.lua after code changes
python3 gen_lua.py es_frontend.bin es.lua

# Or use nes.lua (original NES emulator)
python3 gen_lua.py nes_emu.bin nes.lua --nes
```

---

## Phase 2: Full RetroArch port

```bash
./build_retroarch.sh
```

Clones RetroArch, applies PS5 patches on the orbis (PS4) backend, and attempts
to compile. See `src/ps5_platform.c` for full API documentation.

## Phase 3: Real libretro cores

```bash
./cores/build_cores.sh [nes|snes|md|gb|gba|all]
```

Downloads and compiles: nestopia-ue, snes9x, Genesis Plus GX, gambatte, mGBA.
See script for current porting status.

---

## Architecture

```
PS5 (userland exploit)
└── LuaC0re JIT → es.lua
    ├── Embeds compiled x86-64 shellcode (es_frontend.bin)
    ├── Allocates video framebuffers (1920×1080 BGRA8888)
    ├── Opens audio (48kHz stereo s16)
    ├── Opens FTP server (port 1337) for ROM/core upload
    └── Calls _start(eboot_base, dlsym, ext_args)
        ├── EmulationStation UI (system picker + ROM picker)
        ├── Core loader: reads *.bin, maps RWX, calls _core_start()
        └── libretro run loop (video/audio/input callbacks)
```

**Core binary format:**  
Each `*_core.bin` starts with `_core_start(u64 base, struct core_header *out)`.  
The frontend maps it, calls this function to get function offsets, then invokes  
`retro_init → retro_load_game → retro_run` each frame.

---

## Credits

- EmuC NES emulator core by **egycnq / EgyDevTeam**
- LuaC0re exploit framework
- libretro API: libretro.org
- EmulationStation: emulationstation.org (this project is a PS5 native re-implementation)
