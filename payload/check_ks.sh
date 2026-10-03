#!/bin/bash
export SDK=/opt/ps5-payload-sdk
echo "=== Ποιο .so περιέχει kernel_get_ucred_authid (binary grep) ==="
for f in "$SDK"/target/lib/*.so; do
    if grep -aq 'kernel_get_ucred_authid' "$f" 2>/dev/null; then
        echo "  ✓ $(basename $f)"
    fi
done
echo ""
echo "=== Και libc.a (static) ==="
"$SDK/bin/llvm-nm" "$SDK/target/lib/libc.a" 2>/dev/null | grep -E 'kernel_get_ucred|kernel_set_ucred|kernel_get_root_vnode|kernel_set_proc_rootdir' | head -8
