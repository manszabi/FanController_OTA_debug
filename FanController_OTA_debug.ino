// FanController_OTA_debug — XIAO ESP32-C3/C6 ventilator+gorgo vezerlo (BLE+OTA). Valtozasok: verhistory.md
#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <OneButton.h>
#include "esp_sleep.h"
#include "esp_task_wdt.h"
#include <Update.h>
#include "FS.h"    
#include "SPIFFS.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_log.h"
#include <Preferences.h>  // [FIX-ESP-21] NVS fokozat-mentés áramtalanításra

// ===================== DEBUG CONFIG =====================
#define DEBUG 1
#define OTA_DEBUG 0
#define BOOT_DIAG 1
#define FAN_SENSE_ENABLE 1

// A Serial-t csak akkor inicializáljuk, ha valamelyik kimeneti csatorna aktív
#if DEBUG || OTA_DEBUG || BOOT_DIAG
#define SERIAL_ENABLED 1
#else
#define SERIAL_ENABLED 0
#endif

// Ventilátorvezérlő debug: _P/_V = print (literál/érték), sima/_VLN = println (literál/érték)
#if DEBUG
#define DBG(x) Serial.println(F(x))
#define DBG_P(x) Serial.print(F(x))
#define DBG_V(...) Serial.print(__VA_ARGS__)
#define DBG_VLN(...) Serial.println(__VA_ARGS__)
#else
#define DBG(x)
#define DBG_P(x)
#define DBG_V(...)
#define DBG_VLN(...)
#endif

// OTA debug: ugyanaz a séma, külön kapcsolóval
#if OTA_DEBUG
#define OTA_DBG(x) Serial.println(F(x))
#define OTA_DBG_P(x) Serial.print(F(x))
#define OTA_DBG_V(...) Serial.print(__VA_ARGS__)
#define OTA_DBG_VLN(...) Serial.println(__VA_ARGS__)
#else
#define OTA_DBG(x)
#define OTA_DBG_P(x)
#define OTA_DBG_V(...)
#define OTA_DBG_VLN(...)
#endif

// ===================== VERSION INFO =====================
#define FIRMWARE_VERSION "7.13.0"
#define FIRMWARE_DATE "2026-06-21"

// ===================== PINS =====================      
#if defined(CONFIG_IDF_TARGET_ESP32C6)
#define RELAY_FAN1 23
#define RELAY_FAN2 22
#define RELAY_FAN3 21
#define RELAY_ROLLER 2
#define RELAY_EN 17
#define BUTTON_PIN 1
#define LED_YELLOW 0
#define LED_RED 16
#define RF_SWITCH_EN 3    // RF-kapcsoló engedélyezés (XIAO C6: WIFI_ENABLE), aktív LOW
#define ANT_SELECT 14     // antenna választó (XIAO C6: WIFI_ANT_CONFIG): HIGH=külső, LOW=belső
#else
#define RELAY_FAN1 10
#define RELAY_FAN2 9
#define RELAY_FAN3 8
#define RELAY_ROLLER 2
#define RELAY_EN 21
#define BUTTON_PIN 3
#define LED_YELLOW 5
#define LED_RED 4
#endif

// ===================== FAN RELÉ BONTÓ-ÉRINTKEZŐ FIGYELÉS (H11AA1M) =====================
// Az AC-érzékelés a relék BONTÓ (NC) érintkezőjén van. A ventilátor soros tekercsei
// miatt egy aktív fokozatnál minden ágon megjelenik az AC, ezért a kimenet (NO) figyelése
// nem megkülönböztethető — a bontó-érintkező adja, melyik relé NINCS behúzva.
// Detektálás: a H11AA1M a bemenetén lévő AC-ra LOW-t húz (vezet az opto), AC nélkül a
// felhúzó HIGH. Ezért az AC jelenlétét MINDIG a LOW mintából állapítjuk meg (volt-e LOW
// az ablakban) — ez független az opto-kimeneti RC-szűrőtől, így a szűrőkondi
// kiszáradása/szakadása esetén SEM ad téves eredményt (a régi HIGH-szint figyelés a
// nullátmeneti tüskéktől megzavarodott volna).
#if FAN_SENSE_ENABLE
#if defined(CONFIG_IDF_TARGET_ESP32C6)
#define FAN1_SENSE_PIN 19    // D? — Fan1 (RELAY_FAN1) bontó (NC) érintkezőjének figyelése
#define FAN2_SENSE_PIN 20    // D? — Fan2 (RELAY_FAN2) bontó (NC) érintkezőjének figyelése
#define FAN3_SENSE_PIN 18    // D? — Fan3 (RELAY_FAN3) bontó (NC) érintkezőjének figyelése
#else
#define FAN1_SENSE_PIN 6    // D4 — Fan1 (RELAY_FAN1) bontó (NC) érintkezőjének figyelése
#define FAN2_SENSE_PIN 7    // D5 — Fan2 (RELAY_FAN2) bontó (NC) érintkezőjének figyelése
#define FAN3_SENSE_PIN 20   // D7 — Fan3 (RELAY_FAN3) bontó (NC) érintkezőjének figyelése
#endif
// Mit jelent az AC a sense-ágon: 0 = NC (bontó) bekötés → AC ⇒ a relé NINCS behúzva
// (jelenlegi HW); 1 = NO (kimenet) bekötés → AC ⇒ a relé behúzva (régi HW).
#define FAN_SENSE_AC_MEANS_ENGAGED 0

const uint8_t fanSensePins[3] = { FAN1_SENSE_PIN, FAN2_SENSE_PIN, FAN3_SENSE_PIN };

const unsigned long AC_SENSE_WINDOW_MS = 40;
const unsigned long AC_SENSE_DEBOUNCE_MS = 80;
const unsigned long FAN_SENSE_GRACE_MS = 1500;
const unsigned long FAN_SENSE_MISMATCH_CONFIRM_MS = 1000;
#define FAN_SENSE_FAILSAFE_ON_STUCK 1   // STUCK → STATE_FAILSAFE (azonnal, türelmi idő után)
#define FAN_SENSE_WARN_ON_NOAC      1   // NOAC  → figyelmeztetés + diag.log (failsafe NÉLKÜL)

unsigned long fanSenseLastLow[3] = { 0, 0, 0 };       // utolsó LOW (AC-vezetés) minta ideje (ms)
bool fanRelayEngaged[3] = { false, false, false };        // SZŰRT állapot: TRUE = az adott relé behúzva (NC nyitva, a fokozat aktív)
unsigned long fanSenseChangeSince[3] = { 0, 0, 0 };   // mióta tér el a nyers a szűrttől (debounce)
bool fanSenseSeen[3] = { false, false, false };       // láttunk-e már valaha LOW (AC) mintát
unsigned long fanSenseGraceUntil = 0;                 // eddig nem értékelünk eltérést
unsigned long fanMismatchSince[3] = { 0, 0, 0 };      // NOAC: mióta áll fenn az eltérés (0 = nincs)
bool fanNoacWarned[3] = { false, false, false };      // NOAC: figyelmeztettünk-e már (ne spammeljen)
#endif  // FAN_SENSE_ENABLE

// ===================== FS / OTA DEFINES =====================
#define FLASH SPIFFS
#define FORMAT_SPIFFS_IF_FAILED true

#define OTA_NORMAL_MODE 0
#define OTA_UPDATE_MODE 1
#define OTA_INSTALL_MODE 2

static const size_t OTA_BUF_SIZE = 16384;
static uint8_t* otaBuf = nullptr;

// ===================== DIAG LOG (FIX-ESP-14) =====================
#define DIAG_LOG_PATH "/diag.log"
const size_t DIAG_LOG_MAX = 512;              // a napló max. mérete (byte) – kicsi
const uint32_t LOW_HEAP_THRESHOLD = 20000;    // ez alatt "kevés memória" bejegyzés
const size_t DIAG_CHUNK_SIZE = 20;            // BLE-n egy csomagban küldött byte
const unsigned long DIAG_CHUNK_INTERVAL = 25; // ms két csomag között (BLE flow control)

#define OTA_SERVICE_UUID "fb1e4001-54ae-4a28-9f74-dfccb248601d"
#define OTA_CHARACTERISTIC_UUID_RX "fb1e4002-54ae-4a28-9f74-dfccb248601d"
#define OTA_CHARACTERISTIC_UUID_TX "fb1e4003-54ae-4a28-9f74-dfccb248601d"

static const char* TAG = "OTA_BOOT";
static BLECharacteristic* pOtaTx = nullptr;
static BLECharacteristic* pOtaRx = nullptr;

static bool otaDeviceConnected = false;  // BLE OTA-kliens csatlakozva
static bool otaSendSize = true;           // küldjük-e a flash-méretet a kliensnek
static bool otaWriteFile = false;         // van-e CRC-OK, kiírásra váró part
static int otaWriteLen = 0;            // [FIX-ESP-38] az aktuális part hossza (egy buffer)
static int otaParts = 0, otaCur = 0, otaMTU = 0;  // összes part / aktuális part / part-méret
static int otaMode = OTA_NORMAL_MODE;     // OTA állapotgép: NORMAL / UPDATE / INSTALL
static bool otaCrcOk = true;              // CRC32 önteszt eredménye; FAIL esetén az OTA letiltva
unsigned long otaReceivedBytes = 0, otaTotalBytes = 0;  // eddig kiírt / várt összes byte
unsigned long otaLedTimer = 0;            // OTA-villogás időzítő
bool otaLedState = false;                 // OTA-villogás LED állapot

static uint32_t otaExpectedCrc = 0;    // a 0xFC-ben kapott elvárt CRC32
static int otaPartRetry = 0;           // aktuális part újraküldés-számláló
static const int MAX_PART_RETRY = 5;   // ennyi sikertelen CRC után abort
static int otaExpectedPart = 0;

bool otaPendingReboot = false;
unsigned long otaRebootAt = 0;

// [OTA health-check] true: frissen OTA-zott, még meg nem erősített (PENDING_VERIFY) firmware fut
bool otaPendingVerify = false;

bool otaInstallWaiting = false;
unsigned long otaInstallWaitUntil = 0;

volatile bool diagRequested = false;       // DIAG? parancs érkezett
volatile bool diagClearRequested = false;  // DIAGCLR parancs érkezett
bool diagStreaming = false;                // épp streamelünk-e
File diagFile;                             // nyitott naplófájl streamelés alatt
unsigned long diagLastChunk = 0;           // utolsó csomag ideje

// ===================== FAN / BLE STRUCTS =====================
portMUX_TYPE zoneMux = portMUX_INITIALIZER_UNLOCKED;

struct BleCommand {
  bool hasCommand;
  int zone;
  bool hasRollerCommand;
  int rollerCommand;
};

enum SystemState {
  STATE_NORMAL,
  STATE_FAILSAFE
};

SystemState currentState = STATE_NORMAL;  // fő állapotgép: NORMAL / FAILSAFE

