#!/bin/bash
# ---------------------------------------------------------------------------
# FanController_OTA_debug fordítása a Seeed XIAO ESP32-C3 boardra.
# A toolchaint a .claude/hooks/session-start.sh telepíti (Claude Code on the web).
# Helyi gépen futtatás előtt győződj meg róla, hogy az arduino-cli + esp32 core +
# OneButton + a custom partíció elérhető (lásd a hookot).
#
# Használat:
#   ./build.sh                # fordítás
#   ./build.sh --clean        # tiszta build
#   ./build.sh -v             # részletes kimenet
# ---------------------------------------------------------------------------
set -euo pipefail
export PATH="$HOME/.local/bin:$PATH"

SKETCH_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FQBN="esp32:esp32:XIAO_ESP32C3"
CORE_VERSION="3.1.3"
# app0 partíció mérete a partitions_custom.csv-ből (0x150000) → méretkorlát-ellenőrzés
MAX_APP_SIZE="1376256"

# A custom partíció elérhetővé tétele a core számára (idempotens).
PART_DIR="$HOME/.arduino15/packages/esp32/hardware/esp32/${CORE_VERSION}/tools/partitions"
if [ -d "$PART_DIR" ]; then
  cp -f "$SKETCH_DIR/partitions_custom.csv" "$PART_DIR/partitions_custom.csv"
fi

exec arduino-cli compile \
  --fqbn "$FQBN" \
  --build-property "build.partitions=partitions_custom" \
  --build-property "upload.maximum_size=${MAX_APP_SIZE}" \
  "$@" \
  "$SKETCH_DIR/FanController_OTA_debug.ino"
