// ╔══════════════════════════════════════════════════════════════╗
// ║  VÁLTOZTATÁSOK v7.0.0 → v7.1.0                              ║
// ║                                                              ║
// ║  [MOD-1] performUpdate() – delay(5000) kiváltva             ║
// ║          otaPendingReboot flag + millis() alapú várakozás   ║
// ║                                                              ║
// ║  [MOD-2] OTA_INSTALL_MODE – delay(2000) kiváltva (×2)      ║
// ║          otaInstallWaiting flag + millis() alapú várakozás  ║
// ║                                                              ║
// ║  [MOD-3] performUpdate() – WDT törlése flash írás előtt     ║
// ║          esp_task_wdt_delete(NULL) a watchdog timeout ellen  ║
// ║                                                              ║
// ║  [MOD-4] FIRMWARE_VERSION + FIRMWARE_DATE frissítve        ║
// ║                                                              ║
// ║  [MOD-6] 2026-05-24 – handleMultiClick (3+ kattintás):       ║
// ║          visszavált automata módba (manualMode = false,      ║
// ║          bleEnabled = true), kikapcsolja a kézi zónát, és    ║
// ║          újraindítja a BLE advertising-et.                   ║
// ║                                                              ║
// ║  [FIX-ESP-1] 2026-05-24 – OTA utolsó part nem íródott ki   ║
// ║          OTA_UPDATE_MODE-ban az otaWriteFile blokk az        ║
// ║          otaCur+1==otaParts ellenőrzés UTÁN volt → javítva  ║
// ║  [FIX-ESP-1c] 2026-05-24 – buffer logika kijavítva          ║
// ║          INSTALL_MODE-ban a 1b-s "megfordított" buffer       ║
// ║          logika rossz volt → most ugyanaz mint UPDATE_MODE,  ║
// ║          duplikáció ellen otaWriteFile=false védelem         ║
// ║  [FIX-ESP-2] 2026-05-24 – debug logging otaWriteBinary-be   ║
// ║  [FIX-ESP-3] 2026-05-24 – otaWriteFile=false hibás esetben  ║
// ║          is, hogy ne ragadjunk végtelen ciklusban           ║
// ║  [FIX-ESP-4] 2026-05-24 – ténylegesen kiírt byte-okat       ║
// ║          számoljuk, nem a kértet (részleges write detektál) ║
// ║  [FIX-ESP-5] 2026-05-24 – VALÓDI hiba: módváltás otaWriteFile║
// ║          true állapotban. A SPIFFS write 100+ ms ideig fut, ║
// ║          eközben a 42. part 0xFC megérkezik és újra true-ra ║
// ║          állítja otaWriteFile-t. A write végén false lesz,   ║
// ║          a következő loop sor azonnal módot vált — a 42.    ║
// ║          part írása örökre kimarad. Javítás: módváltás csak  ║
// ║          akkor, ha otaWriteFile=false.                       ║
// ║                                                              ║
// ║  ===== PRODUCTION JAVÍTÁSOK 2026-05-24 =====                ║
// ║  [FIX-ESP-6] WDT visszahelyezése performUpdate() végén az    ║
// ║          esp_task_wdt_delete() után — különben "task not     ║
// ║          found" spam végtelen ideig                          ║
// ║  [FIX-ESP-7] Boot után update.bin maradványok takarítása,    ║
// ║          "update.bin is dir" hibák elkerülésére              ║
// ║  [FIX-ESP-8] WDT deinit boot elején, "TWDT already           ║
// ║          initialized" üzenet elkerülésére soft reset után    ║
// ║  [FIX-ESP-9] OTA_DEBUG=0 production módra — eltávolítja a   ║
// ║          per-csomag log spam-et (OTA packet, FS write...)    ║
// ║                                                              ║
// ║  ===== SPIFFS VÉDELMEK 2026-05-24 =====                     ║
// ║  [FIX-ESP-10] Részleges write detektálás otaWriteBinary-ben  ║
// ║          Ha file.write() kevesebbet ír mint amennyi kellene  ║
// ║          (SPIFFS megtelt), az OTA-t azonnal megszakítjuk     ║
// ║          és töröljük a részleges update.bin-t. Régen végtelen║
// ║          "OTA incomplete" loop volt 96000-en ragadva.        ║
// ║  [FIX-ESP-11] Előzetes méret-ellenőrzés az 0xFE parancsnál:  ║
// ║          ha a firmware nem fér el a SPIFFS-en, hibát küldünk ║
// ║          vissza a kliensnek és nem kezdjük el az OTA-t       ║
// ║          (4 KB tartalékot tartunk a SPIFFS overhead-nek).    ║
// ║  [FIX-ESP-12] 2026-05-24 – ismétlődő "OTA file complete":    ║
// ║          performUpdate() után 5 mp-es nemblokkoló reboot     ║
// ║          várakozás közben az INSTALL_MODE újra meg újra      ║
// ║          lefutott, ismételten triggerelve a complete és      ║
// ║          updateFromFS hívásokat. Javítás: otaTotalBytes = 0  ║
// ║          mielőtt meghívjuk az updateFromFS()-t, így a        ║
// ║          feltételek hamisak lesznek a következő körökben.    ║
// ╚══════════════════════════════════════════════════════════════╝

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

// ===================== DEBUG CONFIG =====================
// [FIX-ESP-9] 2026-05-24: OTA_DEBUG kikapcsolva production módra.
// Ez eltávolítja a sok ezer "OTA packet" sor és "FS write" log üzenetet
// soros monitoron, amely OTA közben elárasztotta a logot.
// DEBUG=1 marad általános üzenetekhez, OTA_DEBUG=0 csak a per-csomag spam-et tiltja.
#define DEBUG 1
#define OTA_DEBUG 0

#if DEBUG
#define DBG(x) Serial.println(F(x))
#define DBG_P(x) Serial.print(F(x))
#else
#define DBG(x)
#define DBG_P(x)
#endif

#if OTA_DEBUG
#define OTA_DBG(x) Serial.println(F(x))
#define OTA_DBG_P(x) Serial.print(F(x))
#else
#define OTA_DBG(x)
#define OTA_DBG_P(x)
#endif

