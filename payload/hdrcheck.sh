#!/bin/bash
export SDK=/opt/ps5-payload-sdk
echo "=== libkernel_sys: ό,τι μοιάζει με cred/prison/jail/type ==="
"$SDK/bin/llvm-nm" -D "$SDK/target/lib/libkernel_sys.so" 2>/dev/null | awk '{print $3}' | grep -iE 'cred|prison|jail|proctype|sony|cap|authid' | head -30
echo ""
echo "=== kernel_* symbols ==="
"$SDK/bin/llvm-nm" -D "$SDK/target/lib/libkernel_sys.so" 2>/dev/null | awk '{print $3}' | grep '^kernel_' | head -30
echo ""
echo "=== Σύνολο εξαγωγών libkernel_sys ==="
"$SDK/bin/llvm-nm" -D "$SDK/target/lib/libkernel_sys.so" 2>/dev/null | grep -c ' T '