unsigned long lastCheck = 0;
const unsigned long checkInterval = 20;    // állapotgép-lépés periódusa (ms)
unsigned long lastBlink = 0;
const unsigned long blinkInterval = 100;
bool blinkState = false;
unsigned long failStart = 0;               // failsafe belépés ideje (timeout-hoz)
bool failStartSet = false;

volatile BleCommand bleCmd = { false, 0, false, 0 };
portMUX_TYPE bleCmdMux = portMUX_INITIALIZER_UNLOCKED;

// ===================== TIMERS =====================
const unsigned long INACTIVITY_MS = 3600000;
const unsigned long RELAY_SWITCH_DELAY_MS = 10;
const unsigned long LED_BLINK_INTERVAL = 500;
const unsigned long HEARTBEAT_INTERVAL = 2000;
const unsigned long HEARTBEAT_PULSE = 100;
const unsigned long BLE_RESTART_DELAY = 500;

volatile bool zoneChanging = false;
volatile unsigned long bleDisconnectTime = 0;
unsigned long currentMillis = 0;
const unsigned long BLE_ZONE_TIMEOUT_MS = 720000;

// ===================== FAN BLE UUIDs =====================
#define SERVICE_UUID "0000ffe0-0000-1000-8000-00805f9b34fb"
#define CHARACTERISTIC_UUID "0000ffe1-0000-1000-8000-00805f9b34fb"

// ===================== BLE AUTH =====================
#define BLE_AUTH_PIN "123456"
#if !defined(BLE_AUTH_PIN)
#warning "BLE_AUTH_PIN is empty – authentication disabled!"
#endif
#define MAX_AUTH_ATTEMPTS 5
#define AUTH_LOCKOUT_TIME_MS 60000

bool isAuthenticated = false;
int authAttempts = 0;
unsigned long lockoutStart = 0;

// ===================== GLOBALS =====================
BLEServer* pServer = NULL;
BLECharacteristic* pCharacteristic = NULL;
volatile bool bleConnected = false;
volatile bool bleEnabled = true;

OneButton button(BUTTON_PIN, true, true);

int currentZone = 0;                   // aktív ventilátor fokozat (0=ki, 1..3)
int manualZoneIndex = 0;               // kézi módban a léptetett fokozat (dupla kattintás)
bool rollerActive = false;             // görgő-relé aktív
bool relaysEnabled = false;            // tápengedély (RELAY_EN) be
bool manualMode = false;               // kézi (gombos) mód, BLE nélkül
esp_reset_reason_t lastBootResetReason = ESP_RST_UNKNOWN;  // [FIX-ESP-19] boot reset-ok mentése

volatile unsigned long lastActivityTime = 0;
unsigned long lastRedToggle = 0;
unsigned long lastYellowToggle = 0;
unsigned long lastHeartbeat = 0;
unsigned long lastHeartbeat_red = 0;
bool redLedState = false;
bool yellowLedState = false;
bool heartbeatPulse = false;
bool heartbeatPulse_red = false;
volatile bool bleNeedsRestart = false;
volatile unsigned long bleRestartTime = 0;

bool zoneChangeInProgress = false;     // folyamatban lévő break-before-make fokozatváltás
unsigned long zoneChangeStart = 0;     // a váltás indításának ideje (ms)
int pendingZone = 0;                   // a váltás célfokozata (handleZoneChange élesíti)

unsigned long lastPrint1 = 0;
unsigned long lastPrint2 = 0;
unsigned long lastPrint3 = 0;
const unsigned long printInterval = 30000;
bool wasActive = false;

RTC_NOINIT_ATTR uint32_t bootMagic;
#define BOOT_MAGIC 0xDEADBEEF

RTC_NOINIT_ATTR uint32_t savedZoneMagic;
RTC_NOINIT_ATTR int savedZone;
#define SAVED_ZONE_MAGIC 0xFA11A5EE

RTC_NOINIT_ATTR uint32_t savedRollerMagic;
RTC_NOINIT_ATTR int savedRoller;       // 1 = aktív volt, 0 = nem
#define SAVED_ROLLER_MAGIC 0xF0117E55

// [FIX-ESP-39] Hibás-reset hurok-megszakító: gyors ismétlődő hibás resetnél a boot nem állít vissza → megszakad a brownout-hurok
RTC_NOINIT_ATTR uint32_t errRestoreMagic;
RTC_NOINIT_ATTR int errRestoreCount;       // egymást követő gyors hibás resetek száma (RTC)
#define ERR_RESTORE_MAGIC 0x10075EED
const int MAX_ERR_RESTORE = 3;                       // ennyiedik egymást követőnél már idle
const unsigned long ERR_RESTORE_CLEAR_MS = 30000;    // ennyi stabil futás után nullázzuk
const unsigned long OTA_VERIFY_HEALTHY_MS = 30000;   // OTA health-check: ennyi stabil futás után validál
bool errRestoreCleared = false;
bool restore_roller = false;

Preferences fanPrefs;
int nvsLastSavedZone = -1;             // amit utoljára NVS-be írtunk (cache, hogy ne írjunk feleslegesen)
int nvsLastSavedRoller = -1;           // [FIX-ESP-30] görgő NVS cache (-1 = nincs mentve)
unsigned long zoneStableSince = 0;     // mikortól stabil a jelenlegi fokozat
bool nvsZonePending = false;           // van-e még nem mentett stabil fokozat
const unsigned long NVS_SAVE_STABLE_MS = 30000;  // 30 mp stabilitás után mentünk
unsigned long lastNvsSaveTime = 0;     // mikor írtunk utoljára NVS-be
const unsigned long NVS_FORCE_SAVE_MS = 300000;  // 5 perc → kényszerített mentés

// ===================== COMMAND SOURCE PRIORITY =====================
enum CommandSource {
  SRC_NONE = 0,
  SRC_BLE = 1,
  SRC_BUTTON = 2,
};

CommandSource activeSource = SRC_NONE;
unsigned long sourceLockedUntil = 0;
const unsigned long SOURCE_LOCK_MS = 2000;

struct Timer {
  unsigned long last = 0;
  unsigned long interval = 0;

  bool elapsed(unsigned long now) {
    if ((unsigned long)(now - last) >= interval) {
      last = now;
      return true;
    }
    return false;
  }
};

// ===================== FORWARD DECLARATIONS =====================
void setFanZone(int zone, CommandSource source = SRC_NONE);
void activateRoller();
void deactivateRoller();
void enableRelays();
void disableRelays();
void handleLEDs(unsigned long currentMillis);
void enterDeepSleep(const char* reason);
void handleClick();
void handleLongPressStop();
void handleDoubleClick();
void handleMultiClick();
void handleZoneChange();
void handleBleCommand();
void stateMachineStep();
void normalMode();
void saveZoneToNvsIfStable();  // [FIX-ESP-21]
void zeroStateForFailsafe();   // [FIX-ESP-33] failsafe-állapot perzisztens nullázása
#if FAN_SENSE_ENABLE
void monitorFanRelays();       // [FIX-ESP-29] H11AA1M kimenet-mintavétel + szűrés
void checkFanRelayMismatch();  // [FIX-ESP-29] elvárt vs. mért → failsafe
#endif
void failSafeMode();
void ota_boot_flow();
void otaInitService(BLEServer* server);
void otaLoop();
void diagLog(const char* line);
void handleDiagRequest();
void printBootDiag();  // [FIX-ESP-28]
bool otaIsRunning() {
  return (otaMode != OTA_NORMAL_MODE);
}

// ===================== OTA HELPERS =====================
static uint32_t crc32_zlib(const uint8_t* p, size_t n) {
  uint32_t crc = 0xFFFFFFFF;
  for (size_t i = 0; i < n; i++) {
    crc ^= p[i];
    for (int k = 0; k < 8; k++) {
      crc = (crc >> 1) ^ (0xEDB88320u & (~(crc & 1u) + 1u));
    }
  }
  return ~crc;
}

static void otaAbort(const String& msg) {
  DBG_P("OTA abort: ");
  DBG_VLN(msg);
  char e[80];
  snprintf(e, sizeof(e), "[ota] abort: %.60s", msg.c_str());
  diagLog(e);
  if (pOtaTx) {
    String result = String((char)0x0F) + "ERR: " + msg;
    pOtaTx->setValue(result.c_str());
    pOtaTx->notify();
    delay(200);
  }
  if (FLASH.exists("/update.bin")) FLASH.remove("/update.bin");
  if (otaBuf) { free(otaBuf); otaBuf = nullptr; }  // [FIX-ESP-38] buffer felszabadítása
  otaMode = OTA_NORMAL_MODE;
  otaReceivedBytes = 0;
  otaTotalBytes = 0;
  otaParts = 0;
  otaCur = 0;
  otaMTU = 0;
  otaWriteLen = 0;
  otaWriteFile = false;
  otaPartRetry = 0;
  otaExpectedPart = 0;
}

static void rebootEspWithReason(String reason) {
  DBG("Rebooting...");
  delay(1000);
  ESP.restart();
}

static void otaWriteBinary(fs::FS& fs, const char* path, uint8_t* dat, int len) {
  OTA_DBG_P("FS write len=");
  OTA_DBG_VLN(len);

  File file = fs.open(path, FILE_APPEND);
  if (!file) {
    DBG("FS write fail");
    otaWriteFile = false;
    return;
  }
  size_t written = file.write(dat, len);
  file.close();
  otaWriteFile = false;
  otaReceivedBytes += written;  // [FIX-ESP-4] 2026-05-24: a TÉNYLEGESEN kiírt
  OTA_DBG_P("FS write done, total=");
  OTA_DBG_VLN(otaReceivedBytes);

  if (written < (size_t)len) {
    DBG_P("SPIFFS full! Wrote ");
    DBG_V(written);
    DBG_P(" of ");
    DBG_V(len);
    DBG_P(" bytes (SPIFFS free: ");
    DBG_V(FLASH.totalBytes() - FLASH.usedBytes());
    DBG(")");
    DBG("Aborting OTA");

    otaMode = OTA_NORMAL_MODE;
    otaInstallWaiting = false;
    otaInstallWaitUntil = 0;
    otaReceivedBytes = 0;
    otaTotalBytes = 0;
    otaParts = 0;
    otaCur = 0;
    otaMTU = 0;
    otaWriteLen = 0;
    otaPartRetry = 0;
    if (otaBuf) { free(otaBuf); otaBuf = nullptr; }  // [FIX-ESP-38]

    if (fs.exists(path)) {
      fs.remove(path);
      DBG("Partial update.bin removed");
    }

    if (pOtaTx) {
      String result = String((char)0x0F) + "ERR: SPIFFS full";
      pOtaTx->setValue(result.c_str());
      pOtaTx->notify();
      delay(200);
    }
  }
}

