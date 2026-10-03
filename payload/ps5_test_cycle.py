#!/usr/bin/env python3
"""Deploy-and-test cycle for ps5_suite_server on 192.168.0.53.

Usage:
  ps5_test_cycle.py wait        - block until elfldr (9021) is reachable
  ps5_test_cycle.py kill        - send port9113_killer.elf to 9021
  ps5_test_cycle.py deploy      - send ps5_suite_server.elf to 9021
  ps5_test_cycle.py ping        - PING the server on 9113
  ps5_test_cycle.py mount TID   - mount/unmount cycle for a title id
  ps5_test_cycle.py full TID    - wait+kill+deploy+ping+mount/unmount
"""
import socket, sys, time, struct

IP = "192.168.0.53"
ELF_PORT = 9021
SRV_PORT = 9113
DIR = "/mnt/c/Users/HACKMAN/Desktop/ps5 test/my_projects/ps5_upload_suite/payload"

def port_open(p, t=3):
    try:
        socket.create_connection((IP, p), timeout=t).close()
        return True
    except OSError:
        return False

def send_elf(path):
    data = open(path, "rb").read()
    s = socket.create_connection((IP, ELF_PORT), timeout=15)
    s.sendall(data)
    s.close()
    return len(data)

def rpc(cmd, payload=b"", timeout=10):
    s = socket.create_connection((IP, SRV_PORT), timeout=timeout)
    s.settimeout(timeout)
    s.sendall(bytes([cmd]) + struct.pack("<I", len(payload)) + payload)
    hdr = s.recv(5)
    if len(hdr) < 5:
        s.close(); return None, b""
    typ = hdr[0]
    ln = struct.unpack("<I", hdr[1:5])[0]
    data = b""
    while len(data) < ln:
        chunk = s.recv(min(65536, ln - len(data)))
        if not chunk:
            break
        data += chunk
    s.close()
    return typ, data

def drain_progress(s, timeout=90):
    """Read frames until RESP_OK(0x01)/RESP_ERROR; print progress."""
    end = time.time() + timeout
    while time.time() < end:
        s.settimeout(max(1, int(end - time.time())))
        hdr = s.recv(5)
        if len(hdr) < 5:
            return None, b""
        typ = hdr[0]; ln = struct.unpack("<I", hdr[1:5])[0]
        data = b""
        while len(data) < ln:
            chunk = s.recv(min(65536, ln - len(data)))
            if not chunk: break
            data += chunk
        if typ == 0x05:
            print("  prog:", data.decode(errors="replace").strip())
        else:
            return typ, data
    return None, b"TIMEOUT"

def rpc_long(cmd, payload=b"", timeout=120):
    s = socket.create_connection((IP, SRV_PORT), timeout=15)
    s.sendall(bytes([cmd]) + struct.pack("<I", len(payload)) + payload)
    r = drain_progress(s, timeout)
    s.close()
    return r

CMD_PING = 0x01
CMD_MOUNT_GAMES = 0x30
CMD_UNMOUNT_GAME = 0x3B

if __name__ == "__main__":
    op = sys.argv[1] if len(sys.argv) > 1 else "wait"
    if op == "wait":
        t0 = time.time()
        while not port_open(ELF_PORT):
            time.sleep(5)
            if time.time() - t0 > 3600:
                print("TIMEOUT waiting for elfldr"); sys.exit(1)
        print("elfldr up")
    elif op == "kill":
        print("sent killer:", send_elf(f"{DIR}/port9113_killer.elf"))
    elif op == "deploy":
        print("sent payload:", send_elf(f"{DIR}/ps5_suite_server.elf"))
    elif op == "ping":
        t, d = rpc(CMD_PING, timeout=8)
        print("PING ->", hex(t or 0), d[:200])
    elif op == "mount":
        tid = sys.argv[2]
        print("== UNMOUNT", tid)
        t, d = rpc_long(CMD_UNMOUNT_GAME, tid.encode())
        print("UNMOUNT ->", hex(t or 0), d[:300])
        print("== MOUNT (Mount Games scans all paths)")
        t, d = rpc_long(CMD_MOUNT_GAMES)
        print("MOUNT ->", hex(t or 0), d[:2000])
    elif op == "full":
        tid = sys.argv[2] if len(sys.argv) > 2 else "PPSA10737"
        print("[1] wait elfldr...")
        t0 = time.time()
        while not port_open(ELF_PORT):
            time.sleep(5)
            if time.time() - t0 > 3600: sys.exit("timeout")
        print("[2] kill stale...")
        send_elf(f"{DIR}/port9113_killer.elf")
        time.sleep(4)
        print("[3] deploy payload...")
        send_elf(f"{DIR}/ps5_suite_server.elf")
        time.sleep(4)
        print("[4] ping...")
        t, d = rpc(CMD_PING, timeout=8)
        print("PING ->", hex(t or 0), d[:200])
        print("[5] mount cycle", tid)
        t, d = rpc_long(CMD_UNMOUNT_GAME, tid.encode())
        print("UNMOUNT ->", hex(t or 0), d[:300])
        t, d = rpc_long(CMD_MOUNT_GAMES)
        print("MOUNT ->", hex(t or 0), d[:2000])