// ===================== VERSION INFO =====================
// [MOD-4] verzió frissítve
// [FIX-ESP-PROD] 2026-05-24: 7.2.0 production verzió, az összes OTA javítással
// [MOD-7] 2026-05-24: 7.3.0 — FIX-ESP-10, FIX-ESP-11, FIX-ESP-12 és MOD-6
// (handleMultiClick) hozzáadva, custom partition table (1.3MB APP + 1.3MB SPIFFS)
// [FIX-ESP-13] 2026-05-30: 7.5.0 — boot reset-detektálás esp_reset_reason()
// alapra állítva (brownout/panic/WDT után újraindul, nem alszik el),
// reset-ok + heap logolás hozzáadva a "30-40 perc után leáll" tünethez
// [FIX-ESP-14] 2026-05-30: 7.6.0 — SPIFFS diag napló (/diag.log): boot reset ok
// + alacsony memória bejegyzés, BLE-n lekérdezhető a DIAG? paranccsal (DIAGCLR
// töröl). Darabolt, nemblokkoló notify streamelés a fan karakterisztikán.
// [FIX-ESP-14b] 2026-05-30: 7.6.1 — naplózás csak ténylegesen szükséges esetben
// (hibás reset + lowmem, a POWERON/DEEPSLEEP/SW kihagyva → OTA-t nem zavarja),
// heap-mentes (String helyett snprintf/stack), kis fájl (512B). Külön Python
// kliens (diag_client.py) a lekérdezéshez AUTH-tal.
// [FIX-ESP-14c] 2026-05-30: 7.6.2 — átnézés utáni javítások: a diag csomagméret
// 20B (alapértelmezett BLE MTU mellett is sértetlen napló), és lowmem-írás
// halasztása streamelés közben (ne csonkoljuk a nyitott naplófájlt).
// [FIX-ESP-15] 2026-05-30: 7.6.3 — enterDeepSleep() forrásának naplózása
// ([sleep] src=button-longpress/idle-timeout/failsafe-timeout), hogy a
// szándékos alvás megkülönböztethető legyen a brownout/panik leállástól.
// [FIX-ESP-16] 2026-05-31: 7.6.4 — OTA magic-byte ellenőrzés az Update.begin()
// előtt. Ha a feltöltött bináris első byte-ja nem 0xE9, érthető "rossz firmware"
// hibát adunk a félrevezető "Decryption error" helyett (amit az arduino-esp32
// Update könyvtár U_AES_DECRYPT_AUTO módja dob nem-0xE9 fejlécre). A hiba a
// diag naplóba is bekerül ([ota] bad magic=0x..).
// [FIX-ESP-18] 2026-06-01: 7.6.5 — RELAY_SWITCH_DELAY_MS 100 -> 10ms.
// A fokozat-váltás break-before-make ideje csökkentve, hogy a teljes táp-
// tranziens (régi fan ki + új fan be) rövid idő alatt lezajljon, és ne legyen
// két külön mély feszültségrogyás. MEGJEGYZÉS: a tényleges break minimum ~20ms
// a checkInterval (20ms) miatt, mert a handleZoneChange() csak annyi időnként
// fut. A 230V AC ventilátor BROWNOUT csak hardveres snubber/MOV-val szűnik meg.
#define FIRMWARE_VERSION "7.6.5"
#define FIRMWARE_DATE "2026-06-01"

// ===================== PINS =====================
#define RELAY_FAN1 10
#define RELAY_FAN2 9
#define RELAY_FAN3 8
#define RELAY_ROLLER 2
#define RELAY_EN 21
#define BUTTON_PIN 3
#define LED_YELLOW 5
#define LED_RED 4

// ===================== FS / OTA DEFINES =====================
#define FLASH SPIFFS
#define FORMAT_SPIFFS_IF_FAILED true

#define OTA_NORMAL_MODE 0
#define OTA_UPDATE_MODE 1
#define OTA_INSTALL_MODE 2

uint8_t otaBuf1[16384];
uint8_t otaBuf2[16384];

// ===================== DIAG LOG (FIX-ESP-14) =====================
// [FIX-ESP-14] 2026-05-30: SPIFFS-be mentett diagnosztikai napló, hogy BLE-n
// keresztül (DIAG? parancs) újraindulás után le lehessen kérdezni MI volt a
// reset oka (pl. BROWNOUT), és hogy mikor fogyott ki a memória.
#define DIAG_LOG_PATH "/diag.log"
const size_t DIAG_LOG_MAX = 512;              // a napló max. mérete (byte) – kicsi
const uint32_t LOW_HEAP_THRESHOLD = 20000;    // ez alatt "kevés memória" bejegyzés
// [FIX-ESP-14c] 20 byte: ez az alapértelmezett BLE MTU (23) melletti biztos
// notify-méret (MTU-3). Így akkor is sértetlen a napló, ha a kliens nem
// egyezteti fel az MTU-t. A napló kicsi (<=512 B), így 20-as darabokkal is
// gyors a letöltés (~26 csomag).
const size_t DIAG_CHUNK_SIZE = 20;            // BLE-n egy csomagban küldött byte
const unsigned long DIAG_CHUNK_INTERVAL = 25; // ms két csomag között (BLE flow control)

#define OTA_SERVICE_UUID "fb1e4001-54ae-4a28-9f74-dfccb248601d"
#define OTA_CHARACTERISTIC_UUID_RX "fb1e4002-54ae-4a28-9f74-dfccb248601d"
#define OTA_CHARACTERISTIC_UUID_TX "fb1e4003-54ae-4a28-9f74-dfccb248601d"

static const char* TAG = "OTA_BOOT";
static BLECharacteristic* pOtaTx = nullptr;
static BLECharacteristic* pOtaRx = nullptr;

static bool otaDeviceConnected = false;
static bool otaSendMode = false;
static bool otaSendSize = true;
static bool otaWriteFile = false;
static bool otaRequest = false;
static int otaWriteLen1 = 0, otaWriteLen2 = 0;
static bool otaCurrentBuf = true;
static int otaParts = 0, otaCur = 0, otaMTU = 0;
static int otaMode = OTA_NORMAL_MODE;
unsigned long otaReceivedBytes = 0, otaTotalBytes = 0;
unsigned long otaLedTimer = 0;
bool otaLedState = false;

// [MOD-1] Új globálisok: nemblokkoló reboot várakozáshoz
bool otaPendingReboot = false;
unsigned long otaRebootAt = 0;

// [MOD-2] Új globálisok: nemblokkoló install várakozáshoz
bool otaInstallWaiting = false;
unsigned long otaInstallWaitUntil = 0;

// [FIX-ESP-14] Diag napló BLE-streameléshez (nemblokkoló állapotgép)
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

SystemState currentState = STATE_NORMAL;

unsigned long lastCheck = 0;
const unsigned long checkInterval = 20;
unsigned long lastBlink = 0;
const unsigned long blinkInterval = 100;
bool blinkState = false;
unsigned long failStart = 0;
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
// [MOD-5] Fordítási idejű figyelmeztetés, ha a PIN üres
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

int currentZone = 0;
int manualZoneIndex = 0;
bool rollerActive = false;
bool relaysEnabled = false;
bool manualMode = false;
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

bool zoneChangeInProgress = false;
unsigned long zoneChangeStart = 0;
int pendingZone = 0;

unsigned long lastPrint1 = 0;
unsigned long lastPrint2 = 0;
unsigned long lastPrint3 = 0;
const unsigned long printInterval = 30000;
bool wasActive = false;

