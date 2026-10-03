#!/usr/bin/env python3
# PS5 Suite — full feature test harness.
# Exercises every payload command over the TCP protocol and reports PASS/FAIL.
# Usage: python test_all_features.py [ip]
import socket, struct, sys, time, os

HOST = sys.argv[1] if len(sys.argv) > 1 else '192.168.0.62'
RESP_OK, RESP_ERROR, RESP_DATA, RESP_READY, RESP_PROGRESS = 1, 2, 3, 4, 5

results = []

def report(name, ok, detail=''):
    results.append((name, ok, detail))
    print(('  PASS ' if ok else '  FAIL ') + name + (' — ' + detail if detail else ''))

def rex(s, n, timeout=20):
    s.settimeout(timeout)
    b = b''
    while len(b) < n:
        c = s.recv(n - len(b))
        if not c:
            raise IOError('conn closed')
        b += c
    return b

def cmd(sock, c, data=b'', timeout=25):
    sock.settimeout(timeout)
    sock.sendall(bytes([c]) + struct.pack('<I', len(data)) + data)
    prog = []
    while True:
        h = rex(sock, 5, timeout)
        r, l = h[0], struct.unpack('<I', h[1:])[0]
        d = rex(sock, l, timeout) if l else b''
        if r == RESP_PROGRESS:
            prog.append(d)
            continue
        return r, d, prog

def find_port():
    for p in range(9113, 9125):
        try:
            s = socket.create_connection((HOST, p), 2)
            s.sendall(bytes([0x01]) + struct.pack('<I', 0))
            h = s.recv(5)
            if h:
                s.close()
                return p
        except Exception:
            pass
    return None

PORT = find_port()
if not PORT:
    print('NO SERVER FOUND'); sys.exit(1)
print('=== PS5 Suite feature test — %s:%d ===' % (HOST, PORT))

def conn():
    return socket.create_connection((HOST, PORT), 5)

# ---------- read-only / info ----------
s = conn()
r, d, _ = cmd(s, 0x01); report('PING', r == RESP_OK or r == RESP_DATA, d[:60].decode(errors='replace'))

r, d, _ = cmd(s, 0x02); report('LIST_STORAGE', r in (RESP_OK, RESP_DATA), d[:80].decode(errors='replace'))

r, d, _ = cmd(s, 0x03, b'/data\x00'); report('LIST_DIR /data', r in (RESP_OK, RESP_DATA), '%d bytes' % len(d))

r, d, _ = cmd(s, 0x31, b'/data\x00'); report('GET_FILE_INFO', r in (RESP_OK, RESP_DATA), d[:60].decode(errors='replace'))

r, d, _ = cmd(s, 0x32, timeout=30); report('GET_SYSTEM_INFO', r in (RESP_OK, RESP_DATA), d[:80].decode(errors='replace'))

r, d, _ = cmd(s, 0x34, timeout=30); report('GET_HW_INFO', r in (RESP_OK, RESP_DATA), d[:80].decode(errors='replace'))

r, d, _ = cmd(s, 0x35); report('GET_TEMPS', r in (RESP_OK, RESP_DATA), d[:60].decode(errors='replace'))

r, d, _ = cmd(s, 0x36, timeout=30); report('GET_RUNNING_APPS', r in (RESP_OK, RESP_DATA), '%d bytes' % len(d))

r, d, _ = cmd(s, 0x39, timeout=30); report('GET_POWER_INFO', r in (RESP_OK, RESP_DATA), d[:60].decode(errors='replace'))

r, d, _ = cmd(s, 0x3A, timeout=40); report('GET_GAME_LIST', r in (RESP_OK, RESP_DATA), '%d bytes' % len(d))
game_list_data = d

r, d, _ = cmd(s, 0x47, timeout=30); report('GET_EXTENDED_INFO', r in (RESP_OK, RESP_DATA), '%d bytes' % len(d))

r, d, _ = cmd(s, 0x48, timeout=30); report('GET_CPU_USAGE', r in (RESP_OK, RESP_DATA), d[:60].decode(errors='replace'))

r, d, _ = cmd(s, 0x49, timeout=30); report('GET_MEMORY_INFO', r in (RESP_OK, RESP_DATA), d[:60].decode(errors='replace'))

r, d, _ = cmd(s, 0x4A, timeout=30); report('GET_MODULE_LIST', r in (RESP_OK, RESP_DATA), '%d bytes' % len(d))

r, d, _ = cmd(s, 0x3F, timeout=30); report('LIST_SAVES', r in (RESP_OK, RESP_DATA), '%d bytes' % len(d))

r, d, _ = cmd(s, 0x45, timeout=30); report('LIST_SCREENSHOTS', r in (RESP_OK, RESP_DATA), '%d bytes' % len(d))
shot_data = d

r, d, _ = cmd(s, 0x52); report('FAN_GET_THRESHOLD', r in (RESP_OK, RESP_DATA), d[:40].decode(errors='replace'))

r, d, _ = cmd(s, 0x51); report('PKG_INSTALL_STATUS', r in (RESP_OK, RESP_DATA), d[:40].decode(errors='replace'))