void ota_boot_flow() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  const esp_partition_t* boot = esp_ota_get_boot_partition();

  DBG("=== OTA BOOT FLOW ===");

  DBG_P("Running partition: type=");
  DBG_V(running->type);
  DBG_P(" subtype=");
  DBG_V(running->subtype);
  DBG_P(" address=0x");
  DBG_VLN(running->address, HEX);

  if (running != boot) {
    DBG_P("Boot partition: type=");
    DBG_V(boot->type);
    DBG_P(" subtype=");
    DBG_V(boot->subtype);
    DBG_P(" address=0x");
    DBG_VLN(boot->address, HEX);

    DBG("New firmware booted FIRST TIME");
  }

  esp_ota_img_states_t state;
  esp_err_t st = esp_ota_get_state_partition(running, &state);

  if (st == ESP_OK) {
    const char* stName;
    switch (state) {
      case ESP_OTA_IMG_NEW:            stName = "NEW"; break;
      case ESP_OTA_IMG_PENDING_VERIFY: stName = "PENDING_VERIFY"; break;
      case ESP_OTA_IMG_VALID:          stName = "VALID"; break;
      case ESP_OTA_IMG_INVALID:        stName = "INVALID"; break;
      case ESP_OTA_IMG_ABORTED:        stName = "ABORTED"; break;
      default:                         stName = "UNDEFINED"; break;
    }
    DBG_P("OTA image state: ");
    DBG_V(stName);
    DBG_P(" (0x");
    DBG_V(state, HEX);
    DBG(")");

    // Health-check: NE validáljuk most — a loop/enterDeepSleep majd, stabil futás után (itt a SPIFFS sincs még mountolva)
    if (state == ESP_OTA_IMG_PENDING_VERIFY) {
      otaPendingVerify = true;
      DBG("PENDING_VERIFY → health-check: validalas stabil futas utan");
    }
  } else {
    DBG_P("Failed to read OTA state: ");
    DBG_VLN(esp_err_to_name(st));
  }

  DBG("=== OTA BOOT FLOW END ===");
}

void sendOtaResult(String result) {
  if (!pOtaTx) return;
  pOtaTx->setValue(result.c_str());
  pOtaTx->notify();
  delay(200);
}

void performUpdate(Stream& updateSource, size_t updateSize) {
  String result = String((char)0x0F);

  DBG("=== OTA DEBUG START ===");

  DBG("WDT delete (flash write may block)...");
  esp_task_wdt_delete(NULL);

  const esp_partition_t* running = esp_ota_get_running_partition();
  const esp_partition_t* next = esp_ota_get_next_update_partition(NULL);

  DBG("Running partition:");
  DBG_P("  addr=0x"); DBG_VLN(running->address, HEX);
  DBG_P(" size="); DBG_V(running->size);
  DBG_P(" label="); DBG_VLN(running->label);

  DBG("Next OTA partition:");
  DBG_P("  addr=0x"); DBG_VLN(next->address, HEX);
  DBG_P(" size="); DBG_V(next->size);
  DBG_P(" label="); DBG_VLN(next->label);

  DBG_P("updateSize = ");
  DBG_VLN(updateSize);

  int magic = updateSource.peek();
  DBG_P("First byte (magic) = 0x");
  DBG_VLN(magic, HEX);
  if (magic != 0xE9) {
    DBG("ERR: bad firmware magic (not 0xE9)");
    char m[40];
    snprintf(m, sizeof(m), "[ota] bad magic=0x%02X size=%u", (unsigned)(magic & 0xFF), (unsigned)updateSize);
    diagLog(m);

    result += "ERR: rossz firmware (magic=0x";
    char hx[4];
    snprintf(hx, sizeof(hx), "%02X", (unsigned)(magic & 0xFF));
    result += hx;
    result += ", nem app .bin)";
    DBG("=== OTA DEBUG END ===");

    esp_task_wdt_add(NULL);
    sendOtaResult(result);
    return;
  }

  DBG("Calling Update.begin()...");
  bool ok = Update.begin(updateSize);
  if (!ok) {
    DBG("Update.begin FAILED!");
    DBG_P("Error code: "); DBG_VLN(Update.getError());
    DBG_P("Error string: "); DBG_VLN(Update.errorString());

    result += "Update.begin FAILED: ";
    result += Update.errorString();
    DBG("=== OTA DEBUG END ===");

    esp_task_wdt_add(NULL);
    sendOtaResult(result);
    return;
  }

  DBG("Update.begin OK");

  DBG("Calling Update.writeStream...");
  size_t written = Update.writeStream(updateSource);

  DBG_P("Update.writeStream returned: ");
  DBG_VLN(written);

  if (written != updateSize) {
    DBG("WARNING: written != updateSize");
    DBG_P("Expected: "); DBG_VLN(updateSize);
    DBG_P("Got: "); DBG_VLN(written);
  }

  DBG("Calling Update.end()...");
  bool endOK = Update.end();

  DBG_P("Update.end() returned: ");
  DBG_VLN(endOK ? "true" : "false");

  if (!endOK) {
    DBG("Update.end FAILED");
    DBG_P("Error code: "); DBG_VLN(Update.getError());
    DBG_P("Error string: "); DBG_VLN(Update.errorString());

    result += "Update.end FAILED: ";
    result += Update.errorString();
    DBG("=== OTA DEBUG END ===");

    esp_task_wdt_add(NULL);
    sendOtaResult(result);
    return;
  }

  DBG_P("Update.isFinished(): ");
  DBG_VLN(Update.isFinished() ? "true" : "false");

  if (!Update.isFinished()) {
    DBG("ERROR: Update not finished!");
  }

  DBG("=== OTA DEBUG END ===");

  DBG("WDT add back");
  esp_task_wdt_add(NULL);

  result += "Written: " + String(written) + "/" + String(updateSize) + "\n";
  result += "OTA done\n";

  if (otaDeviceConnected) {
    DBG("BLE connected → sending OTA result + scheduling reboot");
    sendOtaResult(result);
    otaPendingReboot = true;
    otaRebootAt = millis() + 5000;
  } else {
    DBG("No BLE → immediate reboot");
    rebootEspWithReason("OTA done");
  }
}

void updateFromFS(fs::FS& fs) {
  File updateBin = fs.open("/update.bin");
  if (updateBin) {
    if (updateBin.isDirectory()) {
      DBG("update.bin is dir");
      updateBin.close();
      return;
    }

    size_t updateSize = updateBin.size();

    if (updateSize > 0) {
      DBG("Start OTA from FS");
      performUpdate(updateBin, updateSize);
    } else {
      DBG("update.bin empty");
    }

    updateBin.close();

    DBG("Removing update.bin");
    fs.remove("/update.bin");

  } else {
    DBG("update.bin not found");
  }
}

// ===================== BLE SERVER CALLBACKS =====================
class MyServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) {
    bleConnected = true;
    otaDeviceConnected = true;
    DBG("BLE connected");
    bleDisconnectTime = 0;
  };

  void onDisconnect(BLEServer* pServer) {
    bleConnected = false;
    otaDeviceConnected = false;
    isAuthenticated = false;
    authAttempts = 0;
    lockoutStart = 0;
    bleDisconnectTime = millis();
    DBG("BLE disconnected");

    if (diagStreaming) {
      if (diagFile) diagFile.close();
      diagStreaming = false;
    }
    diagRequested = false;
    diagClearRequested = false;

    if (otaMode != OTA_NORMAL_MODE) {
      DBG("OTA interrupted – resetting OTA state");
      otaMode = OTA_NORMAL_MODE;
      otaInstallWaiting = false;
      otaInstallWaitUntil = 0;
      otaPendingReboot = false;
      otaRebootAt = 0;
      otaReceivedBytes = 0;
      otaTotalBytes = 0;
      otaWriteFile = false;
      otaPartRetry = 0;
      otaExpectedPart = 0;
      otaParts = 0;
      otaCur = 0;
      otaMTU = 0;
      otaWriteLen = 0;
      if (otaBuf) { free(otaBuf); otaBuf = nullptr; }  // [FIX-ESP-38]
      if (FLASH.exists("/update.bin")) {
        FLASH.remove("/update.bin");
        DBG("Incomplete update.bin removed");
      }
    }

    if (bleEnabled) {
      portENTER_CRITICAL(&bleCmdMux);
      bleNeedsRestart = true;
      bleRestartTime = 0;
      portEXIT_CRITICAL(&bleCmdMux);
    }
  }
};

// ===================== FAN CHARACTERISTIC CALLBACKS =====================
class MyCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pCharacteristic) {
    if (!bleConnected) {
      DBG("BLE cmd rejected (no conn)");
      return;
    }

    String val = pCharacteristic->getValue();
    val.trim();

    if (val.length() == 0) return;

    DBG_P("BLE cmd: ");
    if (val.startsWith("AUTH:")) {
      DBG("AUTH:****");
    } else {
      DBG_VLN(val);
    }

    if (val.startsWith("AUTH:")) {
      if (lockoutStart != 0 && millis() - lockoutStart < AUTH_LOCKOUT_TIME_MS) {
        DBG("Auth locked");
        pCharacteristic->setValue("AUTH_LOCKED");
        pCharacteristic->notify();
        return;
      }

      String receivedPin = val.substring(5);
      String correctPin = BLE_AUTH_PIN;

      if (correctPin.length() == 0 || receivedPin == correctPin) {
        isAuthenticated = true;
        authAttempts = 0;
        DBG("Auth OK");
        pCharacteristic->setValue("AUTH_OK");
        pCharacteristic->notify();
      } else {
        authAttempts++;
        DBG("Auth failed");
        if (authAttempts >= MAX_AUTH_ATTEMPTS) {
          lockoutStart = millis();
          DBG("Auth lockout");
          pCharacteristic->setValue("AUTH_LOCKED");
        } else {
          pCharacteristic->setValue("AUTH_FAIL");
        }
        pCharacteristic->notify();
      }

    } else if (val.startsWith("LEVEL:")) {
      String correctPin = BLE_AUTH_PIN;
      if (correctPin.length() > 0 && !isAuthenticated) {
        DBG("LEVEL rejected (no auth)");
        pCharacteristic->setValue("AUTH_REQUIRED");
        pCharacteristic->notify();
        return;
      }

      if (val.length() != 7 || !isDigit(val.charAt(6))) {
        DBG("Invalid zone value");
        return;
      }

      int zone = val.charAt(6) - '0';

      if (zone > 3 || zone < 0) {
        DBG("Zone out of range");
        return;
      }

      portENTER_CRITICAL(&bleCmdMux);
      bleCmd.zone = zone;
      bleCmd.hasCommand = true;
      portEXIT_CRITICAL(&bleCmdMux);

      DBG_P("Zone queued: ");
      DBG_VLN(zone);

    } else if (val.startsWith("ROLLER:")) {
      String correctPin = BLE_AUTH_PIN;
      if (correctPin.length() > 0 && !isAuthenticated) {
        DBG("ROLLER rejected (no auth)");
        pCharacteristic->setValue("AUTH_REQUIRED");
        pCharacteristic->notify();
        return;
      }

      if (val.length() != 8 || !isDigit(val.charAt(7))) {
        DBG("Invalid roller value");
        return;
      }

      int rollerCmd = val.charAt(7) - '0';

      if (rollerCmd != 0 && rollerCmd != 1) {
        DBG("Roller must be 0/1");
        return;
      }

      portENTER_CRITICAL(&bleCmdMux);
      bleCmd.rollerCommand = rollerCmd;
      bleCmd.hasRollerCommand = true;
      portEXIT_CRITICAL(&bleCmdMux);

      DBG_P("Roller queued: ");
      DBG_VLN(rollerCmd);

    } else if (val.startsWith("DIAG?")) {
      String correctPin = BLE_AUTH_PIN;
      if (correctPin.length() > 0 && !isAuthenticated) {
        DBG("DIAG rejected (no auth)");
        pCharacteristic->setValue("AUTH_REQUIRED");
        pCharacteristic->notify();
        return;
      }
      diagRequested = true;
      DBG("Diag log requested");

    } else if (val.startsWith("DIAGCLR")) {
      String correctPin = BLE_AUTH_PIN;
      if (correctPin.length() > 0 && !isAuthenticated) {
        DBG("DIAGCLR rejected (no auth)");
        pCharacteristic->setValue("AUTH_REQUIRED");
        pCharacteristic->notify();
        return;
      }
      diagClearRequested = true;
      DBG("Diag clear requested");

    } else {
      DBG("Unknown BLE cmd");
    }
  }
};

