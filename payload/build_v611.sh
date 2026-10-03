#!/bin/bash
set -e
cd '/mnt/c/Users/HACKMAN/Desktop/ps5 test/my_projects/ps5_upload_suite/payload' || exit 1
make clean >/dev/null 2>&1 || true
make 2>&1 | tail -5
echo "--- DT_NEEDED ---"
readelf -d ps5_upload_server.elf | grep NEEDED
echo "--- SHA256 ---"
sha256sum ps5_upload_server.elf | awk '{print substr($1,1,16)}'
