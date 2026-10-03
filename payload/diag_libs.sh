#!/bin/bash
# Decisive test: is libkernel_sys.sprx the reason our ELF is rejected?
SDK=/opt/ps5-payload-sdk
SRC="/mnt/c/Users/HACKMAN/Desktop/ps5 test/my_projects/ps5_upload_suite/payload/main.c"
DESK="/mnt/c/Users/HACKMAN/Desktop"

echo "=== 1. Undefined sce symbols του payload μας ==="
SYMS=$($SDK/bin/llvm-nm -D "$SRC" 2>/dev/null | head -0)   # placeholder, use ELF below
ELF="/mnt/c/Users/HACKMAN/Desktop/ps5 test/my_projects/ps5_upload_suite/payload/ps5_upload_server.elf"
IMPORTS=$($SDK/bin/llvm-nm -D "$ELF" | grep ' U ' | awk '{print $2}' | grep -E '^(sce|kernel_|getloadavg)' | sort -u)
echo "$IMPORTS"

echo ""
echo "=== 2. Ποιο kernel stub τις εξάγει; ==="
for lib in libkernel libkernel_sys libkernel_web; do
    echo "--- $lib:"
    for sym in $IMPORTS; do
        if $SDK/bin/llvm-nm -D "$SDK/target/lib/$lib.so" 2>/dev/null | grep -q " $sym$"; then
            echo "    ✓ $sym"
        fi
    done
done

echo ""
echo "=== 3. Build του hwinfo sample (χρησιμοποιεί libkernel_sys!) ==="
rm -rf /tmp/hwi && cp -r "$SDK/samples/hwinfo" /tmp/hwi && cd /tmp/hwi
PS5_PAYLOAD_SDK=$SDK make 2>&1 | tail -1 && cp hwinfo.elf "$DESK/hwinfo_TEST.elf" && echo "[+] hwinfo_TEST.elf -> Desktop"

echo ""
echo "=== 4. Probe: μπορεί να χτιστεί το payload μας ΜΟΝΟ με libkernel (όχι sys); ==="
cd /tmp && rm -rf srvtest && mkdir srvtest && cd srvtest
$SDK/bin/prospero-clang -Wall -O3 -pthread \
    -I"$SDK/target/include_bsd" -I"$SDK/target/include" -L"$SDK/target/lib" \
    -o server_kernel.elf "$SRC" \
    -lkernel -lSceSystemService -lSceUserService -lSceAppInstUtil 2>&1 | head -10
if [ -f server_kernel.elf ]; then
    echo "[+] Χτίστηκε ΜΕ libkernel! -> Desktop/server_kernel_TEST.elf"
    $SDK/bin/llvm-strip server_kernel.elf
    cp server_kernel.elf "$DESK/server_kernel_TEST.elf"
    readelf -d server_kernel.elf | grep NEEDED
else
    echo "[-] Δεν χτίζεται με libkernel — κάποια symbols λείπουν (πάνω από)"
fi