// ===================== OTA CHARACTERISTIC CALLBACKS =====================
class OtaCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pCharacteristic) {
    uint8_t* pData = pCharacteristic->getData();
    int len = pCharacteristic->getValue().length();
    if (pData == NULL || len == 0) return;

    OTA_DBG("OTA packet");

    if (pData[0] == 0xFB) {
      if (otaBuf) {
        int base = pData[1] * otaMTU;
        for (int x = 0; x < len - 2; x++) {
          if ((base + x) < (int)OTA_BUF_SIZE) otaBuf[base + x] = pData[x + 2];
        }
      }

    } else if (pData[0] == 0xFC) {
      OTA_DBG_P("0xFC part=");
      OTA_DBG_VLN((pData[3] * 256) + pData[4]);
      if (len < 9) {
        DBG("0xFC too short (no CRC) — re-requesting part");
        otaPartRetry++;
        if (otaPartRetry <= MAX_PART_RETRY && pOtaTx) {
          uint8_t rq[] = { 0xF1, (uint8_t)(otaExpectedPart / 256), (uint8_t)(otaExpectedPart % 256) };
          pOtaTx->setValue(rq, 3);
          pOtaTx->notify();
        } else {
          otaAbort("0xFC truncated");
        }
      } else {
        otaWriteLen = (pData[1] * 256) + pData[2];
        otaExpectedCrc = ((uint32_t)pData[5] << 24) | ((uint32_t)pData[6] << 16) |
                         ((uint32_t)pData[7] << 8) | ((uint32_t)pData[8]);
        otaCur = (pData[3] * 256) + pData[4];
        otaWriteFile = true;
      }

    } else if (pData[0] == 0xFD) {
      if (FLASH.exists("/update.bin")) {
        FLASH.remove("/update.bin");
      }

    } else if (pData[0] == 0xFE) {
      otaReceivedBytes = 0;
      otaTotalBytes = ((uint32_t)pData[1] << 24) | ((uint32_t)pData[2] << 16) | ((uint32_t)pData[3] << 8) | ((uint32_t)pData[4]);
      uint32_t fsFree = FLASH.totalBytes() - FLASH.usedBytes();
      DBG_P("FS free: ");
      DBG_VLN(fsFree);
      DBG_P("OTA size: ");
      DBG_VLN(otaTotalBytes);

      const uint32_t SPIFFS_OVERHEAD = 4096;
      if (otaTotalBytes + SPIFFS_OVERHEAD > fsFree) {
        DBG("ERR: SPIFFS too small for OTA");
        DBG_P("Need (with overhead): ");
        DBG_VLN(otaTotalBytes + SPIFFS_OVERHEAD);
        DBG_P("Available: ");
        DBG_VLN(fsFree);

        if (pOtaTx) {
          String result = String((char)0x0F) + "ERR: SPIFFS too small (need " + String(otaTotalBytes + SPIFFS_OVERHEAD) + ", have " + String(fsFree) + ")";
          pOtaTx->setValue(result.c_str());
          pOtaTx->notify();
          delay(200);
        }

        otaMode = OTA_NORMAL_MODE;
        otaTotalBytes = 0;
        otaReceivedBytes = 0;
        return;
      }

    } else if (pData[0] == 0xFF) {
      otaParts = (pData[1] * 256) + pData[2];
      otaMTU = (pData[3] * 256) + pData[4];
      otaCur = 0;
      otaWriteFile = false;
      otaPartRetry = 0;
      otaExpectedPart = 0;
      if (!otaBuf) otaBuf = (uint8_t*)malloc(OTA_BUF_SIZE);
      if (!otaBuf) {
        DBG("OTA abort: malloc fail (no RAM)");
        otaAbort("no RAM for OTA");
      } else {
        otaMode = OTA_UPDATE_MODE;
        DBG_P("OTA parts: ");
        DBG_VLN(otaParts);
        if (pOtaTx) {
          uint8_t rq[] = { 0xF1, 0x00, 0x00 };
          pOtaTx->setValue(rq, 3);
          pOtaTx->notify();
        }
      }

    } else if (pData[0] == 0xEF) {
      FLASH.format();
      otaSendSize = true;
    }
  }
};

// ===================== BUTTON HANDLERS =====================
void handleClick() {
  if (otaIsRunning()) return;

  DBG("Button: click");

  if (!rollerActive) {
    enableRelays();
    delay(100);
    activateRoller();
  } else if (currentZone != 0) {
    // Aktív ventilátor → első gombnyomás csak a ventilátort állítja le (görgő/relé marad); a következő kattintás kapcsol ki mindent
    manualZoneIndex = 0;
    setFanZone(0, SRC_BUTTON);
  } else {
    deactivateRoller();
    delay(100);
    disableRelays();
  }
}

void handleLongPressStop() {
  if (otaIsRunning()) return;

  DBG("Button: long → sleep");
  enterDeepSleep("button-longpress");
}

void handleDoubleClick() {
  if (otaIsRunning()) return;

  bleEnabled = false;
  DBG("Button: double");

  if (!manualMode) {
    manualMode = true;
    DBG("Manual mode ON");

    if (bleConnected) {
      pServer->disconnect(0);
      delay(100);
    }

    BLEDevice::stopAdvertising();
    bleConnected = false;

    manualZoneIndex = 1;
    setFanZone(manualZoneIndex, SRC_BUTTON);

  } else {
    manualZoneIndex = (manualZoneIndex + 1) % 4;
    setFanZone(manualZoneIndex, SRC_BUTTON);
  }
}

void handleMultiClick() {
  if (otaIsRunning()) return;

  int clicks = button.getNumberClicks();
  if (clicks > 2) {
    DBG("Multi-click → AUTO mode");

    manualMode = false;
    bleEnabled = true;

    manualZoneIndex = 0;
    setFanZone(0, SRC_BUTTON);

    BLEDevice::startAdvertising();
    DBG("Manual mode OFF, BLE advertising restarted");
    return;
  }
}

// ===================== OTA SERVICE INIT =====================
void otaInitService(BLEServer* server) {
  if (!otaCrcOk) {
    // CRC32 önteszt elhasalt → nem regisztráljuk az OTA szolgáltatást.
    // Így a firmware-frissítés nem indítható, de az eszköz egyébként fut.
    DBG("OTA service NOT started: CRC32 self-test failed");
    return;
  }

  BLEService* pOtaService = server->createService(OTA_SERVICE_UUID);

  pOtaTx = pOtaService->createCharacteristic(
    OTA_CHARACTERISTIC_UUID_TX,
    BLECharacteristic::PROPERTY_NOTIFY);
  pOtaRx = pOtaService->createCharacteristic(
    OTA_CHARACTERISTIC_UUID_RX,
    BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);

  pOtaRx->setCallbacks(new OtaCallbacks());
  pOtaTx->addDescriptor(new BLE2902());
  pOtaTx->setNotifyProperty(true);

  pOtaService->start();

  BLEAdvertising* pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(OTA_SERVICE_UUID);
}

static const char* resetReasonStr(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:  return "POWERON";
    case ESP_RST_EXT:      return "EXT";
    case ESP_RST_SW:       return "SW";
    case ESP_RST_PANIC:    return "PANIC";
    case ESP_RST_INT_WDT:  return "INT_WDT";
    case ESP_RST_TASK_WDT: return "TASK_WDT";
    case ESP_RST_WDT:      return "WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_SDIO:     return "SDIO";
    default:               return "UNKNOWN";
  }
}

void diagLog(const char* line) {
  if (FLASH.exists(DIAG_LOG_PATH)) {
    File f = FLASH.open(DIAG_LOG_PATH, FILE_READ);
    if (f) {
      size_t sz = f.size();
      if (sz > DIAG_LOG_MAX) {
        uint8_t tmp[DIAG_LOG_MAX / 2];
        f.seek(sz - sizeof(tmp));
        int n = f.read(tmp, sizeof(tmp));
        f.close();
        int start = 0;
        for (int i = 0; i < n; i++) {
          if (tmp[i] == '\n') { start = i + 1; break; }
        }
        File w = FLASH.open(DIAG_LOG_PATH, FILE_WRITE);  // FILE_WRITE = truncate
        if (w) {
          if (n > start) w.write(tmp + start, n - start);
          w.close();
        }
      } else {
        f.close();
      }
    }
  }

  File f = FLASH.open(DIAG_LOG_PATH, FILE_APPEND);
  if (f) {
    f.print(line);
    f.print('\n');
    f.close();
  } else {
    DBG("diagLog write fail");
  }
}

void printBootDiag() {
#if BOOT_DIAG
  bool rtcValid = (savedZoneMagic == SAVED_ZONE_MAGIC && savedZone >= 0 && savedZone <= 3);
  bool nvsValid = (nvsLastSavedZone >= 0 && nvsLastSavedZone <= 3);
  bool rollerRtcValid = (savedRollerMagic == SAVED_ROLLER_MAGIC &&
                         (savedRoller == 0 || savedRoller == 1));

  Serial.println();
  Serial.println(F("===================================="));
  Serial.println(F("BOOT DIAG (RTC / NVS / diag.log)"));
  Serial.println(F("===================================="));

  Serial.print(F("Free heap: "));
  Serial.println(ESP.getFreeHeap());

  Serial.print(F("RTC magic: 0x"));
  Serial.print(savedZoneMagic, HEX);
  Serial.print(F(" ("));
  Serial.print(rtcValid ? F("valid") : F("invalid"));
  Serial.println(F(")"));
  Serial.print(F("RTC savedZone: "));
  Serial.println(savedZone);
  Serial.print(F("RTC savedRoller: "));
  Serial.print(savedRoller);
  Serial.print(F(" ("));
  Serial.print(rollerRtcValid ? F("valid") : F("invalid"));
  Serial.println(F(")"));

  Serial.print(F("NVS zone: "));
  Serial.print(nvsLastSavedZone);
  Serial.print(F(" ("));
  Serial.print(nvsValid ? F("valid") : F("none/invalid"));
  Serial.println(F(")"));
  Serial.print(F("NVS roller: "));
  Serial.print(nvsLastSavedRoller);
  Serial.print(F(" ("));
  Serial.print((nvsLastSavedRoller == 0 || nvsLastSavedRoller == 1) ? F("valid") : F("none/invalid"));
  Serial.println(F(")"));

  Serial.println(F("--- diag.log ---"));
  if (FLASH.exists(DIAG_LOG_PATH)) {
    File df = FLASH.open(DIAG_LOG_PATH, FILE_READ);
    if (df) {
      if (df.size() == 0) {
        Serial.println(F("(ures)"));
      } else {
        while (df.available()) Serial.write(df.read());
        Serial.println();
      }
      df.close();
    } else {
      Serial.println(F("(nem olvashato)"));
    }
  } else {
    Serial.println(F("(nincs diag.log)"));
  }
  Serial.println(F("===================================="));
#endif
}

