#!/bin/bash
# ===========================================================================
# SessionStart hook — Claude Code on the web
# Telepíti az ESP32 / Arduino toolchaint, hogy a FanController_OTA_debug vázlat
# fordítható legyen (arduino-cli + esp32 core + OneButton + ctags + partíció).
#
# A futtatókörnyezet hálózati szabálya csak bizonyos hosztokat enged:
#   - github.com release-asset útvonalak  -> OK (ezeken jön minden bináris)
#   - raw.githubusercontent.com           -> OK (ESP32 board-index)
#   - downloads.arduino.cc                -> BLOKKOLT (default index, ctags, libek)
# Ezért a default Arduino indexet KERÜLJÜK: az arduino-cli-t, az ESP32 core
# eszközeit, a ctags-et és a OneButton-t mind GitHubról szerezzük be.
# ===========================================================================
set -uo pipefail

# Csak a webes (remote) környezetben fusson — lokálisan ne nyúljon a rendszerhez.
if [ "${CLAUDE_CODE_REMOTE:-}" != "true" ]; then
  exit 0
fi

ARDUINO_CLI_VERSION="1.1.1"
ESP32_CORE_VERSION="3.1.3"
ONEBUTTON_VERSION="2.6.1"
CTAGS_TAG="5.8-arduino11"
DFU_VERSION="0.11.0-arduino5"

A15="$HOME/.arduino15"
BIN_DIR="$HOME/.local/bin"
mkdir -p "$BIN_DIR"
export PATH="$BIN_DIR:$PATH"

# PATH perzisztálása a teljes session-re.
if [ -n "${CLAUDE_ENV_FILE:-}" ]; then
  echo "export PATH=\"$BIN_DIR:\$PATH\"" >> "$CLAUDE_ENV_FILE"
fi

echo "[hook] toolchain ellenőrzése/telepítése..."

# --- 1) arduino-cli (GitHub release) -------------------------------------
if ! command -v arduino-cli >/dev/null 2>&1; then
  echo "[hook]   arduino-cli $ARDUINO_CLI_VERSION letöltése..."
  curl -fsSL -o /tmp/arduino-cli.tar.gz \
    "https://github.com/arduino/arduino-cli/releases/download/v${ARDUINO_CLI_VERSION}/arduino-cli_${ARDUINO_CLI_VERSION}_Linux_64bit.tar.gz"
  tar -xzf /tmp/arduino-cli.tar.gz -C "$BIN_DIR" arduino-cli
  chmod +x "$BIN_DIR/arduino-cli"
fi

# --- 2) pyserial (az esptool.py függ tőle) -------------------------------
python3 -c "import serial" 2>/dev/null || pip3 install --user --quiet pyserial

# --- 3) arduino-cli config + indexek -------------------------------------
arduino-cli config init --overwrite >/dev/null 2>&1 || true
arduino-cli config add board_manager.additional_urls \
  "https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json" 2>/dev/null || true

# Helyi 'arduino' index a dfu-util FÜGGŐSÉG feloldásához. Az esp32 3.x deklarál
# egy arduino:dfu-util függőséget (csak S2/S3 DFU-feltöltéshez kell, C3 fordításhoz
# NEM). A default index blokkolt, ezért egy minimális helyi indexszel tesszük
# "ismertté" az arduino csomagot, a tényleges bináris helyett pedig egy dummy-t
# telepítünk elő (lásd lent), így letöltés nem is történik.
CDIR="$A15/custom_arduino"
if [ ! -f "$CDIR/package_arduino_index.json" ]; then
  mkdir -p "$CDIR/build"
  printf '#!/bin/sh\necho "dummy dfu-util (compile-only env)"\n' > "$CDIR/build/dfu-util"
  chmod +x "$CDIR/build/dfu-util"
  tar -cjf "$CDIR/dfu-util.tar.bz2" -C "$CDIR/build" dfu-util
  SZ=$(stat -c%s "$CDIR/dfu-util.tar.bz2")
  SH=$(sha256sum "$CDIR/dfu-util.tar.bz2" | awk '{print $1}')
  cat > "$CDIR/package_arduino_index.json" <<JSON
{ "packages":[ { "name":"arduino","maintainer":"Arduino","websiteURL":"https://www.arduino.cc/","email":"packages@arduino.cc","platforms":[],"tools":[ { "name":"dfu-util","version":"${DFU_VERSION}","systems":[ {"host":"x86_64-pc-linux-gnu","url":"file://$CDIR/dfu-util.tar.bz2","archiveFileName":"dfu-util.tar.bz2","checksum":"SHA-256:$SH","size":"$SZ"}, {"host":"x86_64-linux-gnu","url":"file://$CDIR/dfu-util.tar.bz2","archiveFileName":"dfu-util.tar.bz2","checksum":"SHA-256:$SH","size":"$SZ"}, {"host":"aarch64-linux-gnu","url":"file://$CDIR/dfu-util.tar.bz2","archiveFileName":"dfu-util.tar.bz2","checksum":"SHA-256:$SH","size":"$SZ"} ] } ] } ] }
JSON
fi
arduino-cli config add board_manager.additional_urls \
  "file://$CDIR/package_arduino_index.json" 2>/dev/null || true

