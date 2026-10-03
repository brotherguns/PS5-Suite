#!/bin/bash
SDK=/opt/ps5-payload-sdk
ELF="/mnt/c/Users/HACKMAN/Desktop/ps5 test/my_projects/ps5_upload_suite/payload/ps5_upload_server.elf"
echo "=== ΟΛΑ τα undefined του ELF μας ==="
"$SDK/bin/llvm-nm" -D "$ELF" | grep ' U ' | awk '{print $2}' | sort -u
echo ""
echo "=== Πλήθος ==="
"$SDK/bin/llvm-nm" -D "$ELF" | grep -c ' U '