// Boot magic
RTC_NOINIT_ATTR uint32_t bootMagic;
#define BOOT_MAGIC 0xDEADBEEF

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
void failSafeMode();
void ota_boot_flow();
void otaInitService(BLEServer* server);
void otaLoop();
void diagLog(const char* line);
void handleDiagRequest();
bool otaIsRunning() {
  return (otaMode != OTA_NORMAL_MODE);
}

// ===================== OTA HELPERS =====================
static void rebootEspWithReason(String reason) {
  DBG("Rebooting...");
  delay(1000);
  ESP.restart();
}

static void otaWriteBinary(fs::FS& fs, const char* path, uint8_t* dat, int len) {
  // [FIX-ESP-2] 2026-05-24: Debug logging hozzáadva, hogy lássuk, az írás
  // egyáltalán meghívódik-e az utolsó partra. Korábban gyanús volt, hogy
  // az otaReceivedBytes nem éri el otaTotalBytes-t.
  // [FIX-ESP-9] 2026-05-24: Serial.println most az OTA_DBG makró mögé téve,
  // hogy production módban (OTA_DEBUG=0) ne spam-eljen.
  OTA_DBG_P("FS write len=");
#if OTA_DEBUG
  Serial.println(len);
#endif

  File file = fs.open(path, FILE_APPEND);
  if (!file) {
    DBG("FS write fail");
    // [FIX-ESP-3] 2026-05-24: otaWriteFile = false beállítás hibás esetben is,
    // hogy ne maradjon végtelen ciklusban "csak ír" állapotban.
    otaWriteFile = false;
    return;
  }
  size_t written = file.write(dat, len);
  file.close();
  otaWriteFile = false;
  otaReceivedBytes += written;  // [FIX-ESP-4] 2026-05-24: a TÉNYLEGESEN kiírt
                                // byte-okat számoljuk, nem a kértet — így ha
                                // SPIFFS részleges write történt, az kiderül.
  OTA_DBG_P("FS write done, total=");
#if OTA_DEBUG
  Serial.println(otaReceivedBytes);
#endif

  // [FIX-ESP-10] 2026-05-24: Részleges write detektálása. Ha a SPIFFS megtelt,
  // a file.write() 0-t vagy a kértnél kevesebbet ír vissza, de hibát nem dob.
  // Korábban ilyenkor a "total=" ragadva maradt és végtelen "OTA incomplete"
  // ciklus következett. Most az OTA-t azonnal megszakítjuk és tisztítunk.
  if (written < (size_t)len) {
    DBG_P("SPIFFS full! Wrote ");
    Serial.print(written);
    DBG_P(" of ");
    Serial.print(len);
    DBG_P(" bytes (SPIFFS free: ");
    // [FIX-ESP-10b] 2026-05-24: fs::FS osztálynak nincs totalBytes()/usedBytes()
    //  metódusa — ezek a SPIFFSFS-specifikus tagok. FLASH (= SPIFFS) globálist
    //  használjuk közvetlenül, mert ez az egyetlen filesystem amit használunk.
    Serial.print(FLASH.totalBytes() - FLASH.usedBytes());
    DBG(")");
    DBG("Aborting OTA");

    // OTA állapot visszaállítása NORMAL módba
    otaMode = OTA_NORMAL_MODE;
    otaInstallWaiting = false;
    otaInstallWaitUntil = 0;
    otaReceivedBytes = 0;
    otaTotalBytes = 0;
    otaParts = 0;
    otaCur = 0;
    otaMTU = 0;
    otaCurrentBuf = true;
    otaWriteLen1 = 0;
    otaWriteLen2 = 0;
    otaRequest = false;

    // Töröljük a részben felírt update.bin-t
    if (fs.exists(path)) {
      fs.remove(path);
      DBG("Partial update.bin removed");
    }

    // Visszajelzés a kliensnek (Python oldali), hogy megszakadt
    if (pOtaTx) {
      String result = String((char)0x0F) + "ERR: SPIFFS full";
      pOtaTx->setValue(result.c_str());
      pOtaTx->notify();
      delay(200);
    }
  }
}

void ota_boot_flow() {
  // 1) Futó és boot partíció lekérdezése
  const esp_partition_t* running = esp_ota_get_running_partition();
  const esp_partition_t* boot = esp_ota_get_boot_partition();

  DBG("=== OTA BOOT FLOW ===");

  DBG_P("Running partition: type=");
  Serial.print(running->type);
  DBG_P(" subtype=");
  Serial.print(running->subtype);
  DBG_P(" address=0x");
  Serial.println(running->address, HEX);

  DBG_P("Boot partition: type=");
  Serial.print(boot->type);
  DBG_P(" subtype=");
  Serial.print(boot->subtype);
  DBG_P(" address=0x");
  Serial.println(boot->address, HEX);

  // 2) Ha a running != boot → új firmware indult először
  if (running != boot) {
    DBG("New firmware booted FIRST TIME → validating...");

    esp_err_t res = esp_ota_mark_app_valid_cancel_rollback();

    DBG_P("Validation result: ");
    Serial.print(esp_err_to_name(res));
    DBG_P(" (");
    Serial.print(res);
    DBG(")");

    if (res == ESP_OK) {
      DBG("Firmware marked as VALID");
    } else {
      DBG("Firmware validation FAILED → rollback may occur");
    }
  }

  // 3) OTA state lekérdezése
  esp_ota_img_states_t state;
  esp_err_t st = esp_ota_get_state_partition(running, &state);

  if (st == ESP_OK) {
    DBG_P("OTA image state: ");
    Serial.println(state);

    bool pending = (state == ESP_OTA_IMG_NEW);

    DBG_P("Rollback pending: ");
    Serial.println(pending ? "YES" : "NO");

    switch (state) {
      case ESP_OTA_IMG_NEW:
        DBG("Image state: NEW (not validated yet)");
        break;
      case ESP_OTA_IMG_VALID:
        DBG("Image state: VALID");
        break;
      case ESP_OTA_IMG_INVALID:
        DBG("Image state: INVALID");
        break;
      case ESP_OTA_IMG_ABORTED:
        DBG("Image state: ABORTED");
        break;
      default:
        DBG("Image state: UNDEFINED");
        break;
    }
  } else {
    DBG_P("Failed to read OTA state: ");
    Serial.println(esp_err_to_name(st));
  }

  DBG("=== OTA BOOT FLOW END ===");
}

void sendOtaResult(String result) {
  if (!pOtaTx) return;
  pOtaTx->setValue(result.c_str());
  pOtaTx->notify();
  delay(200);
}

// [MOD-1] + [MOD-3] performUpdate: WDT törlés + nemblokkoló reboot várakozás
// Eredeti: delay(5000) a sendOtaResult() után, ami blokkolta a főciklust
// Új:      otaPendingReboot = true + otaRebootAt = millis() + 5000
//          A tényleges reboot az otaLoop() elején fut le, millis() ellenőrzéssel
// [MOD-3]: esp_task_wdt_delete(NULL) hozzáadva a flash írás előtt,
//          mert Update.writeStream() hosszú ideig blokkolhat nagy firmware esetén