void handleDiagRequest() {
  if (!pCharacteristic) return;

  if (diagClearRequested) {
    diagClearRequested = false;
    if (FLASH.exists(DIAG_LOG_PATH)) FLASH.remove(DIAG_LOG_PATH);
    pCharacteristic->setValue("DIAG_CLEARED");
    pCharacteristic->notify();
    DBG("Diag log cleared");
    return;
  }

  if (diagRequested && !diagStreaming) {
    diagRequested = false;
    diagFile = FLASH.open(DIAG_LOG_PATH, FILE_READ);
    static const uint8_t DIAG_BEGIN[] = { 0x02, 'D','I','A','G','_','B','E','G','I','N' };
    pCharacteristic->setValue((uint8_t*)DIAG_BEGIN, sizeof(DIAG_BEGIN));
    pCharacteristic->notify();
    diagStreaming = true;
    diagLastChunk = millis();
    return;
  }

  if (diagStreaming) {
    unsigned long now = millis();
    if (now - diagLastChunk < DIAG_CHUNK_INTERVAL) return;
    diagLastChunk = now;

    if (diagFile && diagFile.available()) {
      uint8_t buf[DIAG_CHUNK_SIZE];
      int n = diagFile.read(buf, DIAG_CHUNK_SIZE);
      if (n > 0) {
        pCharacteristic->setValue(buf, n);
        pCharacteristic->notify();
      }
    } else {
      if (diagFile) diagFile.close();
      static const uint8_t DIAG_END[] = { 0x04, 'D','I','A','G','_','E','N','D' };
      pCharacteristic->setValue((uint8_t*)DIAG_END, sizeof(DIAG_END));
      pCharacteristic->notify();
      diagStreaming = false;
      DBG("Diag log sent");
    }
  }
}

// ===================== SETUP =====================
void setup() {
  // [FIX-ESP-39] Relék azonnali tiltása a setup() legelső lépéseként (Serial előtt) → legrövidebb boot-ablak: tápengedély LOW + minden relé OFF
  pinMode(RELAY_EN, OUTPUT); digitalWrite(RELAY_EN, LOW);
  pinMode(RELAY_FAN1, OUTPUT);   digitalWrite(RELAY_FAN1, HIGH);
  pinMode(RELAY_FAN2, OUTPUT);   digitalWrite(RELAY_FAN2, HIGH);
  pinMode(RELAY_FAN3, OUTPUT);   digitalWrite(RELAY_FAN3, HIGH);
  pinMode(RELAY_ROLLER, OUTPUT); digitalWrite(RELAY_ROLLER, HIGH);
  relaysEnabled = false;
  
#if SERIAL_ENABLED
  Serial.begin(115200);
  delay(100);
#endif

  DBG("GPIO + Serial init, Relays safe off done");
  
#if defined(CONFIG_IDF_TARGET_ESP32C6)
  // C6: külső antenna kiválasztása a BLE rádió indítása előtt (közös 2,4 GHz antenna-kapcsoló)
  pinMode(RF_SWITCH_EN, OUTPUT);
  digitalWrite(RF_SWITCH_EN, LOW);         // RF switch control aktiválás
  delay(100);
  pinMode(ANT_SELECT, OUTPUT);
  digitalWrite(ANT_SELECT, HIGH);          // külső antenna használata
#endif
  
#if FAN_SENSE_ENABLE
  pinMode(FAN1_SENSE_PIN, INPUT_PULLUP);
  pinMode(FAN2_SENSE_PIN, INPUT_PULLUP);
  pinMode(FAN3_SENSE_PIN, INPUT_PULLUP);
  fanSenseGraceUntil = millis() + 3000;
#endif
  
  DBG("LED boot state");
  pinMode(LED_YELLOW, OUTPUT); digitalWrite(LED_YELLOW, HIGH);
  pinMode(LED_RED, OUTPUT); digitalWrite(LED_RED, LOW);
  
  ota_boot_flow();

  static_assert(sizeof(BLE_AUTH_PIN) > 1, "BLE_AUTH_PIN is empty!");

  DBG("Boot");

  if (!SPIFFS.begin(FORMAT_SPIFFS_IF_FAILED)) {
    DBG("SPIFFS mount fail");
  }

  if (SPIFFS.exists("/update.bin")) {
    File f = SPIFFS.open("/update.bin");
    if (f) {
      bool isDir = f.isDirectory();
      f.close();
      if (isDir) {
        DBG("Stale update.bin dir removed");
      } else {
        DBG("Stale update.bin removed");
      }
      SPIFFS.remove("/update.bin");
      delay(100);
    }
  }

  esp_task_wdt_config_t wdt_config = {
    .timeout_ms = 15000,
    .idle_core_mask = (1 << 0),
    .trigger_panic = true
  };

  esp_task_wdt_deinit();
  esp_task_wdt_init(&wdt_config);
  esp_task_wdt_add(NULL);

  esp_reset_reason_t resetReason = esp_reset_reason();
  lastBootResetReason = resetReason;  // [FIX-ESP-19] globális mentés
  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
  bootMagic = BOOT_MAGIC;

  DBG("");
  DBG("====================================");
  DBG("Xiao ESP32C3 Fan + Roller + OTA");
  DBG_P("FW: v");
  DBG_P(FIRMWARE_VERSION);
  DBG_P(" (");
  DBG_P(FIRMWARE_DATE);
  DBG(")");
  DBG_P("Reset reason: ");
  DBG_V((int)resetReason);
  DBG_P(" (");
  DBG_V(resetReasonStr(resetReason));
  DBG(")");
  DBG("====================================");

  // CRC32 önteszt: ismert vektorral ellenőrzi, hogy a crc32_zlib a szabványos
  // eredményt adja-e. FAIL esetén nem állítjuk le az eszközt — csak az OTA-t
  // tiltjuk le (a firmware-ellenőrzés megbízhatatlan lenne), és a hibát a
  // diag.log-ba írjuk. Release buildben is fut, mert pont ott számít.
  {
    const uint8_t tv[] = { '1','2','3','4','5','6','7','8','9' };
    uint32_t got = crc32_zlib(tv, 9);
    otaCrcOk = (got == 0xCBF43926);
    DBG_P("CRC32 self-test: 0x");
    DBG_V(got, HEX);
    DBG_VLN(otaCrcOk ? F(" OK") : F(" FAIL!"));
    if (!otaCrcOk) {
      diagLog("[boot] CRC32 self-test FAIL -> OTA off");
    }
  }

  if (resetReason != ESP_RST_POWERON &&
      resetReason != ESP_RST_DEEPSLEEP &&
      resetReason != ESP_RST_SW) {
    char entry[80];
    snprintf(entry, sizeof(entry), "[boot] reason=%s(%d) heap=%u min=%u",
             resetReasonStr(resetReason), (int)resetReason,
             (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap());
    diagLog(entry);
  }

  if (resetReason == ESP_RST_DEEPSLEEP) {
    if (wakeup_reason == ESP_SLEEP_WAKEUP_GPIO) {
      DBG("Wake: button");
    } else {
      DBG("Deep sleep wake (no button) → back to sleep");
#if SERIAL_ENABLED
      Serial.flush();
#endif
      delay(100);
      pinMode(BUTTON_PIN, INPUT_PULLUP);
      esp_deep_sleep_enable_gpio_wakeup(BIT(BUTTON_PIN), ESP_GPIO_WAKEUP_GPIO_LOW);
      esp_deep_sleep_start();
    }
  } else if (resetReason == ESP_RST_POWERON) {
    DBG("Power-on → sleep (wait for button)");
#if SERIAL_ENABLED
    Serial.flush();
#endif
    delay(100);
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    esp_deep_sleep_enable_gpio_wakeup(BIT(BUTTON_PIN), ESP_GPIO_WAKEUP_GPIO_LOW);
    esp_deep_sleep_start();
  } else {
    DBG("Fault/SW reset → resuming normal operation");
  }

  DBG("Button init");
  button.attachClick(handleClick);
  button.attachLongPressStop(handleLongPressStop);
  button.attachDoubleClick(handleDoubleClick);
  button.attachMultiClick(handleMultiClick);
  button.setPressTicks(2000);
  button.setClickTicks(400);

  DBG("Relay state restore");
  fanPrefs.begin("fan", true);  // read-only
  nvsLastSavedZone = fanPrefs.getInt("zone", -1);
  nvsLastSavedRoller = fanPrefs.getInt("roller", -1);  // [FIX-ESP-30] görgő (-1 = nincs)
  fanPrefs.end();

  if (lastBootResetReason == ESP_RST_BROWNOUT ||
      lastBootResetReason == ESP_RST_UNKNOWN ||
      lastBootResetReason == ESP_RST_INT_WDT ||
      lastBootResetReason == ESP_RST_TASK_WDT ||
      lastBootResetReason == ESP_RST_WDT) {

    bool rollerRtcValid = (savedRollerMagic == SAVED_ROLLER_MAGIC &&
                           (savedRoller == 0 || savedRoller == 1));
    bool rollerNvsValid = (nvsLastSavedRoller == 0 || nvsLastSavedRoller == 1);
    int rollerWas;
    if (rollerRtcValid)      rollerWas = savedRoller;          // RTC friss
    else if (rollerNvsValid) rollerWas = nvsLastSavedRoller;   // NVS fallback (brownout)
    else                     rollerWas = -1;                   // ismeretlen → nem indítunk

    // [FIX-ESP-39] Hurok-megszakító számláló (RTC). Érvénytelen magic → 0-ról indul.
    if (errRestoreMagic != ERR_RESTORE_MAGIC) {
      errRestoreCount = 0;
      errRestoreMagic = ERR_RESTORE_MAGIC;
    }

    if (rollerWas != 1) {
      DBG("Boot after error reset, roller was NOT active → staying idle");
    } else if (++errRestoreCount >= MAX_ERR_RESTORE) {
      // Túl sok gyors hibás reset (brownout-hurok gyanú) → nem állítunk vissza, idle marad; a számláló 30 s stabil futás után nullázódik
      DBG_P("Loop-break: consecutive error-restores=");
      DBG_V(errRestoreCount);
      DBG(" → staying idle");
      char e[64];
      snprintf(e, sizeof(e), "[boot] loop-break idle n=%d", errRestoreCount);
      diagLog(e);
    } else {
      restore_roller= true;
      DBG("Boot after BROWNOUT/UNKNOWN/WDT, roller was active → resuming");
      enableRelays();
      delay(100);
      activateRoller();

      bool rtcValid = (savedZoneMagic == SAVED_ZONE_MAGIC && savedZone >= 0 && savedZone <= 3);
      bool nvsValid = (nvsLastSavedZone >= 0 && nvsLastSavedZone <= 3);
      int restoreZone;

      if (rtcValid) {
        restoreZone = savedZone;
        DBG_P("Restoring fan zone (RTC valid, freshest): ");
        DBG_VLN(restoreZone);
      } else if (nvsValid) {
        restoreZone = nvsLastSavedZone;
        DBG_P("Restoring fan zone (RTC invalid, NVS fallback): ");
        DBG_VLN(restoreZone);
      } else {
        restoreZone = 2;
        DBG("Both RTC and NVS invalid → defaulting to zone 2");
      }

      setFanZone(restoreZone, SRC_BUTTON);
      // [FIX-ESP-40] Fan-relé azonnali bekapcsolása bootkor: a setFanZone csak indítja a váltást, a handleZoneChange RELAY_SWITCH_DELAY_MS után hat → kivárjuk, majd hívjuk
      delay(RELAY_SWITCH_DELAY_MS + 5);
      handleZoneChange();
    }
  }

  DBG("BLE init");
  BLEDevice::init("FanController");
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  BLEService* pService = pServer->createService(SERVICE_UUID);
  pCharacteristic = pService->createCharacteristic(
    CHARACTERISTIC_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_NOTIFY);

  pCharacteristic->setCallbacks(new MyCallbacks());
  pCharacteristic->addDescriptor(new BLE2902());
  pService->start();

  otaInitService(pServer);

  BLEAdvertising* pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06);
  pAdvertising->setMaxPreferred(0x12);
  BLEDevice::startAdvertising();

  DBG("BLE ready");

  lastActivityTime = millis();
  lastHeartbeat = millis();

  printBootDiag();
  
  DBG("Boot done");
  digitalWrite(LED_YELLOW, LOW);
}

