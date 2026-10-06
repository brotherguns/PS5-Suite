"""Direct PS5 Suite payload probe — sends raw commands to the payload server
and prints the raw response, for debugging without the client.

Usage: python ps5_probe.py [ip] <cmd_hex> [arg-string]
       python ps5_probe.py 0x79            (pad info)
       python ps5_probe.py 0x7E ledcolor|255,0,0
"""
import socket
import struct
import sys

IP = "192.168.0.62"
PORT = 9113


def send_cmd(ip, cmd, data: bytes = b"", timeout=15.0):
    s = socket.socket()
    s.settimeout(timeout)
    s.connect((ip, PORT))
    s.sendall(bytes([cmd]) + struct.pack("<I", len(data)) + data)
    hdr = b""
    while len(hdr) < 5:
        chunk = s.recv(5 - len(hdr))
        if not chunk:
            raise ConnectionError("closed while reading header")
        hdr += chunk
    resp, dlen = hdr[0], struct.unpack("<I", hdr[1:])[0]
    body = b""
    while len(body) < dlen:
        chunk = s.recv(min(65536, dlen - len(body)))
        if not chunk:
            break
        body += chunk
    s.close()
    return resp, body


def main():
    args = sys.argv[1:]
    ip = IP
    if args and ":" in args[0] or (args and args[0].count(".") == 3):
        ip = args.pop(0)
    if not args:
        print(__doc__)
        return
    cmd = int(args[0], 0)
    data = args[1].encode() if len(args) > 1 else b""
    resp, body = send_cmd(ip, cmd, data)
    name = {1: "OK", 2: "ERROR", 3: "DATA", 0x10: "PROGRESS"}.get(resp, hex(resp))
    print(f"[resp={name} len={len(body)}]")
    try:
        print(body.decode("utf-8", errors="replace"))
    except Exception:
        print(body)


if __name__ == "__main__":
    main()
