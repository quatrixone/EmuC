#!/usr/bin/env python3
"""
gen_lua.py  –  Generate es.lua from a compiled flat binary + the existing nes.lua template.

Usage:
    python3 gen_lua.py <binary.bin> <output.lua> [--template nes.lua]

The script:
  1. Reads the compiled flat binary (es_frontend.bin or nes_emu.bin)
  2. Converts it to a hex string matching the format used in nes.lua
  3. Reads the template Lua file (nes.lua) as the structural base
  4. Replaces:
     - The `local sc = "..."` hex payload with the new binary
     - Branding strings: "NES EMU" → "EmulationStation"  etc.
  5. Writes the new Lua file

This allows the EmulationStation payload (es.lua) to be regenerated every time
the C code is recompiled, without manually editing the huge Lua file.
"""

import sys
import os
import re
import argparse


def bin_to_hex(data: bytes) -> str:
    """Convert binary data to space-separated uppercase hex string."""
    return " ".join(f"{b:02X}" for b in data)


def read_template(path: str) -> str:
    with open(path, "r", encoding="utf-8") as f:
        return f.read()


def replace_payload(lua: str, hex_str: str) -> str:
    """Replace the `local sc = "..."` hex blob with the new binary."""
    # The sc variable spans potentially many lines; use a greedy pattern
    pattern = re.compile(r'(local sc\s*=\s*")[^"]*(")', re.DOTALL)
    if not pattern.search(lua):
        # Fallback: try multiline assignment
        pattern = re.compile(r'(local sc\s*=\s*\[=\[)[^\]]*(\]=\])', re.DOTALL)
    replaced, n = pattern.subn(lambda m: m.group(1) + hex_str + m.group(2), lua)
    if n == 0:
        raise ValueError("Could not find 'local sc = ...' in template Lua file.")
    return replaced


def patch_branding(lua: str, is_es: bool) -> str:
    """Update branding strings for EmulationStation payload."""
    if not is_es:
        return lua
    subs = [
        ("NES EMU V0.4 by egycnq",         "EmulationStation PS5 v1.0"),
        ("NES EMU V0.4",                    "EmulationStation PS5"),
        ("EgyDevTeam NES Launcher",         "EgyDevTeam EmulationStation"),
        ("EgyDevTeam NES EMU",              "EgyDevTeam EmulationStation"),
        ("NES EMU",                         "EmulationStation"),
        ('"nesq"',                          '"esq"'),
        # Notification string
        ("NES EMU V0.4 by egycnq\\\\n",     "EmulationStation PS5 v1.0\\\\n"),
    ]
    for old, new in subs:
        lua = lua.replace(old, new)
    return lua


def main():
    p = argparse.ArgumentParser(description="Generate Lua payload from compiled binary")
    p.add_argument("binary",   help="Compiled .bin file (e.g. es_frontend.bin)")
    p.add_argument("output",   help="Output .lua file (e.g. es.lua)")
    p.add_argument("--template", default=None,
                   help="Template .lua file (default: nes.lua next to this script)")
    p.add_argument("--nes",    action="store_true",
                   help="Keep NES branding (don't replace with EmulationStation)")
    args = p.parse_args()

    base_dir = os.path.dirname(os.path.abspath(__file__))
    template_path = args.template or os.path.join(base_dir, "nes.lua")

    if not os.path.isfile(args.binary):
        print(f"[ERR] Binary not found: {args.binary}", file=sys.stderr)
        sys.exit(1)
    if not os.path.isfile(template_path):
        print(f"[ERR] Template not found: {template_path}", file=sys.stderr)
        sys.exit(1)

    with open(args.binary, "rb") as f:
        data = f.read()

    print(f"  Binary:   {args.binary} ({len(data):,} bytes)")
    hex_str = bin_to_hex(data)
    print(f"  Hex size: {len(hex_str):,} chars")

    lua = read_template(template_path)
    lua = replace_payload(lua, hex_str)
    lua = patch_branding(lua, is_es=not args.nes)

    with open(args.output, "w", encoding="utf-8") as f:
        f.write(lua)

    print(f"  Written:  {args.output} ({os.path.getsize(args.output):,} bytes)")
    print("Done.")


if __name__ == "__main__":
    main()
