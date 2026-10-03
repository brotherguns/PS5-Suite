#!/bin/bash
# ELF structure inspection for etaHEN load debugging (system readelf — ELF is x86-64)
ELF="/mnt/c/Users/HACKMAN/Desktop/ps5 test/my_projects/ps5_upload_suite/payload/ps5_upload_server.elf"

echo "=== 1. ELF HEADER ==="
readelf -h "$ELF"

echo ""
echo "=== 2. PROGRAM HEADERS ==="
readelf -l "$ELF"

echo ""
echo "=== 3. DYNAMIC SECTION ==="
readelf -d "$ELF" | head -25
