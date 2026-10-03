#!/bin/bash
# Probe B: minimal + ONLY libkernel_sys (no Sce services)
# Probe C: minimal + ONLY the 3 Sce services (no libkernel_sys)
# Together with probeA (all libs) and hwinfo (sys sample) -> full bisection
SDK=/opt/ps5-payload-sdk
DESK="/mnt/c/Users/HACKMAN/Desktop"

mk_probe() {
    local NAME=$1
    shift
    local LIBS="$*"
    rm -rf "/tmp/p_$NAME" && mkdir -p "/tmp/p_$NAME" && cd "/tmp/p_$NAME"
    cat > probe.c <<'EOF'
#include <stdint.h>
#include <string.h>

typedef struct notify_request {
    char type;
    int reqId;
    int priority;
    int msgId;
    int unk1;
    char unk2[8];
    char use_icon_image_uri;
    char message[1024];
    char icon_image_uri[1024];
    char unk3[3075];
} notify_request_t;

int sceKernelSendNotificationRequest(const char* app_id, notify_request_t* req, size_t size, int unk);

int main() {
    notify_request_t req;
    memset(&req, 0, sizeof(req));
    strcpy(req.message, "Probe loaded OK!");
    sceKernelSendNotificationRequest("BOOT01102-NPXX51395_00", &req, sizeof(req), 0);
    return 0;
}
EOF
    $SDK/bin/prospero-clang -Wall -O3 \
        -I"$SDK/target/include_bsd" -I"$SDK/target/include" -L"$SDK/target/lib" \
        -o "probe$NAME.elf" probe.c $LIBS 2>&1 | head -5
    if [ -f "probe$NAME.elf" ]; then
        echo "[+] probe$NAME χτίστηκε:"
        readelf -d "probe$NAME.elf" | grep NEEDED
        $SDK/bin/llvm-strip "probe$NAME.elf"
        cp "probe$NAME.elf" "$DESK/probe${NAME}_TEST.elf"
    else
        echo "[-] probe$NAME ΔΕΝ χτίστηκε με libs: $LIBS"
    fi
}

mk_probe B -lkernel_sys
mk_probe C -lSceSystemService -lSceUserService -lSceAppInstUtil

echo ""
echo "=== Όλα τα probes στο Desktop ==="
ls -la "$DESK" | grep -E 'probe|hwinfo|server_diag|UNSTRIPPED'
