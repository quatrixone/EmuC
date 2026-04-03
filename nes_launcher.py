"""
EgyDevTeam NES Launcher
Sends nes.lua to PS5, uploads ROMs via built-in C FTP server.

  python nes_launcher.py <PS5_IP>
  python nes_launcher.py <PS5_IP> --roms-dir D:\\NES
  python nes_launcher.py <PS5_IP> --skip-upload

Place .nes files in a 'roms' folder next to this script
"""

import socket, os, time, argparse
from ftplib import FTP

PAYLOAD_PORT = 9026
FTP_PORT = 1337


def send_payload(host, filepath, port=PAYLOAD_PORT):
    with open(filepath, 'rb') as f:
        data = f.read()
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(10)
    sock.connect((host, port))
    sock.sendall(data)
    sock.close()
    print(f"  Sent {os.path.basename(filepath)} ({len(data):,} bytes)")


def scan_roms(roms_dir, extensions):
    if not os.path.isdir(roms_dir):
        return []
    return sorted([
        os.path.join(roms_dir, f) for f in os.listdir(roms_dir)
        if os.path.splitext(f)[1].lower() in extensions
        and os.path.isfile(os.path.join(roms_dir, f))
    ])


def wait_for_ftp(host, port, timeout=20):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(timeout)
    try:
        s.connect((host, port))
        banner = s.recv(256)
        s.close()
        return b'220' in banner
    except:
        try:
            s.close()
        except:
            pass
        return False


def send_site_exit(host, port):
    try:
        ftp = FTP()
        ftp.connect(host, port, timeout=10)
        ftp.login('anonymous', '')
        ftp.sendcmd('SITE EXIT')
        ftp.quit()
        print("  FTP server released")
    except Exception as e:
        print(f"  [WARN] Could not release FTP: {e}")


def upload_roms(host, roms, port=FTP_PORT):
    total = len(roms)
    total_size = sum(os.path.getsize(r) for r in roms)
    print(f"  {total} files ({total_size / 1048576:.1f} MB)")

    ftp = FTP()
    ftp.connect(host, port, timeout=15)
    ftp.login('anonymous', '')
    ftp.sendcmd('TYPE I')

    existing = set()
    try:
        existing = set(ftp.nlst())
    except:
        pass

    # count actual uploads
    to_upload = 0
    for rom_path in roms:
        fn = os.path.basename(rom_path)
        sz = os.path.getsize(rom_path)
        if fn in existing:
            try:
                if ftp.size(fn) == sz:
                    continue
            except:
                pass
        to_upload += 1

    try:
        ftp.sendcmd(f'SITE TOTAL {to_upload}')
    except:
        pass

    uploaded = 0
    skipped = 0
    bytes_sent = 0
    t0 = time.time()

    for i, rom_path in enumerate(roms, 1):
        fn = os.path.basename(rom_path)
        sz = os.path.getsize(rom_path)

        if fn in existing:
            try:
                if ftp.size(fn) == sz:
                    skipped += 1
                    if skipped % 50 == 0 or i == total:
                        print(f"  [{i*100//total:3d}%] Skipped {fn}")
                    continue
            except:
                pass

        try:
            with open(rom_path, 'rb') as f:
                ftp.storbinary(f'STOR {fn}', f, blocksize=8192)
            uploaded += 1
            bytes_sent += sz
        except Exception as e:
            print(f"  [ERR] {fn}: {e}")
            continue

        if uploaded <= 5 or uploaded == to_upload or uploaded % 25 == 0:
            elapsed = time.time() - t0
            speed = bytes_sent / elapsed / 1024 if elapsed > 0 else 0
            pct = i * 100 // total
            print(f"  {pct:3d}% | {uploaded}/{to_upload} | {speed:.0f} KB/s | {fn}")

    elapsed = time.time() - t0
    print(f"  Done: {uploaded} uploaded, {skipped} skipped "
          f"({bytes_sent / 1048576:.1f} MB in {elapsed:.1f}s)")

    try:
        ftp.sendcmd('SITE EXIT')
    except:
        pass
    try:
        ftp.quit()
    except:
        pass


def main():
    p = argparse.ArgumentParser(description='EgyDevTeam NES Launcher')
    p.add_argument('ps5_ip', help='PS5 IP address')
    p.add_argument('--roms-dir', default=None)
    p.add_argument('--launcher', default=None, help='Lua launcher (default: nes.lua)')
    p.add_argument('--skip-upload', action='store_true')
    p.add_argument('--ext', nargs='+', default=['.nes'])
    p.add_argument('--ftp-wait', type=int, default=10,
                   help='Max seconds to wait for FTP server (default: 10)')
    args = p.parse_args()

    base = os.path.dirname(os.path.abspath(__file__))
    roms_dir = args.roms_dir or os.path.join(base, 'roms')
    launcher = args.launcher or os.path.join(base, 'nes.lua')
    extensions = {e if e.startswith('.') else '.' + e for e in args.ext}

    print(f"EgyDevTeam NES Launcher")
    print(f"  PS5: {args.ps5_ip}  ROMs: {roms_dir}")
    print()

    roms = []
    if not args.skip_upload:
        print("[1] Scanning ROMs...")
        roms = scan_roms(roms_dir, extensions)
        if not roms:
            print(f"  No files in {roms_dir}")
            print(f"  Will launch emulator without uploading.")
        else:
            print(f"  Found {len(roms)} ROM(s)")
        print()

    print("[2] Launching emulator...")
    if not os.path.isfile(launcher):
        print(f"  [FAIL] Not found: {launcher}")
        return
    send_payload(args.ps5_ip, launcher)
    print()

    print(f"[3] Waiting for FTP server on PS5...")
    if wait_for_ftp(args.ps5_ip, FTP_PORT, args.ftp_wait):
        print(f"  FTP server ready")
        if roms and not args.skip_upload:
            print()
            print("[4] Uploading ROMs...")
            try:
                upload_roms(args.ps5_ip, roms)
            except Exception as e:
                print(f"  FTP error: {e}")
                send_site_exit(args.ps5_ip, FTP_PORT)
        else:
            print("  Releasing FTP server...")
            send_site_exit(args.ps5_ip, FTP_PORT)
    else:
        print(f"  FTP server not responding after {args.ftp_wait}s")
        print()

    print("Done!")


if __name__ == '__main__':
    main()