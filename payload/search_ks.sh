#!/bin/bash
R=/home/hackman/.ref
echo "=== WHO runs jbc_loader? ==="
grep -rn 'jbc_loader\|payload_bin' "$R/jbc/ps5-kstuff/main.c" | head -5
echo ""
echo "=== ps5-kstuff main: elfldr / payload launch integration ==="
grep -n 'elfldr\|9021\|payload' "$R/jbc/ps5-kstuff/main.c" | head -12
echo ""
echo "=== jbc_loader Makefile deps (which lib) ==="
cat "$R/jbc/jbc_loader/Makefile"
echo ""
echo "=== Does elfldr payload in kstuff ecosystem jailbreak before spawning? ==="
grep -rn 'jbc\|jailbreak' "$R/jbc/ps5-kstuff/main.c" | head -10
