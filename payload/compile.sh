#!/bin/bash

echo "================================================"
echo "PS5 Upload Server - Compilation"
echo "By Manos"
echo "================================================"
echo ""

PS5_PAYLOAD_SDK="${PS5_PAYLOAD_SDK:-/opt/ps5-payload-sdk}"

if [ ! -x "$PS5_PAYLOAD_SDK/bin/prospero-clang" ]; then
    echo "[-] PS5 payload SDK not found at: $PS5_PAYLOAD_SDK"
    echo "    Install it with:"
    echo "      git clone https://github.com/ps5-payload-dev/sdk"
    echo "      cd sdk && make && sudo make install"
    exit 1
fi

echo "[+] Using SDK: $PS5_PAYLOAD_SDK"
echo "[+] Compiling..."
rm -f ps5_upload_server.elf
"$PS5_PAYLOAD_SDK/bin/prospero-clang" -Wall -O3 -pthread \
    -I"$PS5_PAYLOAD_SDK/target/include_bsd" \
    -I"$PS5_PAYLOAD_SDK/target/include" \
    -L"$PS5_PAYLOAD_SDK/target/lib" \
    -o ps5_upload_server.elf main.c \
    -lkernel_web

if [ -f "ps5_upload_server.elf" ]; then
    echo ""
    echo "================================================"
    echo "[+] SUCCESS! Compiled ps5_upload_server.elf"
    echo "================================================"
    echo ""
    ls -lh ps5_upload_server.elf
    file ps5_upload_server.elf

    # Optional auto-deploy: PS5_DEPLOY=1 bash compile.sh
    # Requires elfldr (port 9021) running on the console (SDK v0.36.1+)
    if [ "$PS5_DEPLOY" = "1" ]; then
        PS5_IP="${PS5_IP:-192.168.0.160}"
        echo ""
        echo "[+] Deploying to $PS5_IP:9021 ..."
        if "$PS5_PAYLOAD_SDK/bin/prospero-deploy" "http://$PS5_IP:9021" ps5_upload_server.elf; then
            echo "[+] Deployed! The payload should now be running on the PS5."
        else
            echo "[-] Deploy failed - is elfldr running on the PS5?"
        fi
    fi
else
    echo ""
    echo "================================================"
    echo "[-] COMPILATION FAILED"
    echo "================================================"
    exit 1
fi