# dummy dfu-util ELŐ-telepítése, hogy a core install ne próbálja letölteni
# (az arduino-cli nem támogatja a file:// sémát tool-letöltéshez).
DFU_DIR="$A15/packages/arduino/tools/dfu-util/${DFU_VERSION}"
if [ ! -x "$DFU_DIR/dfu-util" ]; then
  mkdir -p "$DFU_DIR"
  printf '#!/bin/sh\necho "dummy dfu-util (compile-only env)"\n' > "$DFU_DIR/dfu-util"
  chmod +x "$DFU_DIR/dfu-util"
fi

# A default (downloads.arduino.cc) index 403-at ad — ez nem kritikus (csak az
# AVR core + upload-discovery kellene belőle, fordításhoz nem).
arduino-cli core update-index >/dev/null 2>&1 || true

# --- 4) ESP32 core -------------------------------------------------------
if ! arduino-cli core list 2>/dev/null | grep -q "esp32:esp32[[:space:]]*${ESP32_CORE_VERSION}"; then
  echo "[hook]   esp32:esp32@${ESP32_CORE_VERSION} telepítése (nagy letöltés, ~300 MB)..."
  arduino-cli core install "esp32:esp32@${ESP32_CORE_VERSION}" >/dev/null 2>&1
fi

# --- 5) Arduino ctags (builtin tool; a default index blokkolt -> GitHub) --
CTAGS_DIR="$A15/packages/builtin/tools/ctags/${CTAGS_TAG}"
if [ ! -x "$CTAGS_DIR/ctags" ]; then
  echo "[hook]   ctags ${CTAGS_TAG} telepítése..."
  mkdir -p "$CTAGS_DIR"
  curl -fsSL -o /tmp/ctags.tar.bz2 \
    "https://github.com/arduino/ctags/releases/download/${CTAGS_TAG}/ctags-${CTAGS_TAG}-x86_64-pc-linux-gnu.tar.bz2"
  tar -xjf /tmp/ctags.tar.bz2 -C /tmp
  cp "$(find /tmp -maxdepth 2 -name ctags -type f | head -1)" "$CTAGS_DIR/ctags"
  chmod +x "$CTAGS_DIR/ctags"
fi

# --- 6) OneButton könyvtár (a library registry blokkolt -> GitHub) -------
LIBDIR="$(arduino-cli config get directories.user 2>/dev/null)/libraries"
LIBDIR="${LIBDIR:-$HOME/Arduino/libraries}"
if [ ! -d "$LIBDIR/OneButton" ]; then
  echo "[hook]   OneButton ${ONEBUTTON_VERSION} telepítése..."
  mkdir -p "$LIBDIR"
  curl -fsSL -o /tmp/onebutton.tar.gz \
    "https://github.com/mathertel/OneButton/archive/refs/tags/${ONEBUTTON_VERSION}.tar.gz"
  tar -xzf /tmp/onebutton.tar.gz -C /tmp
  rm -rf "$LIBDIR/OneButton"
  mv "/tmp/OneButton-${ONEBUTTON_VERSION}" "$LIBDIR/OneButton"
fi

# --- 7) custom partíció elérhetővé tétele a core számára -----------------
PART_DIR="$A15/packages/esp32/hardware/esp32/${ESP32_CORE_VERSION}/tools/partitions"
if [ -d "$PART_DIR" ] && [ -f "${CLAUDE_PROJECT_DIR:-.}/partitions_custom.csv" ]; then
  cp -f "${CLAUDE_PROJECT_DIR}/partitions_custom.csv" "$PART_DIR/partitions_custom.csv"
fi

echo "[hook] kész — fordítás:  ./build.sh"
