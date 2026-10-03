#!/bin/bash
DESK="/mnt/c/Users/HACKMAN/Desktop"
SUITE="/mnt/c/Users/HACKMAN/Desktop/ps5 test/my_projects/ps5_upload_suite"

echo "=== hello_world (ΔΟΥΛΕΥΕΙ) ==="
readelf -l "$DESK/hello_world_TEST.elf" | sed -n '/Program Headers/,/Section to Segment/p' | grep -E 'LOAD|Type|Flags|RWE|RW |R E'

echo ""
echo "=== notify (ΔΟΥΛΕΥΕΙ) ==="
readelf -l "$DESK/notify_TEST.elf" | grep -E 'LOAD' -A1 | grep -E 'LOAD|RWE|RW |R E'

echo ""
echo "=== hwinfo sample (libkernel_sys) ==="
readelf -l "$DESK/hwinfo_TEST.elf" | grep -E 'LOAD' -A1 | grep -E 'LOAD|RWE|RW |R E'

echo ""
echo "=== ΔΙΚΟΣ ΜΑΣ (ΑΠΟΤΥΧΑΝΕΙ) ==="
readelf -l "$SUITE/payload/ps5_upload_server.elf" | grep -E 'LOAD' -A1 | grep -E 'LOAD|RWE|RW |R E'
