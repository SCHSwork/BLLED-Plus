#!/usr/bin/env bash
# Turn the addresses from a "[Crash]" Activity log entry into code lines.
# Usage: tools/decode-crash.sh <BLLC_V...elf> 0x400d1234 0x400e5678 ...
# The .elf must come from the exact version (and edition) that crashed: each build writes it next
# to its .bin.ota, and any release can be rebuilt from its commit to get it again.
set -euo pipefail
ELF=$1; shift
A2L=$(command -v xtensa-esp32-elf-addr2line || ls "${ESP_WORKDIR:-$HOME/esp}"/xtensa-esp32-elf/bin/xtensa-esp32-elf-addr2line 2>/dev/null || ls /home/claude/esp/xtensa-esp32-elf/bin/xtensa-esp32-elf-addr2line)
"$A2L" -pfiaC -e "$ELF" "$@"