void performUpdate(Stream& updateSource, size_t updateSize) {
  String result = String((char)0x0F);

  DBG("=== OTA DEBUG START ===");

  // 0) WDT kikapcsolás
  DBG("WDT delete (flash write may block)...");
  esp_task_wdt_delete(NULL);

  // 1) Partíciók kiírása
  const esp_partition_t* running = esp_ota_get_running_partition();
  const esp_partition_t* next = esp_ota_get_next_update_partition(NULL);

  DBG("Running partition:");
  DBG_P("  addr=0x"); Serial.print(running->address, HEX);
  DBG_P(" size="); Serial.print(running->size);
  DBG_P(" label="); Serial.println(running->label);

  DBG("Next OTA partition:");
  DBG_P("  addr=0x"); Serial.print(next->address, HEX);
  DBG_P(" size="); Serial.print(next->size);
  DBG_P(" label="); Serial.println(next->label);

  DBG_P("updateSize = ");
  Serial.println(updateSize);

  // [FIX-ESP-16] 2026-05-31: magic-byte ellenőrzés az Update.begin() ELŐTT.
  // Egy érvényes ESP32 app-image első byte-ja 0xE9 (ESP_IMAGE_HEADER_MAGIC).
  // Ha nem az, az arduino-esp32 Update könyvtár U_AES_DECRYPT_AUTO módban
  // titkosított image-nek hiszi a binárist, megpróbálja visszafejteni kulcs
  // nélkül, és a félrevezető "Decryption error"-t dobja az Update.end()-nél.
  // A valódi ok ilyenkor: ROSSZ fájl lett feltöltve (nem az app .ino.bin,
  // hanem pl. merged.bin offszettel, .gz tömörített, vagy sérült átvitel).
  // Itt előre elkapjuk és ÉRTHETŐ hibát adunk a "Decryption error" helyett.
  int magic = updateSource.peek();
  DBG_P("First byte (magic) = 0x");
  Serial.println(magic, HEX);
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

  // 2) Update.begin
  DBG("Calling Update.begin()...");
  bool ok = Update.begin(updateSize);
  if (!ok) {
    DBG("Update.begin FAILED!");
    DBG_P("Error code: "); Serial.println(Update.getError());
    DBG_P("Error string: "); Serial.println(Update.errorString());

    result += "Update.begin FAILED: ";
    result += Update.errorString();
    DBG("=== OTA DEBUG END ===");

    esp_task_wdt_add(NULL);
    sendOtaResult(result);
    return;
  }

  DBG("Update.begin OK");

  // 3) writeStream
  DBG("Calling Update.writeStream...");
  size_t written = Update.writeStream(updateSource);

  DBG_P("Update.writeStream returned: ");
  Serial.println(written);

  if (written != updateSize) {
    DBG("WARNING: written != updateSize");
    DBG_P("Expected: "); Serial.println(updateSize);
    DBG_P("Got: "); Serial.println(written);
  }

  // 4) Update.end
  DBG("Calling Update.end()...");
  bool endOK = Update.end();

  DBG_P("Update.end() returned: ");
  Serial.println(endOK ? "true" : "false");

  if (!endOK) {
    DBG("Update.end FAILED");
    DBG_P("Error code: "); Serial.println(Update.getError());
    DBG_P("Error string: "); Serial.println(Update.errorString());

    result += "Update.end FAILED: ";
    result += Update.errorString();
    DBG("=== OTA DEBUG END ===");

    esp_task_wdt_add(NULL);
    sendOtaResult(result);
    return;
  }

  // 5) isFinished
  DBG_P("Update.isFinished(): ");
  Serial.println(Update.isFinished() ? "true" : "false");

  if (!Update.isFinished()) {
    DBG("ERROR: Update not finished!");
  }

  DBG("=== OTA DEBUG END ===");

  // 6) WDT vissza
  DBG("WDT add back");
  esp_task_wdt_add(NULL);

  // 7) Eredmény visszaküldése
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

/*
void performUpdate(Stream& updateSource, size_t updateSize) {
  String result = String((char)0x0F);

  // [MOD-3] WDT törlése: az Update.writeStream() akár több másodpercig fut,
  // ami 15s-es timeout esetén is triggerelheti a watchdogot nagy firmwarenél
  esp_task_wdt_delete(NULL);


  if (Update.begin(updateSize)) {
    size_t written = Update.writeStream(updateSource);
    OTA_DBG_P("OTA written: ");
    OTA_DBG(String(written).c_str());
    result += "Written: " + String(written) + "/" + String(updateSize) + "\n";
    if (Update.end()) {
      result += "OTA done: ";
      if (Update.isFinished()) {
        result += "OK\n";
      } else {
        result += "Not finished\n";
      }
    } else {
      result += "Error: " + String(Update.getError());
    }
  } else {
    result += "No space for OTA";
  }

  // [FIX-ESP-6] 2026-05-24: WDT visszahelyezése a flash írás után.
  // Az esp_task_wdt_delete(NULL) eltávolította a fő taskot a WDT listából,
  // ezért minden további esp_task_wdt_reset() hívás "task not found" hibát
  // dob a logba végtelenül. Az esp_task_wdt_add(NULL) visszateszi a taskot,
  // így a reset hívások újra működnek.
  esp_task_wdt_add(NULL);

  if (otaDeviceConnected) {
    sendOtaResult(result);
    // [MOD-1] delay(5000) → nemblokkoló flag alapú várakozás
    otaPendingReboot = true;
    otaRebootAt = millis() + 5000;
  } else {
    // Ha nincs csatlakozva eszköz, azonnal rebootolhat
    rebootEspWithReason("OTA done");
  }
}
*/

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

    // [MOD-1] rebootEspWithReason() hívás eltávolítva innen:
    // a reboot most az otaLoop()-ban történik az otaPendingReboot flag alapján.
    // Ha performUpdate() azonnal rebootolt (nincs BLE), ide nem jutunk el.
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

    // [FIX-ESP-14] Ha épp diag naplót streameltünk, zárjuk le tisztán
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
      otaRequest = false;
      otaParts = 0;
      otaCur = 0;
      otaMTU = 0;
      otaCurrentBuf = true;
      otaWriteLen1 = 0;
      otaWriteLen2 = 0;
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
      Serial.println(val);
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
      Serial.println(zone);

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
      Serial.println(rollerCmd);

    } else if (val.startsWith("DIAG?")) {
      // [FIX-ESP-14] Diag napló lekérése (reset ok + lowmem history)
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
      // [FIX-ESP-14] Diag napló törlése
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
      int pos = pData[1];
      for (int x = 0; x < len - 2; x++) {
        if (otaCurrentBuf) {
          otaBuf1[(pos * otaMTU) + x] = pData[x + 2];
        } else {
          otaBuf2[(pos * otaMTU) + x] = pData[x + 2];
        }
      }

    } else if (pData[0] == 0xFC) {
      OTA_DBG_P("0xFC part=");
      Serial.println((pData[3] * 256) + pData[4]);
      if (otaCurrentBuf) {
        otaWriteLen1 = (pData[1] * 256) + pData[2];
      } else {
        otaWriteLen2 = (pData[1] * 256) + pData[2];
      }
      otaCurrentBuf = !otaCurrentBuf;
      otaCur = (pData[3] * 256) + pData[4];
      otaWriteFile = true;
      if (otaCur < otaParts - 1) {
        otaRequest = true;
      }

    } else if (pData[0] == 0xFD) {
      otaSendMode = true;
      if (FLASH.exists("/update.bin")) {
        FLASH.remove("/update.bin");
      }

    } else if (pData[0] == 0xFE) {
      otaReceivedBytes = 0;
      otaTotalBytes = ((uint32_t)pData[1] << 24) | ((uint32_t)pData[2] << 16) | ((uint32_t)pData[3] << 8) | ((uint32_t)pData[4]);
      uint32_t fsFree = FLASH.totalBytes() - FLASH.usedBytes();
      DBG_P("FS free: ");
      Serial.println(fsFree);
      DBG_P("OTA size: ");
      Serial.println(otaTotalBytes);

      // [FIX-ESP-11] 2026-05-24: Előzetes méret-ellenőrzés. Ha a SPIFFS-en
      // nincs elég hely a teljes update.bin-hez, ne is kezdjünk neki — azonnal
      // hibát küldünk a kliensnek. Régen ez csak menet közben derült ki, és
      // részlegesen felírt fájlt hagyott maga után. A free space-hez +4KB
      // tartalék kell (SPIFFS metaadatok, blokk-overhead).
      const uint32_t SPIFFS_OVERHEAD = 4096;
      if (otaTotalBytes + SPIFFS_OVERHEAD > fsFree) {
        DBG("ERR: SPIFFS too small for OTA");
        DBG_P("Need (with overhead): ");
        Serial.println(otaTotalBytes + SPIFFS_OVERHEAD);
        DBG_P("Available: ");
        Serial.println(fsFree);

        if (pOtaTx) {
          String result = String((char)0x0F) + "ERR: SPIFFS too small (need " + String(otaTotalBytes + SPIFFS_OVERHEAD) + ", have " + String(fsFree) + ")";
          pOtaTx->setValue(result.c_str());
          pOtaTx->notify();
          delay(200);
        }

        // OTA állapot visszaállítása
        otaMode = OTA_NORMAL_MODE;
        otaTotalBytes = 0;
        otaReceivedBytes = 0;
        return;
      }

    } else if (pData[0] == 0xFF) {
      otaParts = (pData[1] * 256) + pData[2];
      otaMTU = (pData[3] * 256) + pData[4];
      otaMode = OTA_UPDATE_MODE;
      DBG_P("OTA parts: ");
      Serial.println(otaParts);

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
  } else {
    if (currentZone == 0) {
      deactivateRoller();
      delay(100);
      disableRelays();
    }
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

    // Vissza automata üzemmódba
    manualMode = false;
    bleEnabled = true;

    // Ventilátor kikapcsolása (kézi módban beállított zóna törlése)
    manualZoneIndex = 0;
    setFanZone(0, SRC_BUTTON);

    // BLE újraindítása, hogy újra fogadjon csatlakozást
    BLEDevice::startAdvertising();
    DBG("Manual mode OFF, BLE advertising restarted");
    return;
  }
}

