#!/bin/bash
SUITE="/mnt/c/Users/HACKMAN/Desktop/ps5 test/my_projects/ps5_upload_suite"
DESK="/mnt/c/Users/HACKMAN/Desktop"

# Copy new ELF to Desktop and release folder
cp "$SUITE/payload/ps5_upload_server.elf" "$DESK/ps5_upload_server.elf"
cp "$SUITE/payload/ps5_upload_server.elf" "$SUITE/release/v6.1.0/ps5_upload_server.elf"
cp "$SUITE/client-avalonia/publish/win-x64/PS5UploadSuite.exe" "$SUITE/release/v6.1.0/PS5UploadSuite-v6.1.0-Windows-x64.exe"

echo "=== Ενημερωμένα αρχεία ==="
ls -la "$DESK/ps5_upload_server.elf" "$SUITE/release/v6.1.0/ps5_upload_server.elf" "$SUITE/release/v6.1.0/PS5UploadSuite-v6.1.0-Windows-x64.exe"
echo ""
echo "=== Νέα SHA256 ==="
sha256sum "$SUITE/release/v6.1.0/ps5_upload_server.elf" "$SUITE/release/v6.1.0/PS5UploadSuite-v6.1.0-Windows-x64.exe" | awk '{print substr($1,1,16), $2}'