// ===================== LOOP =====================
void loop() {
  esp_task_wdt_reset();
  unsigned long now2 = millis();

  // [FIX-ESP-39] ERR_RESTORE_CLEAR_MS stabil futás után nullázzuk a hibás-reset számlálót (csak a gyors, ismétlődő reseteket számoljuk)
  if (!errRestoreCleared && now2 >= ERR_RESTORE_CLEAR_MS) {
    errRestoreCount = 0;
    errRestoreMagic = ERR_RESTORE_MAGIC;
    errRestoreCleared = true;
  }

  // OTA health-check: csak OTA_VERIFY_HEALTHY_MS stabil futás után validál; ha előbb újraindul, a bootloader visszagörget
  if (otaPendingVerify && now2 >= OTA_VERIFY_HEALTHY_MS) {
    esp_err_t r = esp_ota_mark_app_valid_cancel_rollback();
    otaPendingVerify = false;
    if (r == ESP_OK) {
      DBG("OTA health-check OK → firmware VALID (rollback lemondva)");
    } else {
      DBG_P("OTA mark valid FAILED: ");
      DBG_VLN(esp_err_to_name(r));
    }
  }

  if (now2 - lastCheck >= checkInterval) {
    lastCheck = now2;
    stateMachineStep();
  }

#if FAN_SENSE_ENABLE
  if (!otaIsRunning()) monitorFanRelays();
#endif

  otaLoop();

  if (!otaIsRunning()) handleDiagRequest();

  if (!otaIsRunning()) saveZoneToNvsIfStable();
}

// ===================== STATE MACHINE =====================
void stateMachineStep() {
  if (otaIsRunning()) {
    return;
  }

  switch (currentState) {
    case STATE_NORMAL:
      normalMode();
      break;
    case STATE_FAILSAFE:
      failSafeMode();
      break;
  }
}

void normalMode() {
  unsigned long nowNormalMode = millis();

  failStartSet = false;
  failStart = 0;

  currentMillis = nowNormalMode;

  bool hasActivity =
    rollerActive && (bleConnected || manualMode) && (currentZone != 0 || manualZoneIndex != 0);

  bool prevActive = wasActive;
  wasActive = hasActivity;

  if (hasActivity && !prevActive) {
    DBG("Activity detected");
  }

  if (hasActivity) {
    lastActivityTime = nowNormalMode;
  }

  static Timer inactivityTimer{ 0, INACTIVITY_MS };

  if (hasActivity) {
    inactivityTimer.last = nowNormalMode;
  }

  if (inactivityTimer.elapsed(nowNormalMode)) {
    if (!bleConnected && !manualMode) {
      DBG("Idle → sleep");
      enterDeepSleep("idle-timeout");
    }
  }

  static Timer printTimer2{ 0, printInterval };
  if (printTimer2.elapsed(nowNormalMode)) {
    unsigned long diff = nowNormalMode - lastActivityTime;
    long remainingMs = (long)INACTIVITY_MS - (long)diff;
    if (remainingMs < 0) remainingMs = 0;
    long remainingMin = remainingMs / 60000;

    static long lastPrintedMin = -1;
    if (remainingMin != lastPrintedMin) {
      lastPrintedMin = remainingMin;
      DBG_P("To sleep (min): ");
      DBG_VLN(remainingMin);
      DBG_P("Free heap: ");
      DBG_V(ESP.getFreeHeap());
      DBG_P(" / min: ");
      DBG_VLN(ESP.getMinFreeHeap());
    }
  }

  static bool lowHeapLogged = false;
  uint32_t freeHeapNow = ESP.getFreeHeap();
  if (freeHeapNow < LOW_HEAP_THRESHOLD) {
    if (!lowHeapLogged && !diagStreaming) {
      char e[72];
      snprintf(e, sizeof(e), "[lowmem] heap=%u min=%u t=%lus",
               (unsigned)freeHeapNow, (unsigned)ESP.getMinFreeHeap(),
               (unsigned long)(nowNormalMode / 1000));
      diagLog(e);
      DBG("LOW HEAP logged to diag");
      lowHeapLogged = true;
    }
  } else if (freeHeapNow > LOW_HEAP_THRESHOLD + 4096) {
    lowHeapLogged = false;
  }

  static Timer bleRestartTimer{ 0, BLE_RESTART_DELAY };
  static bool bleRestartMsgShownLocal = false;

  if (bleNeedsRestart && bleEnabled) {
    if (!bleRestartMsgShownLocal) {
      DBG("BLE restart start");
      bleRestartMsgShownLocal = true;
    }

    if (bleRestartTime == 0) {
      bleRestartTime = nowNormalMode;
      bleRestartTimer.last = nowNormalMode;
    }

    if (bleRestartTimer.elapsed(nowNormalMode)) {
      DBG("BLE restart done");
      pServer->getAdvertising()->start();
      bleNeedsRestart = false;
      bleRestartTime = 0;
      bleRestartMsgShownLocal = false;
    }

  } else {
    bleRestartMsgShownLocal = false;
  }

  static Timer bleZoneTimeout{ 0, BLE_ZONE_TIMEOUT_MS };
  static bool bleZoneTimeoutMsgShownLocal = false;

  if (!bleConnected && currentZone != 0 && !manualMode) {

    if (bleDisconnectTime != 0) {

      if (bleZoneTimeout.last == 0)
        bleZoneTimeout.last = bleDisconnectTime;

      if (!bleZoneTimeoutMsgShownLocal) {
        DBG("BLE lost, zone timeout start");
        bleZoneTimeoutMsgShownLocal = true;
      }

      if (bleZoneTimeout.elapsed(nowNormalMode)) {
        DBG("Zone timeout → all OFF");
        setFanZone(0, SRC_NONE);
        deactivateRoller();
        disableRelays();
        bleZoneTimeoutMsgShownLocal = false;
      }
    }

  } else {
    bleZoneTimeout.last = nowNormalMode;
    bleZoneTimeoutMsgShownLocal = false;
  }

  button.tick();
  handleLEDs(nowNormalMode);
  handleZoneChange();
  handleBleCommand();

  int f1 = digitalRead(RELAY_FAN1);
  int f2 = digitalRead(RELAY_FAN2);
  int f3 = digitalRead(RELAY_FAN3);
  if ((f1 == LOW) + (f2 == LOW) + (f3 == LOW) >= 2) {
    char e[48];
    int n = snprintf(e, sizeof(e), "[RELAYS]");
    if (f1 == LOW) n += snprintf(e + n, sizeof(e) - n, " 1");
    if (f2 == LOW) n += snprintf(e + n, sizeof(e) - n, " 2");
    if (f3 == LOW) n += snprintf(e + n, sizeof(e) - n, " 3");
    snprintf(e + n, sizeof(e) - n, " ACTIVE ST zone=%d", currentZone);
    DBG_VLN(e);
    if (!diagStreaming) diagLog(e);
    zeroStateForFailsafe();  // [FIX-ESP-33] nullázás MÉG a STATE_FAILSAFE előtt
    currentState = STATE_FAILSAFE;
    return;
  }

#if FAN_SENSE_ENABLE
  checkFanRelayMismatch();
  if (currentState == STATE_FAILSAFE) return;
#endif

  yield();
}

void zeroStateForFailsafe() {
  portENTER_CRITICAL(&zoneMux);
  currentZone = 0;
  pendingZone = 0;
  zoneChanging = false;
  zoneChangeInProgress = false;
  savedZone = 0;
  savedZoneMagic = SAVED_ZONE_MAGIC;
  savedRoller = 0;
  savedRollerMagic = SAVED_ROLLER_MAGIC;
  portEXIT_CRITICAL(&zoneMux);
  rollerActive = false;
  nvsZonePending = false;

  if (!otaIsRunning() && (nvsLastSavedZone != 0 || nvsLastSavedRoller != 0)) {
    fanPrefs.begin("fan", false);
    fanPrefs.putInt("zone", 0);
    fanPrefs.putInt("roller", 0);
    fanPrefs.end();
    nvsLastSavedZone = 0;
    nvsLastSavedRoller = 0;
    lastNvsSaveTime = millis();
  }
}