r, d, _ = cmd(s, 0x60, timeout=30); report('SAVE_SCAN', r in (RESP_OK, RESP_DATA), '%d bytes' % len(d))
save_scan = d

r, d, _ = cmd(s, 0x63); report('SAVE_MOUNT_STATUS', r in (RESP_OK, RESP_DATA), d[:60].decode(errors='replace'))

r, d, _ = cmd(s, 0x41); report('INDEX_STATUS', r in (RESP_OK, RESP_DATA), d[:60].decode(errors='replace'))
s.close()

# ---------- game icon/details/pic (needs a title id) ----------
title_id = None
try:
    txt = game_list_data.decode(errors='replace')
    for tk in txt.replace('\x00', ' ').replace('|', ' ').replace('\n', ' ').split():
        if len(tk) == 9 and tk[:4] in ('PPSA', 'CUSA', 'ELAS', 'SCES'):
            title_id = tk
            break
except Exception:
    pass
print('  (detected title_id: %s)' % title_id)

s = conn()
if title_id:
    for c, name in ((0x3C, 'GET_GAME_ICON'), (0x3D, 'GET_GAME_DETAILS')):
        r, d, _ = cmd(s, c, title_id.encode() + b'\x00', timeout=30)
        report(name, r in (RESP_OK, RESP_DATA), '%d bytes' % len(d))
    # GET_GAME_PIC wants TITLE_ID:TYPE (0=pic0/bg, 1=pic1/fg)
    r, d, _ = cmd(s, 0x3E, (title_id + ':0').encode() + b'\x00', timeout=30)
    report('GET_GAME_PIC', r in (RESP_OK, RESP_DATA), '%d bytes' % len(d))
else:
    for name in ('GET_GAME_ICON', 'GET_GAME_DETAILS', 'GET_GAME_PIC'):
        report(name, None, 'no title id detected — SKIPPED')

# ---------- fs write ops in /data/ps5suite_test ----------
s = conn()
r, d, _ = cmd(s, 0x04, b'/data/ps5suite_test\x00'); report('CREATE_DIR', r == RESP_OK, d[:60].decode(errors='replace'))

# upload: START(path\0+u64 size) -> READY, chunks (no resp), END -> OK
s.settimeout(20)
up = b'/data/ps5suite_test/hello.txt\x00' + struct.pack('<Q', 11)
r, d, _ = cmd(s, 0x10, up)
ok_ready = (r == RESP_READY or r == RESP_OK)
report('START_UPLOAD', ok_ready, 'resp %d' % r)
if ok_ready:
    s.sendall(bytes([0x11]) + struct.pack('<I', 11) + b'hello world')
    r, d, _ = cmd(s, 0x12); report('UPLOAD_CHUNK+END', r == RESP_OK, d[:60].decode(errors='replace'))

r, d, _ = cmd(s, 0x33, b'/data/ps5suite_test/hello.txt\x00', timeout=30); report('VERIFY_FILE', r in (RESP_OK, RESP_DATA), d[:60].decode(errors='replace'))

# download back
s.settimeout(20)
s.sendall(bytes([0x13]) + struct.pack('<I', len(b'/data/ps5suite_test/hello.txt\x00')) + b'/data/ps5suite_test/hello.txt\x00')
h = rex(s, 5)
if h[0] == 3:
    sz = struct.unpack('<Q', rex(s, 8))[0]
    data = rex(s, sz)
    report('DOWNLOAD_FILE', data == b'hello world', repr(data[:30]))
else:
    report('DOWNLOAD_FILE', False, 'resp %s' % h.hex())

src = b'/data/ps5suite_test/hello.txt\x00' + b'/data/ps5suite_test/hello2.txt\x00'
r, d, _ = cmd(s, 0x08, src); report('COPY_FILE', r == RESP_OK, d[:60].decode(errors='replace'))

src = b'/data/ps5suite_test/hello2.txt\x00' + b'/data/ps5suite_test/hello2_renamed.txt\x00'
r, d, _ = cmd(s, 0x07, src); report('RENAME', r == RESP_OK, d[:60].decode(errors='replace'))

src = b'/data/ps5suite_test/hello2_renamed.txt\x00' + b'/data/ps5suite_test/moved/hello2.txt\x00'
cmd(s, 0x04, b'/data/ps5suite_test/moved\x00')
r, d, _ = cmd(s, 0x09, src); report('MOVE_FILE', r == RESP_OK, d[:60].decode(errors='replace'))

r, d, _ = cmd(s, 0x05, b'/data/ps5suite_test/moved/hello2.txt\x00'); report('DELETE_FILE', r == RESP_OK, d[:60].decode(errors='replace'))

r, d, _ = cmd(s, 0x06, b'/data/ps5suite_test\x00', timeout=60); report('DELETE_DIR', r == RESP_OK, d[:60].decode(errors='replace'))

