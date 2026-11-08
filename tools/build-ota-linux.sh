#!/usr/bin/env bash
# Builds a BLLED .bin.ota on Linux without PlatformIO, using only GitHub downloads:
# Arduino-ESP32 core 2.0.17, Espressif GCC 8.4.0 (esp-2021r2-patch5), pinned libraries, arduino-cli.
# Matches platformio.ini: esp32dev, min_spiffs layout, DIO 40 MHz. Usage: tools/build-ota-linux.sh <workdir>
set -euo pipefail
W=${1:-$HOME/esp}; REPO=$(cd "$(dirname "$0")/.." && pwd)
VER=$(sed -n 's/^custom_version = \([0-9.]*\).*/\1/p' "$REPO/platformio.ini")
# EDITION=lite builds the smaller edition for controllers on the original BLLED layout
EDITION=${EDITION:-full}; EXTRA="${EXTRA_FLAGS:-}"; SUFFIX=""
VER=${VER_OVERRIDE:-$VER}
if [ "$EDITION" = lite ]; then EXTRA="$EXTRA -DBLLED_LITE"; SUFFIX="-lite"; fi
mkdir -p "$W" && cd "$W"
[ -d esp32-2.0.17 ] || { curl -sSL -o core.zip https://github.com/espressif/arduino-esp32/releases/download/2.0.17/esp32-2.0.17.zip; unzip -q core.zip; }
[ -d xtensa-esp32-elf ] || curl -sSL https://github.com/espressif/crosstool-NG/releases/download/esp-2021r2-patch5/xtensa-esp32-elf-gcc8_4_0-esp-2021r2-patch5-linux-amd64.tar.gz | tar xz
command -v esptool.py >/dev/null || pip install esptool==4.7.0
[ -x arduino-cli ] || curl -sSL https://github.com/arduino/arduino-cli/releases/download/v1.1.1/arduino-cli_1.1.1_Linux_64bit.tar.gz | tar xz arduino-cli
mkdir -p libs sketchbook/hardware/espressif sketchbook/libraries fakectags
while read -r repo tag; do n=$(basename "$repo"); [ -d "libs/$n" ] || git clone -q --depth 1 -b "$tag" "https://github.com/$repo.git" "libs/$n"; ln -sfn "$W/libs/$n" "sketchbook/libraries/$n"; done <<'LIBS'
bblanchon/ArduinoJson v7.4.2
knolleary/pubsubclient v2.8
luc-github/ESP32SSDP 1.2.1
ESP32Async/AsyncTCP v3.5.0
ESP32Async/ESPAsyncWebServer v3.12.1
mathieucarbou/MycilaWebSerial v8.2.3
LIBS
ln -sfn "$W/esp32-2.0.17" sketchbook/hardware/espressif/esp32
printf '#!/bin/sh\nexit 0\n' > fakectags/ctags && chmod +x fakectags/ctags   # the .ino has no functions to prototype
./arduino-cli config init --overwrite >/dev/null && ./arduino-cli config set directories.user "$W/sketchbook"
(cd "$REPO" && python3 pre_build.py >/dev/null)
rm -rf BLLED && mkdir BLLED && cp -r "$REPO/src" BLLED/src
printf '#include <ArduinoJson.h> // listed so library detection always adds it\n' > BLLED/BLLED.ino
./arduino-cli compile --fqbn espressif:esp32:esp32:PartitionScheme=min_spiffs,FlashMode=dio,FlashFreq=40 \
  --build-property "runtime.tools.xtensa-esp32-elf-gcc.path=$W/xtensa-esp32-elf" \
  --build-property "runtime.tools.esptool_py.path=$(dirname "$(command -v esptool.py)")" \
  --build-property "runtime.tools.ctags.path=$W/fakectags" \
  --build-property "compiler.cpp.extra_flags=-DVERSION=$VER -DSTRVERSION=\"$VER\" -DCONFIG_ASYNC_TCP_STACK_SIZE=4096 $EXTRA" \
  --build-property "compiler.c.extra_flags=-DCONFIG_ASYNC_TCP_STACK_SIZE=4096" \
  --build-path "$W/build" BLLED
cp "$W/build/BLLED.ino.bin" "$REPO/BLLC_V$VER$SUFFIX.bin.ota"
echo "Built $REPO/BLLC_V$VER$SUFFIX.bin.ota ($(stat -c %s "$REPO/BLLC_V$VER$SUFFIX.bin.ota") bytes)"
