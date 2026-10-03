#!/bin/bash
export SDK=/opt/ps5-payload-sdk
LIB="$SDK/target/lib/libkernel_web.so"
echo "=== mount symbols (raw) ==="
"$SDK/bin/llvm-nm" -D "$LIB" | grep -i mount
echo ""
echo "=== Our ELF's undefined mount symbols ==="
ELF="/mnt/c/Users/HACKMAN/Desktop/ps5 test/my_projects/ps5_upload_suite/payload/ps5_upload_server.elf"
"$SDK/bin/llvm-nm" -D "$ELF" | grep -i mount
