#!/bin/bash
# Build 2 diagnostic probes + compare ELF structures
SDK=/opt/ps5-payload-sdk
DESK="/mnt/c/Users/HACKMAN/Desktop"
SUITE="/mnt/c/Users/HACKMAN/Desktop/ps5 test/my_projects/ps5_upload_suite"

echo "=== 0. Σύγκριση program headers: hello_world (δουλεύει) vs δικός μας ==="
echo "--- hello_world_TEST.elf:"
readelf -l "$DESK/hello_world_TEST.elf" | grep -E 'LOAD|INTERP' 
echo "--- ps5_upload_server.elf (δικός μας, stripped):"
readelf -l "$SUITE/payload/ps5_upload_server.elf" | grep -E 'LOAD|INTERP'

echo ""
echo "=== 1. libprobe_TEST.elf: notify body + ΟΛΕΣ οι δικες μας βιβλιοθήκες ==="
rm -rf /tmp/probe && mkdir -p /tmp/probe && cd /tmp/probe
cp "$SDK/samples/notify/main.c" .
$SDK/bin/prospero-clang -Wall -O3 -pthread \
    -I"$SDK/target/include_bsd" -I"$SDK/target/include" -L"$SDK/target/lib" \
    -o libprobe.elf main.c \
    -lkernel_sys -lSceSystemService -lSceUserService -lSceAppInstUtil 2>&1 | head -5
if [ -f libprobe.elf ]; then
    echo "[+] Χτίστηκε. DT_NEEDED:"
    readelf -d libprobe.elf | grep NEEDED
    cp libprobe.elf "$DESK/libprobe_TEST.elf"
fi

echo ""
echo "=== 2. ps5_upload_server_UNSTRIPPED.elf: ο δικός μας χωρίς llvm-strip ==="
cd "$SUITE/payload"
cp ps5_upload_server.elf /tmp/server_stripped_backup.elf
"$SDK/bin/prospero-clang" -Wall -O3 -pthread \
    -I"$SDK/target/include_bsd" -I"$SDK/target/include" -L"$SDK/target/lib" \
    -o ps5_upload_server_unstripped.elf main.c \
    -lkernel_sys -lSceSystemService -lSceUserService -lSceAppInstUtil
if [ -f ps5_upload_server_unstripped.elf ]; then
    ls -la ps5_upload_server_unstripped.elf
    cp ps5_upload_server_unstripped.elf "$DESK/ps5_upload_server_UNSTRIPPED.elf"
    echo "[+] Στο Desktop"
fi
