"""
EgyDevTeam EmulationStation Launcher
Sends es.lua to PS5, uploads libretro core .bin files, then uploads ROMs
organised by system via the built-in C FTP server.

Usage:
  python es_launcher.py <PS5_IP>
  python es_launcher.py <PS5_IP> --roms-dir /path/to/roms
  python es_launcher.py <PS5_IP> --system nes
  python es_launcher.py <PS5_IP> --skip-upload

ROM directory structure expected under --roms-dir:
  roms/
    nes/    *.nes  *.rom
    snes/   *.sfc  *.smc
    md/     *.md   *.bin  *.gen  *.smd
    gb/     *.gb   *.gbc
    gba/    *.gba

Core directory (--cores-dir) should contain flat binaries:
  cores/
    nes_core.bin
    snes_core.bin   (when available)
    ...
"""

import socket
import os
import time
import argparse
from ftplib import FTP

# ── Constants ─────────────────────────────────────────────────────────────────

PAYLOAD_PORT = 9026
FTP_PORT     = 1337

# Extensions recognised per system (lower-case).
SYSTEM_EXTENSIONS = {
    "nes":  {".nes", ".rom"},
    "snes": {".sfc", ".smc"},
    "md":   {".md", ".bin", ".gen", ".smd"},
    "gb":   {".gb", ".gbc"},
    "gba":  {".gba"},
}

ALL_SYSTEMS = ["nes", "snes", "md", "gb", "gba"]

# ── Shared helpers (mirror nes_launcher.py) ───────────────────────────────────

def send_payload(host, filepath, port=PAYLOAD_PORT):
    """Open a raw TCP connection to *port* and send the contents of *filepath*."""
    with open(filepath, "rb") as f:
        data = f.read()
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(10)
    sock.connect((host, port))
    sock.sendall(data)
    sock.close()
    print(f"  Sent {os.path.basename(filepath)} ({len(data):,} bytes)")


def scan_roms(roms_dir, extensions):
    """Return a sorted list of absolute paths for files whose extension is in
    *extensions* (a set of lower-case strings including the leading dot)."""
    if not os.path.isdir(roms_dir):
        return []
    return sorted([
        os.path.join(roms_dir, f)
        for f in os.listdir(roms_dir)
        if os.path.splitext(f)[1].lower() in extensions
        and os.path.isfile(os.path.join(roms_dir, f))
    ])


