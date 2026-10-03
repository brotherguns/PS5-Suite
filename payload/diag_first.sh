#!/bin/bash
# DIAG variant: our server but with a notification as the VERY FIRST statement of main()
# Purpose: if this shows a notification on the PS5, the process starts fine and
#          something after it (init_workers etc.) fails; if nothing shows, spawn fails.
SDK=/opt/ps5-payload-sdk
SUITE="/mnt/c/Users/HACKMAN/Desktop/ps5 test/my_projects/ps5_upload_suite"
DESK="/mnt/c/Users/HACKMAN/Desktop"

rm -rf /tmp/diag && mkdir -p /tmp/diag && cd /tmp/diag
cp "$SUITE/payload/main.c" main.c

python3 - <<'PYEOF'
import re
src = open('main.c', 'r', encoding='utf-8', errors='replace').read()
marker = "int main() {"
inject = '''int main() {
    send_notification("DIAG v6.1: process started!");'''
assert marker in src, "main() marker not found"
src = src.replace(marker, inject, 1)
open('main_diag.c', 'w', encoding='utf-8').write(src)
print("[+] main_diag.c created")
PYEOF

$SDK/bin/prospero-clang -Wall -O3 -pthread \
    -I"$SDK/target/include_bsd" -I"$SDK/target/include" -L"$SDK/target/lib" \
    -o server_diag.elf main_diag.c \
    -lkernel_sys -lSceSystemService -lSceUserService -lSceAppInstUtil 2>&1 | head -8
if [ -f server_diag.elf ]; then
    $SDK/bin/llvm-strip server_diag.elf
    cp server_diag.elf "$DESK/server_diag_TEST.elf"
    ls -la "$DESK/server_diag_TEST.elf"
    echo "[+] server_diag_TEST.elf on Desktop"
fi
