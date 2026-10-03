#!/bin/bash
export SDK=/opt/ps5-payload-sdk
ELF="/mnt/c/Users/HACKMAN/Desktop/ps5 test/my_projects/ps5_upload_suite/payload/ps5_upload_server.elf"
echo "=== mount-related undefined ==="
"$SDK/bin/llvm-nm" -D "$ELF" | grep ' U ' | grep -i mount
echo "---"
echo "=== πλήρης λίστα (όλα) ==="
"$SDK/bin/llvm-nm" -D "$ELF" | grep ' U ' | awk '{print $2}' | sort | head -120