# ---------- shell ----------
r, d, _ = cmd(s, 0x20); report('SHELL_OPEN', r == RESP_OK, d[:60].decode(errors='replace'))
r, d, _ = cmd(s, 0x21, b'ls /data\x00', timeout=30); report('SHELL_EXEC ls', r in (RESP_OK, RESP_DATA), d[:60].decode(errors='replace'))
r, d, _ = cmd(s, 0x22); report('SHELL_INTERRUPT', r in (RESP_OK, RESP_DATA, RESP_ERROR), d[:40].decode(errors='replace'))
r, d, _ = cmd(s, 0x23); report('SHELL_CLOSE', r == RESP_OK, d[:40].decode(errors='replace'))
s.close()

# ---------- index ----------
s = conn()
r, d, _ = cmd(s, 0x40, b'/data/ps5suite_test2,/data\x00', timeout=15)
report('INDEX_START', r in (RESP_OK, RESP_DATA) or (r == RESP_ERROR and b'already' in d.lower()), d[:60].decode(errors='replace'))
time.sleep(2)
r, d, _ = cmd(s, 0x41); report('INDEX_STATUS(2)', r in (RESP_OK, RESP_DATA), d[:60].decode(errors='replace'))
r, d, _ = cmd(s, 0x42, b'hello\x00', timeout=20); report('SEARCH_INDEX', r in (RESP_OK, RESP_DATA), d[:80].decode(errors='replace'))
r, d, _ = cmd(s, 0x43); report('INDEX_CANCEL', r in (RESP_OK, RESP_DATA, RESP_ERROR), d[:40].decode(errors='replace'))
s.close()

# ---------- fan set + restore ----------
s = conn()
r, d, _ = cmd(s, 0x52)
orig = d
try:
    cur = int(''.join(ch for ch in d.decode(errors='replace') if ch.isdigit()))
except Exception:
    cur = None
if cur is not None:
    r, d, _ = cmd(s, 0x53, bytes([min(cur, 255)]))
    report('FAN_SET_THRESHOLD', r == RESP_OK, d[:60].decode(errors='replace'))
    cmd(s, 0x53, bytes([min(cur, 255)]))  # restore same
else:
    report('FAN_SET_THRESHOLD', None, 'no current value — SKIPPED')
s.close()

# ---------- saves (garlic flow): status -> mount -> browse -> unmount ----------
s = conn()
img = None
try:
    txt = save_scan.decode(errors='replace')
    for part in txt.replace('\x00', '|').split('|'):
        if 'sdimg' in part or part.endswith(('.img', '.bin')):
            img = part.strip()
            break
    if not img:
        for tk in txt.split():
            if 'sdimg' in tk:
                img = tk
                break
except Exception:
    pass
print('  (detected save image: %s)' % img)
if img:
    r, d, _ = cmd(s, 0x61, img.encode() + b'\x00', timeout=60)
    ok = (r == RESP_OK) and (b'ERROR' not in d.upper())
    report('SAVE_MOUNT', ok, d[:80].decode(errors='replace'))
    if ok:
        r, d, _ = cmd(s, 0x03, b'/data/save_mnt\x00')
        report('SAVE browse', r in (RESP_OK, RESP_DATA), '%d bytes' % len(d))
        r, d, _ = cmd(s, 0x62, b'\x00', timeout=60)
        report('SAVE_UNMOUNT', r == RESP_OK, d[:60].decode(errors='replace'))
else:
    report('SAVE_MOUNT', None, 'no save image parsed — check SAVE_SCAN output')

# ---------- mount/unmount games ----------
r, d, p = cmd(s, 0x30, b'\x00', timeout=120)
report('MOUNT_GAMES', r in (RESP_OK, RESP_DATA), d[:80].decode(errors='replace'))
r, d, _ = cmd(s, 0x3B, b'\x00', timeout=60)
report('UNMOUNT_GAME', r in (RESP_OK, RESP_DATA), d[:60].decode(errors='replace'))
s.close()

# ---------- error-path commands (safe bogus input) ----------
s = conn()
r, d, _ = cmd(s, 0x37, b'FAKEAPP01\x00'); report('KILL_APP(bogus)', r in (RESP_OK, RESP_ERROR), d[:60].decode(errors='replace'))
r, d, _ = cmd(s, 0x46, b'/nonexistent/shot.png\x00'); report('DELETE_SCREENSHOT(bogus)', r in (RESP_OK, RESP_ERROR), d[:60].decode(errors='replace'))
r, d, _ = cmd(s, 0x50, b'/nonexistent.pkg\x00', timeout=30); report('PKG_INSTALL(bogus)', r in (RESP_OK, RESP_ERROR), d[:60].decode(errors='replace'))
r, d, _ = cmd(s, 0x64, b'/nonexistent/sdimg_fake\x00'); report('SAVE_DELETE(bogus)', r in (RESP_OK, RESP_ERROR), d[:60].decode(errors='replace'))
s.close()

# ---------- summary ----------
passed = sum(1 for _, ok, _ in results if ok is True)
failed = sum(1 for _, ok, _ in results if ok is False)
skipped = sum(1 for _, ok, _ in results if ok is None)
print('\n=== SUMMARY: %d PASS, %d FAIL, %d SKIPPED ===' % (passed, failed, skipped))
for n, ok, det in results:
    if ok is False:
        print('   FAILED: %s (%s)' % (n, det))
