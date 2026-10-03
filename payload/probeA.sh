#!/bin/bash
# Probe A: minima main + exact same libs as our payload
SDK=/opt/ps5-payload-sdk
DESK="/mnt/c/Users/HACKMAN/Desktop"

rm -rf /tmp/probeA && mkdir -p /tmp/probeA && cd /tmp/probeA
cat > probe.c <<'EOF'
#include <stdint.h>
#include <string.h>
#include <stdio.h>

typedef struct notify_request {
    char type;              // 0
    int reqId;              // 4
    int priority;           // 8
    int msgId;              // 12
    int unk1;               // 16
    char unk2[8];           // 20
    char use_icon_image_uri; // 28
    char message[1024];     // 29
    char icon_image_uri[1024];
    char unk3[3075];
} notify_request_t;

int sceKernelSendNotificationRequest(const char* app_id, notify_request_t* req, size_t size, int unk);

void send_note(const char* msg) {
    notify_request_t req;
    memset(&req, 0, sizeof(req));
    req.type = 0;
    memset(req.unk2, 0, sizeof(req.unk2));
    req.use_icon_image_uri = 0;
    req.msgId = 0;
    strcpy(req.message, msg);
    sceKernelSendNotificationRequest("BOOT01102-NPXX51395_00", &req, sizeof(req), 0);
}

int main() {
    send_note("Probe A: libkernel_sys + SceSvc OK!");
    return 0;
}
EOF

$SDK/bin/prospero-clang -Wall -O3 \
    -I"$SDK/target/include_bsd" -I"$SDK/target/include" -L"$SDK/target/lib" \
    -o probeA.elf probe.c \
    -lkernel_sys -lSceSystemService -lSceUserService -lSceAppInstUtil 2>&1 | head -5
if [ -f probeA.elf ]; then
    echo "[+] probeA χτίστηκε. DT_NEEDED:"
    readelf -d probeA.elf | grep NEEDED
    $SDK/bin/llvm-strip probeA.elf
    cp probeA.elf "$DESK/probeA_TEST.elf"
    ls -la "$DESK/probeA_TEST.elf"
fi