// ===================== OTA SERVICE INIT =====================
void otaInitService(BLEServer* server) {
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

// [FIX-ESP-13] 2026-05-30: Olvasható string a reset okhoz, hogy a soros
// monitoron / PC oldalon azonnal látsszon, mi indította újra az eszközt.
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

// [FIX-ESP-14] 2026-05-30: Egy sor hozzáfűzése a diag naplóhoz. Heap-mentes
// (csak stack puffer, nincs String → nincs fragmentáció). Csak akkor írunk
// flash-t, ha tényleg kell (lásd a hívási helyeket: csak hibás reset / lowmem).
// Ha a fájl túllépi a DIAG_LOG_MAX-ot, megtartjuk az utolsó felét.
void diagLog(const char* line) {
  // Méret-cap: csak akkor írjuk újra a fájlt, ha tényleg túl nagy
  if (FLASH.exists(DIAG_LOG_PATH)) {
    File f = FLASH.open(DIAG_LOG_PATH, FILE_READ);
    if (f) {
      size_t sz = f.size();
      if (sz > DIAG_LOG_MAX) {
        uint8_t tmp[DIAG_LOG_MAX / 2];
        f.seek(sz - sizeof(tmp));
        int n = f.read(tmp, sizeof(tmp));
        f.close();
        // az első (részleges) sort eldobjuk, hogy ép sorral kezdődjön
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

// [FIX-ESP-14] 2026-05-30: A DIAG? / DIAGCLR parancsok nemblokkoló kiszolgálása.
// A BLE onWrite callback CSAK flag-et állít (diagRequested/diagClearRequested),
// a tényleges SPIFFS olvasás és a darabolt notify itt, a fő loopban történik –
// így nem blokkoljuk a BLE stack taskját és nem árasztjuk el a notify buffert.
//
// Protokoll a fan karakterisztikán (ffe1), válasz a kliensnek:
//   0x02 "DIAG_BEGIN"   – stream kezdete
//   <nyers napló byte-ok ... DIAG_CHUNK_SIZE-os darabokban>
//   0x04 "DIAG_END"     – stream vége
// A kliens a 0x02/0x04 vezérlőjelek közti byte-okat fűzi össze.
void handleDiagRequest() {
  if (!pCharacteristic) return;

  // Törlés kérés
  if (diagClearRequested) {
    diagClearRequested = false;
    if (FLASH.exists(DIAG_LOG_PATH)) FLASH.remove(DIAG_LOG_PATH);
    pCharacteristic->setValue("DIAG_CLEARED");
    pCharacteristic->notify();
    DBG("Diag log cleared");
    return;
  }

  // Stream indítása
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

  // Darabok küldése (időzítve, hogy ne floodoljuk a BLE-t)
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
  Serial.begin(115200);
  delay(100);

  ota_boot_flow();

  // [MOD-5] Futásidejű ellenőrzés – ha PIN üres, Serial warningot dob
  static_assert(sizeof(BLE_AUTH_PIN) > 1, "BLE_AUTH_PIN is empty!");

  DBG("Boot");

  if (!SPIFFS.begin(FORMAT_SPIFFS_IF_FAILED)) {
    DBG("SPIFFS mount fail");
  }

  // [FIX-ESP-7] 2026-05-24: Sikeres OTA után az updateFromFS() törli az
  // update.bin-t, de ha a reboot előtt megszakadt valami (pl. áramkimaradás),
  // a fájl ott marad SPIFFS-ben. Boot után detektáljuk és töröljük, hogy
  // a következő OTA tiszta állapotból induljon. Ezenkívül néha az is
  // előfordulhat, hogy update.bin könyvtárként marad ott ("update.bin is dir"
  // log üzenet) — ezt is kezeljük.
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

  // [FIX-ESP-8] 2026-05-24: Boot után az "esp_task_wdt_init: TWDT already
  // initialized" hibát az okozza, hogy soft reset után a WDT állapota
  // megmaradhat. Az esp_task_wdt_deinit() biztosítja, hogy tiszta állapotból
  // induljunk az esp_task_wdt_init() előtt. Hibát ignoráljuk, mert ha
  // nem volt inicializálva, a deinit nem kritikus.
  esp_task_wdt_deinit();
  esp_task_wdt_init(&wdt_config);
  esp_task_wdt_add(NULL);

  // [FIX-ESP-13] 2026-05-30: A korábbi logika az esp_sleep_get_wakeup_cause() +
  // RTC_NOINIT bootMagic kombinációval próbálta megkülönböztetni a power-on-t a
  // soft reset-től. PROBLÉMA: relék/ventilátorok kapcsolásakor (induktív
  // terhelés, bekapcsolási áramlökés) vagy BLE adás közbeni áramcsúcsoknál
  // BROWNOUT reset történhet, ami az ESP32-C3-on törölheti az RTC memóriát →
  // bootMagic != BOOT_MAGIC → a kód "power-on"-nak hitte és DEEP SLEEP-be ment.
  // Innen jött a "30-40 perc után váratlanul leáll, mintha deep sleepbe menne"
  // tünet: a brownout reset után az eszköz elaludt ahelyett, hogy újraindult
  // volna, és csak gombnyomásra ébredt fel.
  //
  // MEGOLDÁS: esp_reset_reason() használata, ami megbízhatóan megadja a reset
  // okát. Csak VALÓDI hidegindítás (POWERON) esetén alszunk el és várunk
  // gombnyomásra. Deep sleepből csak gombbal ébredünk. Minden HIBÁS reset
  // (BROWNOUT, PANIC, WDT, SW) esetén ÚJRAINDULUNK és folytatjuk a normál
  // működést (BLE advertising újraindul) — az eszköz nem marad "halott".
  esp_reset_reason_t resetReason = esp_reset_reason();
  lastBootResetReason = resetReason;  // [FIX-ESP-19] globális mentés
  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
  bootMagic = BOOT_MAGIC;

  Serial.println();
  Serial.println(F("===================================="));
  Serial.println(F("Xiao ESP32C3 Fan + Roller + OTA"));
  Serial.print(F("FW: v"));
  Serial.print(F(FIRMWARE_VERSION));
  Serial.print(F(" ("));
  Serial.print(F(FIRMWARE_DATE));
  Serial.println(F(")"));
  Serial.print(F("Reset reason: "));
  Serial.print((int)resetReason);
  Serial.print(F(" ("));
  Serial.print(resetReasonStr(resetReason));
  Serial.println(F(")"));
  Serial.println(F("===================================="));

  // [FIX-ESP-14] 2026-05-30: Reset ok mentése a diag naplóba – DE CSAK ha
  // tényleg hiba volt. A várt reseteket (POWERON, DEEPSLEEP-ből ébredés, és a
  // szándékos SW reset pl. OTA után) NEM naplózzuk, így nem koptatjuk a flash-t
  // és nem zavarjuk az OTA-t. Csak PANIC/WDT/BROWNOUT/EXT/SDIO/UNKNOWN kerül be.
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
    // Deep sleepből ébredtünk – csak gombnyomásra induljon el a rendszer
    if (wakeup_reason == ESP_SLEEP_WAKEUP_GPIO) {
      DBG("Wake: button");
    } else {
      DBG("Deep sleep wake (no button) → back to sleep");
      Serial.flush();
      delay(100);
      pinMode(BUTTON_PIN, INPUT_PULLUP);
      esp_deep_sleep_enable_gpio_wakeup(BIT(BUTTON_PIN), ESP_GPIO_WAKEUP_GPIO_LOW);
      esp_deep_sleep_start();
    }
  } else if (resetReason == ESP_RST_POWERON) {
    // Valódi hidegindítás (tápra dugás) → alvás, gombnyomásra indul
    DBG("Power-on → sleep (wait for button)");
    Serial.flush();
    delay(100);
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    esp_deep_sleep_enable_gpio_wakeup(BIT(BUTTON_PIN), ESP_GPIO_WAKEUP_GPIO_LOW);
    esp_deep_sleep_start();
  } else {
    // BROWNOUT / PANIC / WDT / SW reset → hibából való helyreállás:
    // FOLYTATJUK a normál működést (NEM alszunk el), így az eszköz
    // automatikusan újraindul és újra elérhető lesz BLE-n.
    DBG("Fault/SW reset → resuming normal operation");
  }

  DBG("GPIO init");
  pinMode(RELAY_FAN1, OUTPUT);
  pinMode(RELAY_FAN2, OUTPUT);
  pinMode(RELAY_FAN3, OUTPUT);
  pinMode(RELAY_ROLLER, OUTPUT);
  pinMode(RELAY_EN, OUTPUT);
  pinMode(LED_YELLOW, OUTPUT);
  pinMode(LED_RED, OUTPUT);

  DBG("Relays safe OFF");
  digitalWrite(RELAY_EN, LOW);
  delay(100);
  digitalWrite(RELAY_FAN1, HIGH);
  digitalWrite(RELAY_FAN2, HIGH);
  digitalWrite(RELAY_FAN3, HIGH);
  digitalWrite(RELAY_ROLLER, HIGH);
  relaysEnabled = false;

  DBG("LED boot");
  digitalWrite(LED_YELLOW, HIGH);
  digitalWrite(LED_RED, LOW);

  DBG("Button init");
  button.attachClick(handleClick);
  button.attachLongPressStop(handleLongPressStop);
  button.attachDoubleClick(handleDoubleClick);
  button.attachMultiClick(handleMultiClick);
  button.setPressTicks(2000);
  button.setClickTicks(400);

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

  Serial.println();
  DBG_P("Free heap: ");
  Serial.println(ESP.getFreeHeap());

  // [FIX-ESP-19] 2026-06-01: BROWNOUT/UNKNOWN reset után görgő bekapcs.
  // Ha az eszköz BROWNOUT miatt resetelt (a 230V AC ventilátor terhelésétől),
  // boot után azonnal bekapcsoljuk a görgőt + relékre, hogy az eszköz
  // működőképes maradjon és ne legyen "halott" állapot.
  if (lastBootResetReason == ESP_RST_BROWNOUT ||
      lastBootResetReason == ESP_RST_UNKNOWN) {
    DBG("Boot after BROWNOUT/UNKNOWN → activating roller");
    enableRelays();
    delay(100);
    activateRoller();
  }

  digitalWrite(LED_YELLOW, LOW);
}

// ===================== LOOP =====================
void loop() {
  esp_task_wdt_reset();
  unsigned long now2 = millis();

  if (now2 - lastCheck >= checkInterval) {
    lastCheck = now2;
    stateMachineStep();
  }

  otaLoop();

  // [FIX-ESP-14] Diag napló BLE-kiszolgálása (csak ha nem fut OTA)
  if (!otaIsRunning()) handleDiagRequest();
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

  int f1 = digitalRead(RELAY_FAN1);
  int f2 = digitalRead(RELAY_FAN2);
  int f3 = digitalRead(RELAY_FAN3);

  int lowCount = (f1 == LOW) + (f2 == LOW) + (f3 == LOW);

  if (lowCount >= 2) {
    DBG("FAILSAFE: 2 fans LOW");
    currentState = STATE_FAILSAFE;
    return;
  }

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

    DBG_P("To sleep (min): ");
    Serial.println(remainingMs / 60000);

    // [FIX-ESP-13] 2026-05-30: Heap monitorozás. Ha a "Free heap" 30-40 perc
    // alatt folyamatosan csökken, memóriaszivárgás van (pl. BLE újracsatlakozási
    // ciklusok). A "min" a valaha mért legkisebb szabad heap — ha ez vészesen
    // alacsony, az heap-kifogyás miatti összeomlást (és resetet) okozhat.
    DBG_P("Free heap: ");
    Serial.print(ESP.getFreeHeap());
    DBG_P(" / min: ");
    Serial.println(ESP.getMinFreeHeap());
  }

  // [FIX-ESP-14] 2026-05-30: Alacsony memória detektálás. Ha a szabad heap a
  // küszöb alá esik, egyszer bejegyezzük a diag naplóba (BLE-n lekérdezhető).
  // Hiszterézissel: csak akkor logolunk újra, ha közben visszaállt a heap.
  static bool lowHeapLogged = false;
  uint32_t freeHeapNow = ESP.getFreeHeap();
  if (freeHeapNow < LOW_HEAP_THRESHOLD) {
    // [FIX-ESP-14c] Ne írjunk a naplóba, amíg épp streameljük (a diagFile
    // nyitva van olvasásra) – különben ugyanazt a fájlt csonkolnánk/írnánk.
    // A lowHeapLogged false marad, így a stream befejeztével rögzül a bejegyzés.
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

  yield();
}

void failSafeMode() {
  if (!failStartSet) {
    failStart = millis();
    failStartSet = true;
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

  if (nowfailSafeMode - failStart >= 60000) {
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

  DBG_P("Zone change: ");
  Serial.print(fromZone);
  Serial.print(" -> ");
  Serial.println(zone);
}

void handleZoneChange() {
  unsigned long nowhandleZoneChange = millis();

  // [MOD] zoneChangeStart és pendingZone kiolvasása kritikus szekción belül,
  // összhangban azzal ahogy setFanZone() írja őket
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

  // Időellenőrzés már a lokális másolattal, szekción kívül
  if (nowhandleZoneChange >= localZoneChangeStart) {
    if (nowhandleZoneChange - localZoneChangeStart < RELAY_SWITCH_DELAY_MS) {
      return;
    }
  }

  // Relay váltás és állapot frissítés
  portENTER_CRITICAL(&zoneMux);

  currentZone = localPendingZone;

  switch (localPendingZone) {
    case 1: digitalWrite(RELAY_FAN1, LOW); break;
    case 2: digitalWrite(RELAY_FAN2, LOW); break;
    case 3: digitalWrite(RELAY_FAN3, LOW); break;
    case 0: break;
  }

  zoneChanging = false;
  zoneChangeInProgress = false;

  portEXIT_CRITICAL(&zoneMux);

  switch (localPendingZone) {
    case 1: DBG("Fan1 ON (33%)"); break;
    case 2: DBG("Fan2 ON (66%)"); break;
    case 3: DBG("Fan3 ON (100%)"); break;
    case 0: DBG("All fans OFF"); break;
  }
}

// ===================== ROLLER CONTROL =====================
void activateRoller() {
  digitalWrite(RELAY_ROLLER, LOW);
  rollerActive = true;
  DBG("Roller ON");
}

void deactivateRoller() {
  digitalWrite(RELAY_ROLLER, HIGH);
  rollerActive = false;
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
  Serial.println(F("===================================="));
  Serial.println(F("Enter deep sleep"));
  Serial.print(F("Reason: "));
  Serial.println(reason);
  Serial.println(F("===================================="));

  // [FIX-ESP-15] 2026-05-30: Honnan indult a deep sleep, a diag naplóba is.
  // Így teszteléskor megkülönböztethető a SZÁNDÉKOS alvás (gomb / tétlenség /
  // failsafe) a brownout/panik miatti "leállástól". Heap-mentes (stack puffer).
  {
    char e[64];
    snprintf(e, sizeof(e), "[sleep] src=%s heap=%u t=%lus",
             reason, (unsigned)ESP.getFreeHeap(),
             (unsigned long)(millis() / 1000));
    diagLog(e);
  }

  if (bleEnabled) {
    DBG("BLE stop");
    if (bleConnected) {
      pServer->disconnect(0);
      delay(200);
    }
    BLEDevice::stopAdvertising();
    delay(100);
    bleConnected = false;
    bleEnabled = false;
  }

  DBG("Relays OFF before sleep");
  disableRelays();

  DBG("LEDs OFF");
  digitalWrite(LED_RED, LOW);
  digitalWrite(LED_YELLOW, LOW);

  DBG("Deep sleep on BTN");
  esp_deep_sleep_enable_gpio_wakeup(BIT(BUTTON_PIN), ESP_GPIO_WAKEUP_GPIO_LOW);

  Serial.flush();
  esp_deep_sleep_start();
}

// ===================== OTA LOOP =====================
void otaLoop() {
  if (!pOtaTx || !pOtaRx) return;

  // [MOD-1] Nemblokkoló reboot várakozás
  // Eredeti: performUpdate()-ban delay(5000), blokkolta a főciklust
  // Új: flag + millis() ellenőrzés itt, a főciklus folytatódhat közben
  if (otaPendingReboot && millis() >= otaRebootAt) {
    rebootEspWithReason("OTA done");
  }

  // Gyors LED váltakozás OTA alatt
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
        if (otaSendMode) {
          uint8_t fMode[] = { 0xAA, 0x00 };
          pOtaTx->setValue(fMode, 2);
          pOtaTx->notify();
          delay(50);
          otaSendMode = false;
        }

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

      if (otaRequest) {
        uint8_t rq[] = { 0xF1, (uint8_t)((otaCur + 1) / 256), (uint8_t)((otaCur + 1) % 256) };
        pOtaTx->setValue(rq, 3);
        pOtaTx->notify();
        delay(50);
        otaRequest = false;
      }

      // [FIX-ESP-1] 2026-05-24: otaWriteFile ellenőrzés ELŐRE hozva.
      if (otaWriteFile) {
        if (!otaCurrentBuf) {
          otaWriteBinary(FLASH, "/update.bin", otaBuf1, otaWriteLen1);
        } else {
          otaWriteBinary(FLASH, "/update.bin", otaBuf2, otaWriteLen2);
        }
      }

      // [FIX-ESP-5] 2026-05-24: A valódi hiba végre megtalálva!
      // A SPIFFS write (otaWriteBinary) több 100ms-ig fut, eközben a BLE
      // callback szálon megérkezhet a 42. (utolsó) part 0xFC-je is, ami
      // újra otaWriteFile=true-t állít, otaCur=41-et és otaCurrentBuf-ot
      // megfordít. AMIKOR az otaWriteBinary befejeződik, otaWriteFile=false
      // lesz (az ELŐZŐ 41. part írása után), és a következő loop sorban
      // az otaCur+1==otaParts feltétel azonnal teljesül — INSTALL_MODE-ba
      // váltunk, MIELŐTT a 42. part otaWriteFile=true újra kiváltaná az
      // írást. Eredmény: 1728 byte örökre elveszik.
      //
      // A javítás: a módváltást CSAK akkor engedjük, ha otaWriteFile FALSE,
      // ÉS az otaCurrentBuf a megfelelő (kiindulási) állapotban van.
      // Ha még otaWriteFile=true, vagy a flag éppen most lett beállítva
      // (még írás vár), akkor maradjunk UPDATE_MODE-ban — a következő
      // loop kör elvégzi az írást, és csak utána lép át.
      if (otaCur + 1 == otaParts && !otaWriteFile) {
        uint8_t com[] = { 0xF2, (uint8_t)((otaCur + 1) / 256), (uint8_t)((otaCur + 1) % 256) };
        pOtaTx->setValue(com, 3);
        pOtaTx->notify();
        delay(50);
        otaMode = OTA_INSTALL_MODE;
      }

      break;

    // [MOD-2] OTA_INSTALL_MODE: mindkét delay(2000) kiváltva nemblokkoló várakozással
    // Eredeti: delay(2000) a "file complete" és az "incomplete" ágban is
    // Új: otaInstallWaiting flag + otaInstallWaitUntil millis() időbélyeg
    //     A ciklus a várakozás alatt visszatér, és nem blokkolja a főciklust
    case OTA_INSTALL_MODE:

      // [FIX-ESP-1c] 2026-05-24: A buffer logika HELYES iránya:
      // A 0xFC callback-ban:
      //   if (otaCurrentBuf) { otaWriteLen1 = ...; }   ← buf1-be írtunk
      //   else               { otaWriteLen2 = ...; }   ← buf2-be írtunk
      //   otaCurrentBuf = !otaCurrentBuf;              ← MEGFORDUL
      //
      // Ezért az írás idejére:
      //   - otaCurrentBuf most TRUE  → az imént FALSE volt → buf2-be írtunk → buf2-t kell írni
      //   - otaCurrentBuf most FALSE → az imént TRUE  volt → buf1-be írtunk → buf1-t kell írni
      //
      // Tehát a HELYES logika (ugyanaz mint az UPDATE_MODE-ban):
      //   if (!otaCurrentBuf) write(buf1, len1);   ← most FALSE → buf1
      //   else                write(buf2, len2);   ← most TRUE  → buf2
      //
      // Az előző FIX-ESP-1b verzió fordítva volt → a rossz (üres) buffert írta,
      // és az otaReceivedBytes nem növekedett, mert a write SPIFFS-en lefutott
      // ugyan, de 0 byte-tal vagy hibás adattal.
      // A duplikáció nem probléma, mert otaWriteBinary() beállítja
      // otaWriteFile = false-ra a sikeres írás után.
      if (otaWriteFile) {
        if (!otaCurrentBuf) {
          otaWriteBinary(FLASH, "/update.bin", otaBuf1, otaWriteLen1);
        } else {
          otaWriteBinary(FLASH, "/update.bin", otaBuf2, otaWriteLen2);
        }
      }

      // Ha várakozás folyamatban van, csak az időt ellenőrizzük
      if (otaInstallWaiting) {
        if (millis() >= otaInstallWaitUntil) {
          otaInstallWaiting = false;
          // Időlejárat után: ha minden megérkezett, telepítés
          if (otaReceivedBytes == otaTotalBytes && otaTotalBytes > 0) {
            // [FIX-ESP-12] 2026-05-24: otaTotalBytes nullázása MIELŐTT meghívjuk
            // az updateFromFS()-t. A performUpdate() ugyanis nem rebootol azonnal
            // (otaPendingReboot flag + 5 mp), hanem visszatér, és addig az
            // INSTALL_MODE ág újra meg újra lefut, ismételten triggerelve az
            // "OTA file complete" üzenetet és próbálkozva updateFromFS()-szel.
            // otaTotalBytes = 0 után az alábbi feltételek hamisak lesznek,
            // így az INSTALL_MODE csendben várja a reboot-ot.
            uint32_t savedTotal = otaTotalBytes;
            otaTotalBytes = 0;
            otaReceivedBytes = 0;
            updateFromFS(FLASH);
            (void)savedTotal;  // ha esetleg debug-hoz kéne
            // updateFromFS → performUpdate → otaPendingReboot=true,
            // a tényleges reboot az otaLoop() tetején történik 5 mp múlva
          }
        }
        break;  // Várakozás alatt nem futtatjuk le az alábbi logikát
      }

      if (otaReceivedBytes == otaTotalBytes && otaTotalBytes > 0) {
        DBG("OTA file complete");
        // [MOD-2] delay(2000) → nemblokkoló várakozás indítása
        otaInstallWaiting = true;
        otaInstallWaitUntil = millis() + 2000;

      } else if (otaTotalBytes > 0) {
        // [MOD] otaWriteFile = true eltávolítva – duplikált írást okozhatott.
        // Ha még nem érkezett meg minden byte, egyszerűen várunk.
        // Az otaWriteFile-t csak az OtaCallbacks::onWrite() állíthatja be,
        // amikor valóban érkezik új csomag.
        DBG("OTA incomplete");
        DBG_P("Expected: ");
        Serial.println(otaTotalBytes);
        DBG_P("Received: ");
        Serial.println(otaReceivedBytes);
        // [MOD-2] delay(2000) → nemblokkoló várakozás indítása
        otaInstallWaiting = true;
        otaInstallWaitUntil = millis() + 2000;
      }
      break;
  }
}