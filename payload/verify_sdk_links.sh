#!/bin/bash
# Static verification: does our ELF's undefined imports resolve against SDK v0.43 libs?
SDK=/opt/ps5-payload-sdk
ELF="/mnt/c/Users/HACKMAN/Desktop/ps5 test/my_projects/ps5_upload_suite/payload/ps5_upload_server.elf"

# Only the libs the Makefile links against (plus libc for getloadavg etc.)
LIBS="$SDK/target/lib/libkernel_sys.so $SDK/target/lib/libSceSystemService.so $SDK/target/lib/libSceAppInstUtil.so $SDK/target/lib/libSceUserService.so $SDK/target/lib/libc.a"

echo "=== 1. sce/getloadavg imports tou ELF mas ==="
IMPORTS=$($SDK/bin/llvm-nm -D "$ELF" | grep ' U ' | awk '{print $2}' | grep -E '^(sce|getloadavg)')
echo "$IMPORTS"
echo ""

echo "=== 2. Se poio SDK lib lynetai kathena ==="
FAIL=0
for sym in $IMPORTS; do
    FOUND=""
    for LIB in $LIBS; do
        if nm -D "$LIB" 2>/dev/null | grep -qE " T $sym\$" || nm "$LIB" 2>/dev/null | grep -qE " T $sym\$"; then
            FOUND=$(basename "$LIB")
            break
        fi
    done
    if [ -z "$FOUND" ]; then
        echo "X $sym -> DEN VRETHIKE"
        FAIL=1
    else
        echo "OK $sym -> $FOUND"
    fi
done
echo ""
if [ $FAIL -eq 0 ]; then
    echo "APOTELESMA: OLA TA imports lynontai sto SDK v0.43"
else
    echo "APOTELESMA: YPARXOUN anelyta symvola"
fi