def wait_for_ftp(host, port, timeout=20):
    """Block until the PS5 FTP banner (220 …) arrives or *timeout* seconds
    have elapsed.  Returns True on success."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        remaining = max(1, int(deadline - time.time()))
        s.settimeout(min(remaining, 5))
        try:
            s.connect((host, port))
            banner = s.recv(256)
            s.close()
            if b"220" in banner:
                return True
        except Exception:
            try:
                s.close()
            except Exception:
                pass
        time.sleep(1)
    return False


def send_site_exit(host, port):
    """Send SITE EXIT to the PS5 FTP server so it releases the network port."""
    try:
        ftp = FTP()
        ftp.connect(host, port, timeout=10)
        ftp.login("anonymous", "")
        ftp.sendcmd("SITE EXIT")
        ftp.quit()
        print("  FTP server released")
    except Exception as e:
        print(f"  [WARN] Could not release FTP: {e}")


def upload_roms(host, roms, port=FTP_PORT):
    """Upload *roms* (list of local file paths) to the PS5 FTP server.

    Files that already exist on the console with the same byte size are
    skipped.  Progress is printed at meaningful milestones.
    """
    total      = len(roms)
    total_size = sum(os.path.getsize(r) for r in roms)
    print(f"  {total} files ({total_size / 1_048_576:.1f} MB)")

    ftp = FTP()
    ftp.connect(host, port, timeout=15)
    ftp.login("anonymous", "")
    ftp.sendcmd("TYPE I")

    existing = set()
    try:
        existing = set(ftp.nlst())
    except Exception:
        pass

    # Determine how many files actually need uploading.
    to_upload = 0
    for rom_path in roms:
        fn = os.path.basename(rom_path)
        sz = os.path.getsize(rom_path)
        if fn in existing:
            try:
                if ftp.size(fn) == sz:
                    continue
            except Exception:
                pass
        to_upload += 1

    try:
        ftp.sendcmd(f"SITE TOTAL {to_upload}")
    except Exception:
        pass

    uploaded   = 0
    skipped    = 0
    bytes_sent = 0
    t0         = time.time()

    for i, rom_path in enumerate(roms, 1):
        fn = os.path.basename(rom_path)
        sz = os.path.getsize(rom_path)

        if fn in existing:
            try:
                if ftp.size(fn) == sz:
                    skipped += 1
                    if skipped % 50 == 0 or i == total:
                        print(f"  [{i * 100 // total:3d}%] Skipped {fn}")
                    continue
            except Exception:
                pass

        try:
            with open(rom_path, "rb") as f:
                ftp.storbinary(f"STOR {fn}", f, blocksize=8192)
            uploaded   += 1
            bytes_sent += sz
        except Exception as e:
            print(f"  [ERR] {fn}: {e}")
            continue

        if uploaded <= 5 or uploaded == to_upload or uploaded % 25 == 0:
            elapsed = time.time() - t0
            speed   = bytes_sent / elapsed / 1024 if elapsed > 0 else 0
            pct     = i * 100 // total
            print(f"  {pct:3d}% | {uploaded}/{to_upload} | {speed:.0f} KB/s | {fn}")

    elapsed = time.time() - t0
    print(f"  Done: {uploaded} uploaded, {skipped} skipped "
          f"({bytes_sent / 1_048_576:.1f} MB in {elapsed:.1f}s)")

    try:
        ftp.sendcmd("SITE EXIT")
    except Exception:
        pass
    try:
        ftp.quit()
    except Exception:
        pass


# ── New helper: core upload ───────────────────────────────────────────────────

def upload_cores(host, cores_dir, port=FTP_PORT):
    """Upload all *.bin files found in *cores_dir* to the PS5 FTP server.

    Core binaries are transferred to the current working directory on the
    console (same as ROMs – the ES frontend reads them from there).
    Files that already exist with the same byte-size are skipped.
    """
    if not os.path.isdir(cores_dir):
        print(f"  [WARN] Cores directory not found: {cores_dir}")
        return

    bins = sorted([
        os.path.join(cores_dir, f)
        for f in os.listdir(cores_dir)
        if f.lower().endswith(".bin") and os.path.isfile(os.path.join(cores_dir, f))
    ])

    if not bins:
        print(f"  No *.bin core files found in {cores_dir}")
        return

    total_size = sum(os.path.getsize(b) for b in bins)
    print(f"  {len(bins)} core(s) ({total_size / 1_048_576:.2f} MB)")

    ftp = FTP()
    ftp.connect(host, port, timeout=15)
    ftp.login("anonymous", "")
    ftp.sendcmd("TYPE I")

    existing = set()
    try:
        existing = set(ftp.nlst())
    except Exception:
        pass

    uploaded = 0
    skipped  = 0

    for core_path in bins:
        fn = os.path.basename(core_path)
        sz = os.path.getsize(core_path)

        if fn in existing:
            try:
                if ftp.size(fn) == sz:
                    print(f"  Skip  {fn} (already up to date)")
                    skipped += 1
                    continue
            except Exception:
                pass

        try:
            with open(core_path, "rb") as f:
                ftp.storbinary(f"STOR {fn}", f, blocksize=8192)
            print(f"  OK    {fn} ({sz:,} bytes)")
            uploaded += 1
        except Exception as e:
            print(f"  [ERR] {fn}: {e}")

    print(f"  Done: {uploaded} uploaded, {skipped} skipped")

    try:
        ftp.sendcmd("SITE EXIT")
    except Exception:
        pass
    try:
        ftp.quit()
    except Exception:
        pass


# ── Main ──────────────────────────────────────────────────────────────────────

def main():
    p = argparse.ArgumentParser(
        description="EgyDevTeam EmulationStation Launcher – multi-system PS5 frontend",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  python es_launcher.py 192.168.1.100
  python es_launcher.py 192.168.1.100 --system nes
  python es_launcher.py 192.168.1.100 --roms-dir /mnt/roms --cores-dir ./cores
  python es_launcher.py 192.168.1.100 --skip-upload
""",
    )
    p.add_argument("ps5_ip",        help="PS5 IP address")
    p.add_argument("--roms-dir",    default=None,
                   help="Root directory containing system sub-dirs "
                        "(nes/ snes/ md/ gb/ gba/).  Default: ./roms")
    p.add_argument("--cores-dir",   default=None,
                   help="Directory containing *.bin core files.  Default: ./cores")
    p.add_argument("--launcher",    default=None,
                   help="Lua payload file to send.  Default: ./es.lua")
    p.add_argument("--skip-upload", action="store_true",
                   help="Skip all FTP uploads (just send the Lua payload)")
    p.add_argument("--system",      choices=ALL_SYSTEMS, default=None,
                   help="Only upload ROMs for this system")
    p.add_argument("--ftp-wait",    type=int, default=15,
                   help="Seconds to wait for FTP server to become ready (default: 15)")
    args = p.parse_args()

    base      = os.path.dirname(os.path.abspath(__file__))
    roms_root = args.roms_dir  or os.path.join(base, "roms")
    cores_dir = args.cores_dir or os.path.join(base, "cores")
    launcher  = args.launcher  or os.path.join(base, "es.lua")

    # Which systems to upload ROMs for.
    systems = [args.system] if args.system else ALL_SYSTEMS

    print("EgyDevTeam EmulationStation Launcher")
    print(f"  PS5     : {args.ps5_ip}")
    print(f"  ROMs    : {roms_root}")
    print(f"  Cores   : {cores_dir}")
    print(f"  Launcher: {launcher}")
    print(f"  Systems : {', '.join(systems)}")
    print()

    # ── Step 1: Send the ES Lua payload ──────────────────────────────────────
    print("[1] Sending EmulationStation payload...")
    if not os.path.isfile(launcher):
        print(f"  [FAIL] Launcher not found: {launcher}")
        return
    try:
        send_payload(args.ps5_ip, launcher)
    except Exception as e:
        print(f"  [FAIL] Could not send payload: {e}")
        return
    print()

    # Skip everything FTP-related if asked.
    if args.skip_upload:
        print("  --skip-upload set; skipping FTP uploads.")
        print()
        print("Done! EmulationStation running on PS5.")
        return

    # ── Step 2: Wait for FTP ─────────────────────────────────────────────────
    print("[2] Waiting for FTP server...")
    if not wait_for_ftp(args.ps5_ip, FTP_PORT, args.ftp_wait):
        print(f"  [FAIL] FTP server not responding after {args.ftp_wait}s")
        print("  Is the ES payload running?  Try increasing --ftp-wait.")
        print()
        print("Done! (FTP unavailable – ROMs/cores not uploaded)")
        return
    print("  FTP server ready")
    print()

    # ── Step 3: Upload cores ─────────────────────────────────────────────────
    print("[3] Uploading cores...")
    try:
        upload_cores(args.ps5_ip, cores_dir, FTP_PORT)
    except Exception as e:
        print(f"  FTP error during core upload: {e}")
        send_site_exit(args.ps5_ip, FTP_PORT)
        return
    print()

    # ── Steps 4+: Upload ROMs per system ─────────────────────────────────────
    step = 4
    any_roms = False

    for system in systems:
        sys_dir = os.path.join(roms_root, system)
        exts    = SYSTEM_EXTENSIONS[system]
        roms    = scan_roms(sys_dir, exts)

        if not roms:
            print(f"[{step}] Uploading ROMs ({system.upper()}: 0 files – skipping)")
            print(f"  (No files found in {sys_dir})")
            print()
            step += 1
            continue

        print(f"[{step}] Uploading ROMs ({system.upper()}: {len(roms)} files)...")
        any_roms = True
        try:
            upload_roms(args.ps5_ip, roms, FTP_PORT)
        except Exception as e:
            print(f"  FTP error during {system.upper()} upload: {e}")
            send_site_exit(args.ps5_ip, FTP_PORT)
            return
        print()
        step += 1

    # If upload_roms() already sent SITE EXIT on the last system we are done;
    # but if no ROMs were found we still need to release the FTP port cleanly.
    if not any_roms:
        print(f"[{step}] No ROMs found in any system directory – releasing FTP...")
        send_site_exit(args.ps5_ip, FTP_PORT)
        print()

    print("Done! EmulationStation running on PS5.")


if __name__ == "__main__":
    main()
