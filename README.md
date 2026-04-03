# EmuC0re

Emulators running as native x86_64 shellcode on PS5 through the [LuaC0re](https://github.com/Gezine/Luac0re) JIT exploit.

PoC that full homebrew apps can be built and run from PS5 userland — no kernel exploit needed.  
Works on all PS5 firmwares up to **13.00** (latest).

## NES Emulator

First emulator in the project

- Full 6502 CPU (all 256 opcodes including illegals)
- PPU with scrolling, sprites, sprite 0 hit
- APU with 48kHz output
- DualSense native pad support
- Web-based touch/WebHID controller
- Built-in FTP server for ROM upload
- ROM picker menu
- NTSC/PAL

### Supported Mappers

| #   | Name      |
| --- | --------- |
| 0   | NROM      |
| 1   | MMC1      |
| 2   | UxROM     |
| 3   | CNROM     |
| 4   | MMC3      |
| 7   | AxROM     |
| 9   | MMC2      |
| 10  | MMC4      |
| 34  | BNROM     |
| 66  | GxROM     |
| 69  | FME-7     |
| 87  | J87       |
| 94  | UxROM V   |
| 180 | Inv UxROM |
| 185 | CNROM CP  |
| 206 | DxROM     |

Games on unsupported mappers won't run.

## Requirements

- PS5 console (any firmware, tested up to 13.00)
- [LuaC0re](https://github.com/Gezine/Luac0re) set up and working
- _Star Wars Racer Revenge_ — US (CUSA03474) or EU (CUSA03492)  
  If you're on latest FW you can grab the digital version from the PS Store
- Python 3 on your PC
- PC and PS5 on the same network

## Building

```
make clean && make
```

Produces `nes_emu.bin`. Convert to hex and paste into `nes.lua` as the `sc` variable:

## Usage

### Setup

Optional — listen for debug log on your PC:

Edit `nes.lua` and set your PC's IP for debug logs:

```lua
local PC_IP = "192.168.1.121"
```

```
nc -u -l -p 9027
```

### Launch

```
python nes_launcher.py <PS5_IP>
```

This sends the payload to the LuaC0re loader (port 9026), waits for the FTP server, uploads ROMs from the `roms/` folder, then starts the emulator.

Options:

```
--roms-dir PATH    ROM folder (default: ./roms)
--skip-upload      Launch without uploading ROMs
--launcher PATH    Custom lua file (default: ./nes.lua)
--ext .nes .rom    File extensions to scan
--ftp-wait SEC     FTP timeout (default: 10)
```

### ROMs

**FTP upload :** put `.nes` files in a `roms/` folder next to the script, the launcher handles the rest.

or

**Manual:** place files directly in the savedata directory, they'll show up in the nes emu picker.

### Controls

ROM picker:

- **D-Pad** — navigate
- **Cross / Start** — launch
- **L1** — back to menu
- **R1** — exit

In-game:

| DualSense        | NES    |
| ---------------- | ------ |
| Cross            | A      |
| Square           | B      |
| Triangle         | Select |
| Circle / Options | Start  |
| D-Pad            | D-Pad  |

### Input Sources

Two options: **native DualSense** or **web controller** (open `http://<PS5_IP>:9030` on your phone/PC)

The input source locks on the first button press and stays for the entire emulator session. You can't switch mid-session — you need to relaunch the emulator to change input method.

The web controller supports touch buttons, Gamepad API, and DualSense WebHID (Chrome/Edge, USB or BT).

## TODO

- [ ] More mappers
- [ ] Save states
- [ ] SNES emulator
- [ ] Game Boy / GBC emulator
- [ ] maybe more emulators lets see...

## Credits

**EgyDevTeam**

Special thanks to [Abkarino](https://github.com/AbkarinoMHM) — co-founder of EgyDevTeam.

- [Gezine](https://github.com/Gezine/Luac0re) — LuaC0re framework and JIT exploit
- [CTurt](https://github.com/CTurt) — [mast1c0re](https://cturt.github.io/mast1c0re.html) writeup
- [McCaulay](https://github.com/McCaulay) — [mast1c0re](https://mccaulay.co.uk/mast1c0re-part-2-arbitrary-ps2-code-execution/) writeup and [Okage](https://github.com/McCaulay/mast1c0re) reference implementation
- [ChampionLeake](https://github.com/ChampionLeake) — PS2 _Star Wars Racer Revenge_ exploit writeup on [psdevwiki](https://www.psdevwiki.com/ps2/Vulnerabilities)
- [shahrilnet](https://github.com/shahrilnet/remote_lua_loader) & [null_ptr](https://github.com/n0llptr) — Code references from [remote_lua_loader](https://github.com/shahrilnet/remote_lua_loader)
- [NESDev Wiki](https://www.nesdev.org/wiki/) & community — NES hardware documentation
- [nondebug/dualsense](https://github.com/nondebug/dualsense) — DualSense HID docs

## Disclaimer

For research and educational purposes only. Use at your own risk.
