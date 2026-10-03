#!/bin/bash
export SDK=/opt/ps5-payload-sdk
for lib in libkernel libkernel_web libkernel_sys; do
    echo "--- $lib:"
    "$SDK/bin/llvm-nm" -D "$SDK/target/lib/$lib.so" 2>/dev/null | grep -E 'kernel_get_ucred_authid|kernel_set_ucred_caps|kernel_set_proc_rootdir|kernel_get_proc_rootdir|kernel_get_ucred_sceProcType|kernel_get_ucred_sonyCred' | head -6
done
echo ""
echo "=== kernel.h full function list (the SDK ps5/kernel.h) ==="
grep -nE '^kernel_|^intptr_t kernel|^uint64_t kernel|^int32_t kernel|^uid_t kernel' "$SDK/target/include/ps5/kernel.h" | head -40