void failSafeMode() {
  if (!failStartSet) {
    failStart = millis();
    failStartSet = true;

    zeroStateForFailsafe();
    DBG("FAILSAFE entry → roller+fan state zeroed (RTC+NVS)");
  }

  digitalWrite(RELAY_FAN1, HIGH);
  digitalWrite(RELAY_FAN2, HIGH);
  digitalWrite(RELAY_FAN3, HIGH);
  digitalWrite(RELAY_ROLLER, HIGH);
  digitalWrite(RELAY_EN, LOW);

  unsigned long nowfailSafeMode = millis();

  static Timer failPrintTimer{ 0, 1000 };
  if (failPrintTimer.elapsed(nowfailSafeMode)) {
    DBG("FAILSAFE active");
  }

  if (nowfailSafeMode - lastBlink >= blinkInterval) {
    lastBlink = nowfailSafeMode;
    blinkState = !blinkState;

    digitalWrite(LED_RED, blinkState);
    digitalWrite(LED_YELLOW, blinkState);
  }

  if (nowfailSafeMode - failStart >= 10000) {
    DBG("Failsafe timeout → sleep");
    enterDeepSleep("failsafe-timeout");
  }
}

// ===================== BLE CMD HANDLER =====================
void handleBleCommand() {
  int zone = -1;
  int rollerCmd = -1;

  portENTER_CRITICAL(&bleCmdMux);
  if (bleCmd.hasCommand) {
    zone = bleCmd.zone;
    bleCmd.hasCommand = false;
  }
  if (bleCmd.hasRollerCommand) {
    rollerCmd = bleCmd.rollerCommand;
    bleCmd.hasRollerCommand = false;
  }
  portEXIT_CRITICAL(&bleCmdMux);

  if (zone != -1) {
    setFanZone(zone, SRC_BLE);
  }

  if (rollerCmd != -1) {
    if (rollerCmd == 1) {
      if (!relaysEnabled) enableRelays();
      activateRoller();
    } else {
      deactivateRoller();
      if (currentZone == 0) disableRelays();
    }
  }
}

// ===================== ZONE CONTROL =====================
void setFanZone(int zone, CommandSource source) {
  if (otaIsRunning()) {
    DBG("Zone change blocked (OTA)");
    return;
  }

  unsigned long now = millis();
  int fromZone = currentZone;

  portENTER_CRITICAL(&zoneMux);

  if (now >= sourceLockedUntil) {
    activeSource = SRC_NONE;
  }

  if (zoneChanging || zoneChangeInProgress) {
    portEXIT_CRITICAL(&zoneMux);
    DBG("Zone change blocked");
    return;
  }

  zoneChanging = true;
  zoneChangeInProgress = true;

  if (activeSource != SRC_NONE && source != SRC_NONE && now < sourceLockedUntil) {
    if (source < activeSource) {
      zoneChanging = false;
      zoneChangeInProgress = false;
      portEXIT_CRITICAL(&zoneMux);
      DBG("Zone change rejected");
      return;
    }
  }

  if (source != SRC_NONE) {
    activeSource = source;
    sourceLockedUntil = now + SOURCE_LOCK_MS;
  }

  if (zone < 0) zone = 0;
  if (zone > 3) zone = 3;

  if (zone == currentZone) {
    zoneChanging = false;
    zoneChangeInProgress = false;
    portEXIT_CRITICAL(&zoneMux);
    DBG("Zone already set");
    return;
  }

  digitalWrite(RELAY_FAN1, HIGH);
  digitalWrite(RELAY_FAN2, HIGH);
  digitalWrite(RELAY_FAN3, HIGH);

  pendingZone = zone;
  zoneChangeStart = now;

  portEXIT_CRITICAL(&zoneMux);

#if FAN_SENSE_ENABLE
  fanSenseGraceUntil = now + RELAY_SWITCH_DELAY_MS + FAN_SENSE_GRACE_MS;
#endif

  DBG_P("Zone change: ");
  DBG_V(fromZone);
  DBG_P(" -> ");
  DBG_VLN(zone);
}

void handleZoneChange() {
  unsigned long nowhandleZoneChange = millis();

  unsigned long localZoneChangeStart;
  int localPendingZone;

  portENTER_CRITICAL(&zoneMux);
  if (!zoneChangeInProgress) {
    portEXIT_CRITICAL(&zoneMux);
    return;
  }
  localZoneChangeStart = zoneChangeStart;
  localPendingZone = pendingZone;
  portEXIT_CRITICAL(&zoneMux);

  if (nowhandleZoneChange >= localZoneChangeStart) {
    if (nowhandleZoneChange - localZoneChangeStart < RELAY_SWITCH_DELAY_MS) {
      return;
    }
  }

  portENTER_CRITICAL(&zoneMux);

  currentZone = localPendingZone;

  savedZone = localPendingZone;
  savedZoneMagic = SAVED_ZONE_MAGIC;

  switch (localPendingZone) {
    case 1: digitalWrite(RELAY_FAN1, LOW); break;
    case 2: digitalWrite(RELAY_FAN2, LOW); break;
    case 3: digitalWrite(RELAY_FAN3, LOW); break;
    case 0: break;
  }

  zoneChanging = false;
  zoneChangeInProgress = false;

  portEXIT_CRITICAL(&zoneMux);

#if FAN_SENSE_ENABLE
  fanSenseGraceUntil = nowhandleZoneChange + FAN_SENSE_GRACE_MS;
  fanMismatchSince[0] = fanMismatchSince[1] = fanMismatchSince[2] = 0;
  fanNoacWarned[0] = fanNoacWarned[1] = fanNoacWarned[2] = false;
#endif

  zoneStableSince = nowhandleZoneChange;
  nvsZonePending = true;

  switch (localPendingZone) {
    case 1: DBG("Fan1 ON (33%)"); break;
    case 2: DBG("Fan2 ON (66%)"); break;
    case 3: DBG("Fan3 ON (100%)"); break;
    case 0: DBG("All fans OFF"); break;
  }
}

void saveZoneToNvsIfStable() {
  unsigned long now = millis();

  int z;
  portENTER_CRITICAL(&zoneMux);
  z = currentZone;
  portEXIT_CRITICAL(&zoneMux);
  int rollerNow = rollerActive ? 1 : 0;  // bool, atomi olvasás

  bool stableSave = nvsZonePending && (now - zoneStableSince >= NVS_SAVE_STABLE_MS);
  bool forceSave  = (now - lastNvsSaveTime >= NVS_FORCE_SAVE_MS) && (z != nvsLastSavedZone);
  if (stableSave) nvsZonePending = false;  // a stabil-pending elintézve, nem pörgünk rá

  bool zoneNeedsWrite   = (stableSave || forceSave) && (z != nvsLastSavedZone);
  bool rollerNeedsWrite = (rollerNow != nvsLastSavedRoller);

  if (!zoneNeedsWrite && !rollerNeedsWrite) return;

  fanPrefs.begin("fan", false);
  if (zoneNeedsWrite) {
    fanPrefs.putInt("zone", z);
    nvsLastSavedZone = z;
    lastNvsSaveTime = now;
  }
  if (rollerNeedsWrite) {
    fanPrefs.putInt("roller", rollerNow);
    nvsLastSavedRoller = rollerNow;
  }
  fanPrefs.end();

  if (zoneNeedsWrite) {
    DBG_P("NVS zone saved: ");
    DBG_V(z);
    DBG_VLN((forceSave && !stableSave) ? " (force 5min)" : " (stable 30s)");
  }
  if (rollerNeedsWrite) {
    DBG_P("NVS roller saved: ");
    DBG_VLN(rollerNow);
  }
}

// ===================== FAN RELÉ KIMENET FIGYELÉS (H11AA1M) =====================
#if FAN_SENSE_ENABLE
void monitorFanRelays() {
  unsigned long now = millis();
  bool inGrace = ((long)(fanSenseGraceUntil - now) > 0);

  for (int i = 0; i < 3; i++) {
    int raw = digitalRead(fanSensePins[i]);

    // AC a sense-ágon = volt-e LOW (opto-vezetés) az ablakban. A LOW mintára épülünk,
    // mert AC jelenlétében a jel — RC-szűréssel stabil LOW, anélkül tüskés, de túlnyomóan
    // LOW — mindig ad LOW mintát; a (nullátmeneti) HIGH-tüskéket szándékosan ignoráljuk,
    // így a kimeneti szűrőkondi kiesése sem ad téves eredményt.
    if (raw == LOW) {
      fanSenseLastLow[i] = now;
      fanSenseSeen[i] = true;
    }
    bool acOnSense = fanSenseSeen[i] &&
                     ((unsigned long)(now - fanSenseLastLow[i]) < AC_SENSE_WINDOW_MS);

    // Bekötés-függő leképezés „relé behúzva"-ra (NC: AC ⇒ nincs behúzva; NO: AC ⇒ behúzva).
#if FAN_SENSE_AC_MEANS_ENGAGED
    bool rawEngaged = acOnSense;
#else
    bool rawEngaged = !acOnSense;
#endif

    if (rawEngaged != fanRelayEngaged[i]) {
      if (fanSenseChangeSince[i] == 0) fanSenseChangeSince[i] = now;
      if ((unsigned long)(now - fanSenseChangeSince[i]) >= AC_SENSE_DEBOUNCE_MS) {
        fanRelayEngaged[i] = rawEngaged;
        fanSenseChangeSince[i] = 0;
        if (!inGrace) {
          DBG_P("Relay");
          DBG_V(i + 1);
          DBG_VLN(rawEngaged ? F(" ACTIVE") : F(" INACTIVE"));
        }
      }
    } else {
      fanSenseChangeSince[i] = 0;
    }
  }
}

void checkFanRelayMismatch() {
  unsigned long now = millis();

  bool inGrace = ((long)(fanSenseGraceUntil - now) > 0);

  for (int i = 0; i < 3; i++) {
    bool expectedEngaged = relaysEnabled && (currentZone == (i + 1));
    bool engaged = fanRelayEngaged[i];          // TRUE = a relé behúzva (NC-érzékelés)

    bool stuck = (!expectedEngaged && engaged);   // a zóna OFF-ot vár, de a relé BEHÚZVA (NC nyitva) → beragadt relé
    bool noac  = (expectedEngaged && !engaged);   // a zóna ON-t vár, de a relé NINCS behúzva → relé/biztosíték/hálózat hiba

#if FAN_SENSE_FAILSAFE_ON_STUCK
    if (stuck && !inGrace) {
      char e[48];
      snprintf(e, sizeof(e), "[RELAYS] Relay%d STUCK zone=%d", i + 1, currentZone);
      DBG_VLN(e);
      if (!diagStreaming) diagLog(e);

      zeroStateForFailsafe();  // [FIX-ESP-33] nullázás MÉG a STATE_FAILSAFE előtt
      currentState = STATE_FAILSAFE;
      return;
    }
#endif

#if FAN_SENSE_WARN_ON_NOAC
    if (noac && !inGrace) {
      if (fanMismatchSince[i] == 0) fanMismatchSince[i] = now;
      if (!fanNoacWarned[i] &&
          (unsigned long)(now - fanMismatchSince[i]) >= FAN_SENSE_MISMATCH_CONFIRM_MS) {
        DBG_P("FIGYELEM: Fan");
        DBG_V(i + 1);
        DBG(" zona ON, de a rele nincs behuzva (nincs NC-visszajelzes) - tovabb fut");

        fanNoacWarned[i] = true;  // egyszer figyelmeztetünk, amíg fennáll
      }
    } else {
      fanMismatchSince[i] = 0;
      fanNoacWarned[i] = false;
    }
#endif
  }
}
#endif  // FAN_SENSE_ENABLE

