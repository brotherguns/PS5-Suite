#!/usr/bin/env python3
# Probe the PS5: (1) which upload-server build is alive on 9113,
# (2) what actually remains on disk around PPSA10737 via etaHEN FTP.
import socket, ftplib, sys

PS5 = "192.168.0.53"
TID = "PPSA10737"

def probe_server():
    print("=== 9113 GET_SYSTEM_INFO probe ===")
    try:
        s = socket.create_connection((PS5, 9113), timeout=6)
        s.sendall(b'\x32\x00\x00\x00\x00')          # 0x32 GET_SYSTEM_INFO, len 0
        hdr = b''
        while len(hdr) < 5:
            c = s.recv(5 - len(hdr))
            if not c:
                break
            hdr += c
        if len(hdr) < 5:
            print("no reply (server dead or busy)")
            s.close()
            return
        resp = hdr[0]
        ln = int.from_bytes(hdr[1:5], 'little')
        data = b''
        while len(data) < ln:
            c = s.recv(ln - len(data))
            if not c:
                break
            data += c
        s.close()
        text = data.decode('utf-8', 'replace').replace('\0', '')
        print(f"resp=0x{resp:02x} len={ln}")
        for line in text.splitlines():
            if line.strip():
                print("  ", line.strip())
    except Exception as e:
        print("probe failed:", e)

def ftp_ls(ftp, path):
    try:
        items = ftp.nlst(path)
        print(f"[{path}] -> {len(items)} entries")
        for it in items[:25]:
            print("   ", it)
        return items
    except Exception as e:
        print(f"[{path}] ERROR: {e}")
        return []

def main():
    probe_server()
    print()
    print("=== FTP probe (etaHEN :1337) ===")
    try:
        ftp = ftplib.FTP()
        ftp.connect(PS5, 1337, timeout=10)
        try:
            ftp.login('anonymous', 'anonymous')
        except Exception:
            ftp.login('', '')
        print("connected:", ftp.getwelcome()[:80] if ftp.getwelcome() else "ok")

        for p in ["/user/app/" + TID, "/user/appmeta/" + TID,
                  "/system_ex/app/" + TID]:
            ftp_ls(ftp, p)

        # list /user/app and /user/appmeta to see which title dirs exist
        app = ftp_ls(ftp, "/user/app")
        meta = ftp_ls(ftp, "/user/appmeta")
        print()
        print("title dirs in /user/app:", [a.split('/')[-1] for a in app if TID in a or 'CUSA' in a or 'PPSA' in a])
        print("title dirs in /user/appmeta:", [m.split('/')[-1] for m in meta if TID in m or 'CUSA' in m or 'PPSA' in m])
        ftp.quit()
    except Exception as e:
        print("FTP failed:", e)

if __name__ == '__main__':
    main()
