#!/bin/bash
# Diagnose etaHEN "Failed to load payload" — stage 2
SDK=/opt/ps5-payload-sdk

echo "=== 1. SupportsListing: τι σκάνει το etaHEN στο /data/etaHEN ==="
grep -rn "SupportsListing\|data/etaHEN" "$SDK/payloads/" 2>/dev/null | head -8

echo ""
echo "=== 2. Ο χάρτης με τα lazy stubs (AnonFuncs) — τα grep:['^sce[A-Z]'] ==="
grep -n "grep:" "$SDK/payloads/Makefile" | head -5
grep -n "AnonFuncs" "$SDK/payloads/jar" 2>/dev/null | head -3
ls "$SDK/payloads/" | head -20