// ===================== ROLLER CONTROL =====================
void activateRoller() {
  digitalWrite(RELAY_ROLLER, LOW);
  rollerActive = true;
  savedRoller = 1;
  savedRollerMagic = SAVED_ROLLER_MAGIC;
  DBG("Roller ON");
}

void deactivateRoller() {
  digitalWrite(RELAY_ROLLER, HIGH);
  rollerActive = false;
  savedRoller = 0;
  savedRollerMagic = SAVED_ROLLER_MAGIC;
  DBG("Roller OFF");
}

// ===================== RELAY CONTROL =====================
void enableRelays() {
  digitalWrite(RELAY_FAN1, HIGH);
  digitalWrite(RELAY_FAN2, HIGH);
  digitalWrite(RELAY_FAN3, HIGH);
  digitalWrite(RELAY_ROLLER, HIGH);
  delay(10);
  digitalWrite(RELAY_EN, HIGH);
  delay(10);
  relaysEnabled = true;
#if FAN_SENSE_ENABLE
  fanSenseGraceUntil = millis() + FAN_SENSE_GRACE_MS;
  fanMismatchSince[0] = fanMismatchSince[1] = fanMismatchSince[2] = 0;
  fanNoacWarned[0] = fanNoacWarned[1] = fanNoacWarned[2] = false;
#endif
  DBG("Relays ON");
}

void disableRelays() {
  digitalWrite(RELAY_FAN1, HIGH);
  digitalWrite(RELAY_FAN2, HIGH);
  digitalWrite(RELAY_FAN3, HIGH);
  digitalWrite(RELAY_ROLLER, HIGH);
  delay(10);
  digitalWrite(RELAY_EN, LOW);
  delay(10);
  relaysEnabled = false;
#if FAN_SENSE_ENABLE
  fanSenseGraceUntil = millis() + FAN_SENSE_GRACE_MS;
  fanMismatchSince[0] = fanMismatchSince[1] = fanMismatchSince[2] = 0;
  fanNoacWarned[0] = fanNoacWarned[1] = fanNoacWarned[2] = false;
#endif
  DBG("Relays OFF");
}

// ===================== LED HANDLING =====================
void handleLEDs(unsigned long currentMillis) {
  if (otaIsRunning()) {
    return;
  }

  if (bleConnected) {
    digitalWrite(LED_RED, HIGH);

  } else if (manualMode) {
    digitalWrite(LED_RED, LOW);

  } else if (bleEnabled && !bleConnected) {
    if (currentMillis - lastRedToggle > LED_BLINK_INTERVAL) {
      redLedState = !redLedState;
      digitalWrite(LED_RED, redLedState ? HIGH : LOW);
      lastRedToggle = currentMillis;
    }

  } else {
    if (!heartbeatPulse_red) {
      if (currentMillis - lastHeartbeat_red >= HEARTBEAT_INTERVAL) {
        digitalWrite(LED_RED, HIGH);
        heartbeatPulse_red = true;
        lastHeartbeat_red = currentMillis;
      } else {
        digitalWrite(LED_RED, LOW);
      }
    } else {
      if (currentMillis - lastHeartbeat_red >= HEARTBEAT_PULSE) {
        digitalWrite(LED_RED, LOW);
        heartbeatPulse_red = false;
      }
    }
  }

  if (relaysEnabled && rollerActive) {
    if (currentMillis - lastYellowToggle > LED_BLINK_INTERVAL) {
      yellowLedState = !yellowLedState;
      digitalWrite(LED_YELLOW, yellowLedState ? HIGH : LOW);
      lastYellowToggle = currentMillis;
    }

  } else {
    if (!heartbeatPulse) {
      if (currentMillis - lastHeartbeat >= HEARTBEAT_INTERVAL) {
        digitalWrite(LED_YELLOW, HIGH);
        heartbeatPulse = true;
        lastHeartbeat = currentMillis;
      } else {
        digitalWrite(LED_YELLOW, LOW);
      }
    } else {
      if (currentMillis - lastHeartbeat >= HEARTBEAT_PULSE) {
        digitalWrite(LED_YELLOW, LOW);
        heartbeatPulse = false;
      }
    }
  }
}

// ===================== DEEP SLEEP =====================
void enterDeepSleep(const char* reason) {
  DBG("====================================");
  DBG("Enter deep sleep");
  DBG_P("Reason: ");
  DBG_VLN(reason);
  DBG("====================================");

  // OTA health-check: a kontrollált deep sleep elérése = működő firmware → validálunk (PENDING_VERIFY-ben ébredés különben rollbackot váltana)
  if (otaPendingVerify) {
    esp_ota_mark_app_valid_cancel_rollback();
    otaPendingVerify = false;
    DBG("OTA health-check OK (pre-sleep) → firmware VALID (rollback lemondva)");
  }

  if (bleEnabled) {
    DBG("BLE stop");
    if (bleConnected) {
      pServer->disconnect(0);
      delay(500);   // [FIX-ESP-23] BLE stack teljes kimaradása
    }
    BLEDevice::stopAdvertising();
    delay(300);     // [FIX-ESP-23] advertising shutdown
    bleConnected = false;
    bleEnabled = false;
  }

  DBG("Relays OFF before sleep");
  disableRelays();
  delay(200);       // [FIX-ESP-23] GPIO settle time relé OFF után

  DBG("LEDs OFF");
  digitalWrite(LED_RED, LOW);
  digitalWrite(LED_YELLOW, LOW);
  delay(200);       // [FIX-ESP-23] GPIO settle time LED OFF után

  DBG("Deep sleep on BTN");
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);  // korábbi wakeup sourceok törlése
  esp_deep_sleep_enable_gpio_wakeup(BIT(BUTTON_PIN), ESP_GPIO_WAKEUP_GPIO_LOW);

  delay(500);       // [FIX-ESP-23] ESP stabilizáció a deep sleep előtt
#if SERIAL_ENABLED
  Serial.flush();
#endif
  esp_deep_sleep_start();
}

// ===================== OTA LOOP =====================
void otaLoop() {
  if (!pOtaTx || !pOtaRx) return;

  if (otaPendingReboot && millis() >= otaRebootAt) {
    rebootEspWithReason("OTA done");
  }

  if (otaMode != OTA_NORMAL_MODE) {
    unsigned long now = millis();
    if (now - otaLedTimer >= 50) {
      otaLedTimer = now;
      otaLedState = !otaLedState;
      digitalWrite(LED_RED, otaLedState ? HIGH : LOW);
      digitalWrite(LED_YELLOW, otaLedState ? LOW : HIGH);
    }
  }

  switch (otaMode) {

    case OTA_NORMAL_MODE:
      if (otaDeviceConnected) {
        if (otaSendSize) {
          unsigned long x = FLASH.totalBytes();
          unsigned long y = FLASH.usedBytes();
          uint8_t fSize[] = {
            0xEF,
            (uint8_t)(x >> 16),
            (uint8_t)(x >> 8),
            (uint8_t)x,
            (uint8_t)(y >> 16),
            (uint8_t)(y >> 8),
            (uint8_t)y
          };
          pOtaTx->setValue(fSize, 7);
          pOtaTx->notify();
          delay(50);
          otaSendSize = false;
        }
      }
      break;

    case OTA_UPDATE_MODE:

      if (otaWriteFile) {
        if (!otaBuf) { otaWriteFile = false; break; }
        uint8_t* buf = otaBuf;
        int      blen = otaWriteLen;

        uint32_t crc = crc32_zlib(buf, (size_t)blen);

        if (crc != otaExpectedCrc) {
          otaWriteFile = false;
          otaPartRetry++;
          DBG_P("OTA CRC fail part=");
          DBG_V(otaCur);
          DBG_P(" got=0x"); DBG_V(crc, HEX);
          DBG_P(" exp=0x"); DBG_V(otaExpectedCrc, HEX);
          DBG_P(" try="); DBG_VLN(otaPartRetry);

          if (otaPartRetry <= MAX_PART_RETRY) {
            char e[72];
            snprintf(e, sizeof(e), "[ota] crc retry part=%d try=%d", otaCur, otaPartRetry);
            diagLog(e);
            otaExpectedPart = otaCur;  // [FIX-ESP-35] ugyanezt a partot várjuk vissza
            uint8_t rq[] = { 0xF1, (uint8_t)(otaCur / 256), (uint8_t)(otaCur % 256) };
            pOtaTx->setValue(rq, 3);
            pOtaTx->notify();
            delay(50);
          } else {
            otaAbort("CRC fail part " + String(otaCur));
          }
          break;
        }

        otaPartRetry = 0;
        otaWriteBinary(FLASH, "/update.bin", buf, blen);  // otaWriteFile=false, otaReceivedBytes += blen

        if (otaMode != OTA_UPDATE_MODE) break;

        if (otaCur + 1 == otaParts) {
          uint8_t com[] = { 0xF2, (uint8_t)((otaCur + 1) / 256), (uint8_t)((otaCur + 1) % 256) };
          pOtaTx->setValue(com, 3);
          pOtaTx->notify();
          delay(50);
          if (otaBuf) { free(otaBuf); otaBuf = nullptr; }
          otaMode = OTA_INSTALL_MODE;
        } else {
          otaExpectedPart = otaCur + 1;  // [FIX-ESP-35] ezt várjuk vissza
          uint8_t rq[] = { 0xF1, (uint8_t)((otaCur + 1) / 256), (uint8_t)((otaCur + 1) % 256) };
          pOtaTx->setValue(rq, 3);
          pOtaTx->notify();
          delay(50);
        }
      }

      break;

    case OTA_INSTALL_MODE:

      if (otaInstallWaiting) {
        if (millis() >= otaInstallWaitUntil) {
          otaInstallWaiting = false;
          if (otaReceivedBytes == otaTotalBytes && otaTotalBytes > 0) {
            uint32_t savedTotal = otaTotalBytes;
            otaTotalBytes = 0;
            otaReceivedBytes = 0;
            updateFromFS(FLASH);
            (void)savedTotal;  // ha esetleg debug-hoz kéne
          }
        }
        break;  // Várakozás alatt nem futtatjuk le az alábbi logikát
      }

      if (otaReceivedBytes == otaTotalBytes && otaTotalBytes > 0) {
        DBG("OTA file complete");
        otaInstallWaiting = true;
        otaInstallWaitUntil = millis() + 2000;

      } else if (otaTotalBytes > 0) {
        DBG("OTA incomplete");
        DBG_P("Expected: ");
        DBG_VLN(otaTotalBytes);
        DBG_P("Received: ");
        DBG_VLN(otaReceivedBytes);
        otaInstallWaiting = true;
        otaInstallWaitUntil = millis() + 2000;
      }
      break;
  }
}
