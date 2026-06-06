# FanController_OTA_debug

ESP32-C3 alapú **háromfokozatú ventilátor- és görgővezérlő**, BLE-n keresztül
irányítható, OTA firmware-frissítéssel, és beépített diagnosztikai naplóval.

**Aktuális firmware verzió:** `7.7.0` (2026-06-02)

---

## Tartalom

- [Áttekintés](#áttekintés)
- [Hardver / pinkiosztás](#hardver--pinkiosztás)
- [Fan relé KIMENET figyelése (H11AA1M)](#fan-relé-kimenet-figyelése-h11aa1m)
- [Működés](#működés)
  - [Üzemmódok](#üzemmódok)
  - [Gombvezérlés](#gombvezérlés)
  - [BLE vezérlés és parancsok](#ble-vezérlés-és-parancsok)
  - [Relé-kapcsolás (break-before-make)](#relé-kapcsolás-break-before-make)
  - [Failsafe (biztonsági) állapot](#failsafe-biztonsági-állapot)
  - [Deep sleep (energiatakarékos alvás)](#deep-sleep-energiatakarékos-alvás)
- [Fokozat-mentés és visszaállítás (RTC + NVS hibrid)](#fokozat-mentés-és-visszaállítás-rtc--nvs-hibrid)
- [Diagnosztikai napló](#diagnosztikai-napló)
- [OTA firmware-frissítés](#ota-firmware-frissítés)
- [Particionálás](#particionálás)
- [Diagnosztikai eszközök (Python)](#diagnosztikai-eszközök-python)
- [Időzítések összefoglaló](#időzítések-összefoglaló)
- [Verziótörténet (kivonat)](#verziótörténet-kivonat)

---

## Áttekintés

Az eszköz egy 230V AC ventilátort vezérel három fokozatban (33% / 66% / 100%),
plusz egy „görgő" (roller) relét. A vezérlés történhet:

- **BLE-n** (telefonról / PC-ről, PIN-es hitelesítéssel), vagy
- **fizikai gombbal** (kézi mód).

A firmware kiemelt figyelmet fordít a **tápoldali instabilitásra (BROWNOUT)**,
amit a 230V induktív terhelés relé-kapcsoláskori árama okoz. Ezért:

- a fokozatot **azonnal** és **tartósan** is elmenti (hibrid RTC + NVS),
- hibás reset után **automatikusan visszaáll** a kapcsolás előtti állapotba,
- minden hibás resetet **naplóz**, ami BLE-n lekérdezhető.

---

## Hardver / pinkiosztás

| Funkció | GPIO |
|---|---|
| Ventilátor 1. fokozat relé (`RELAY_FAN1`) | 10 |
| Ventilátor 2. fokozat relé (`RELAY_FAN2`) | 9 |
| Ventilátor 3. fokozat relé (`RELAY_FAN3`) | 8 |
| Görgő relé (`RELAY_ROLLER`) | 2 |
| Relé tápengedélyezés (`RELAY_EN`) | 21 |
| Nyomógomb (`BUTTON_PIN`) | 3 |
| Sárga LED (`LED_YELLOW`) | 5 |
| Piros LED (`LED_RED`) | 4 |
| Fan1 kimenet-figyelő opto (`FAN1_SENSE_PIN`, H11AA1M) | 6 |
| Fan2 kimenet-figyelő opto (`FAN2_SENSE_PIN`, H11AA1M) | 7 |
| Fan3 kimenet-figyelő opto (`FAN3_SENSE_PIN`, H11AA1M) | 20 |

> A relék **aktív-LOW** vezérlésűek (`digitalWrite(..., LOW)` = bekapcsol).
>
> A `FANx_SENSE_PIN` lábak **belső pullup**-pal (`INPUT_PULLUP`) működnek. A
> GPIO20 az ESP32-C3 U0RXD lába — ez csak akkor szabad, ha a `Serial` **USB-CDC**
> (és nem a hardveres UART0). Mivel a `RELAY_EN` a GPIO21 (U0TXD) kimenetként van
> használva, ez itt teljesül.

---

## Fan relé KIMENET figyelése (H11AA1M)

3 db **H11AA1M** AC-bemenetű optocsatoló figyeli, hogy a fan relék **kimenetén**
ténylegesen megjelenik-e a **230V AC**. Bekötés (feltételezett): az opto
fototranzisztor kollektora a `FANx_SENSE_PIN`-re (belső pullup), emittere GND-re,
a bemeneti LED-pár a relé kimenete (terhelés) és nulla közé, soros áramkorlátozó
ellenállással.

**FONTOS — miért nem elég egy `digitalRead`:** a H11AA1M bemenetén antiparallel
LED-pár van, ami az AC **mindkét** félhullámán vezet, **kivéve a nullátmenetek
körül**. Ezért a kimenet 230V AC jelenlétében **nem folyamatosan alacsony**,
hanem ~**100 Hz**-cel (50 Hz hálózat → félhullámonként egyszer) rövid időre
HIGH-ra ugrik. A program ezért **idő-ablakot** figyel: ha az utóbbi
`AC_SENSE_WINDOW_MS` (**40 ms**, > 1 hálózati periódus) ideje alatt **volt LOW
minta**, akkor van AC a kimeneten; folyamatos HIGH → nincs AC. Erre `80 ms`
debounce és a relé-parancs utáni `1500 ms` türelmi idő épül.

A mért állapotot (`fanLineLive[]`) a program összeveti az **elvárttal**
(`relaysEnabled && currentZone == fan`), és a két eltérés-irányra **aszimmetrikusan**
reagál (`DIAG?` paranccsal a `diag.log` lekérdezhető):

- **STUCK** – a zóna **OFF**, de **van AC** a kimeneten → beragadt/hegedt relé.
  Reakció: **azonnali `STATE_FAILSAFE`** + figyelmeztetés + `diag.log`. A `diag.log`
  **szinkron** (flush) íródik a failsafe-be lépés **előtt**, így a naplózás nem
  szakad félbe. (A `fanLineLive` ekkor már a 40 ms ablak + 80 ms debounce-on átment.)
- **NOAC** – a zóna **ON**, de **nincs AC** → relé/biztosíték/ventilátor/hálózat hiba.
  Reakció: `FAN_SENSE_MISMATCH_CONFIRM_MS` (**1000 ms**) debounce után **egyszeri**
  figyelmeztetés + `diag.log`, **failsafe NÉLKÜL** — a rendszer fut tovább. A latch
  (`fanNoacWarned`) megakadályozza az ismételt naplózást, az eltérés megszűntével
  (vagy relé-parancsnál) újraélesedik.

A teljes funkció a program elején, a `DEBUG`/`OTA_DEBUG`/`BOOT_DIAG` kapcsolók
mellett a **`FAN_SENSE_ENABLE`** makróval ki/be kapcsolható (`1` = be, `0` = ki).
Kikapcsolva a hozzá tartozó kód **bele sem fordul**, és a GPIO6/7/20 lábak
szabadon maradnak. Finomhangolás a PINS szekció után: `FAN_SENSE_ACTIVE_LOW`
(polaritás), `FAN_SENSE_FAILSAFE_ON_STUCK` (STUCK → failsafe),
`FAN_SENSE_WARN_ON_NOAC` (NOAC → figyelmeztetés), valamint az időzítő konstansok.

---

## Működés

### Üzemmódok

- **STATE_NORMAL** – normál működés: BLE vagy kézi vezérlés, görgő + fokozatok.
- **STATE_FAILSAFE** – biztonsági állapot, ha egyszerre **2 vagy több** ventilátor-relé
  aktív (hibás állapot). Ekkor minden relé lekapcsol és mindkét LED villog.

### Gombvezérlés

A `OneButton` könyvtár kezeli a gombot:

| Gesztus | Hatás |
|---|---|
| **Egyszeres kattintás** | Görgő be (relék + roller). Ha a görgő már jár és a fokozat 0, akkor görgő ki + relék ki. |
| **Dupla kattintás** | **Kézi mód** be/léptetés. Belépéskor leállítja a BLE-t, és fokozatot léptet: 1 → 2 → 3 → 0 → 1 … |
| **Több (>2) kattintás** | Vissza **automata módba**: ventilátor ki, BLE advertising újraindul. |
| **Hosszú nyomás (elengedésre)** | **Deep sleep** (`src=button-longpress`). |

### BLE vezérlés és parancsok

Az eszköz neve advertisingban: **`FanController`**.

Két BLE szolgáltatás fut:

- **Ventilátor-vezérlés** – Service `0000ffe0-…`, karakterisztika `0000ffe1-…`
- **OTA** – Service `fb1e4001-…` (RX `…4002`, TX `…4003`)

A vezérlő parancsokat a `ffe1` karakterisztikára kell írni (UTF-8 szöveg):

| Parancs | Leírás | Válasz |
|---|---|---|
| `AUTH:<pin>` | Hitelesítés (alapért. PIN: `123456`) | `AUTH_OK` / `AUTH_FAIL` / `AUTH_LOCKED` |
| `LEVEL:<0-3>` | Fokozat: 0 = ki, 1 = 33%, 2 = 66%, 3 = 100% | — (hiba esetén `AUTH_REQUIRED`) |
| `ROLLER:<0/1>` | Rendszer/görgő ki (0) / be (1) | — |
| `DIAG?` | Diagnosztikai napló lekérése | `0x02"DIAG_BEGIN"` … `0x04"DIAG_END"` |
| `DIAGCLR` | Diagnosztikai napló törlése | `DIAG_CLEARED` |

**Hitelesítés:** minden vezérlő parancs (LEVEL/ROLLER/DIAG) **AUTH-ot igényel**.
Sikertelen kísérletek: **5 próbálkozás után 60 mp-es zárolás** (`AUTH_LOCKED`).

### Relé-kapcsolás (break-before-make)

Fokozatváltáskor előbb **lekapcsol minden ventilátor-relé**, majd
`RELAY_SWITCH_DELAY_MS` (**10 ms**) szünet után kapcsol be az új fokozat.
Így nincs átfedés („make-before-break"), és a táp-tranziens rövid.

> Megjegyzés: a tényleges break-idő ~20 ms, mert a `handleZoneChange()` csak a
> 20 ms-os `checkInterval` ütemében fut. A 230V AC ventilátor okozta BROWNOUT
> teljes megszüntetése csak **hardveres RC-snubber + MOV varisztor** és nagyobb
> szűrőkondenzátor mellett lehetséges; a firmware csak enyhíti.

### Failsafe (biztonsági) állapot

Ha a `normalMode()` azt észleli, hogy **2 vagy több** ventilátor-relé egyszerre
aktív, azonnal `STATE_FAILSAFE`-be vált:

- minden relé lekapcsol (`RELAY_EN` LOW),
- mindkét LED ~2 Hz-cel villog,
- **60 mp** után az eszköz deep sleepbe megy (`src=failsafe-timeout`).

### Deep sleep (energiatakarékos alvás)

Az eszköz deep sleepbe lép:

- **gombos hosszú nyomásra** (`button-longpress`),
- **1 óra tétlenség** után (`idle-timeout`, csak ha nincs BLE és nincs kézi mód),
- **failsafe** 60 mp után (`failsafe-timeout`).

Ébresztés: a **gomb** (GPIO low) megnyomásával.

A `enterDeepSleep()` alvás előtt gondosan „lecsöndesíti" a rendszert, hogy
elkerülje az INT_WDT(5) watchdog-resetet ébredéskor:

1. BLE lecsatlakozás → **500 ms**
2. advertising stop → **300 ms**
3. relék ki → **200 ms** (GPIO settle)
4. LED-ek ki → **200 ms** (GPIO settle)
5. **interrupt cleanup**: `portDISABLE_INTERRUPTS()` +
   `esp_intr_disable_source(ETS_GPIO_INUM)` + `portENABLE_INTERRUPTS()` → **100 ms**
6. korábbi wakeup-források törlése (`esp_sleep_disable_wakeup_source(ALL)`),
   majd csak a gomb GPIO wakeup beállítása
7. **500 ms** végső stabilizáció → `esp_deep_sleep_start()`

---

## Fokozat-mentés és visszaállítás (RTC + NVS hibrid)

A ventilátor-fokozatot **két** mechanizmus tárolja, egymást kiegészítve:

### RTC_NOINIT (gyors, resetet túlél)

- **Mikor:** azonnal, a `handleZoneChange()`-ben, **még a relé fizikai
  kapcsolása ELŐTT** (ugyanabban a critical szekcióban).
- **Mit:** `savedZone` + `savedZoneMagic` (0xFA11A5EE magic védi az érvényességet).
- **Miért:** BROWNOUT / reset után **azonnal**, biztonságosan visszaáll.
- **Korlát:** **teljes áramtalanításkor elveszik** (az RTC RAM tápigényes).

### NVS (flash, áramtalanítást is túlél)

- **Mikor:** csak ha egy fokozat **30 mp-ig stabil** maradt
  (`NVS_SAVE_STABLE_MS`), a `loop()`-ból hívott `saveZoneToNvsIfStable()`-ben,
  és **csak ha nem fut OTA**.
- **Miért késleltetve:** hogy **ne írjunk flasht** a brownout-veszélyes kapcsolási
  pillanatban (flash-korrupció elkerülése), és kíméljük a flash-kopást.
- **Cache:** `nvsLastSavedZone` megakadályozza a fölösleges újraírást ugyanazzal
  az értékkel.
- **Korlát:** lassabb írás (~10–50 ms, blokkoló), ezért nem a kapcsolás előtt fut.

### Visszaállítás boot-kor

A fokozat (és a görgő) **csak hibás reset után** áll vissza automatikusan:

```
ESP_RST_BROWNOUT | ESP_RST_UNKNOWN | ESP_RST_INT_WDT | ESP_RST_TASK_WDT | ESP_RST_WDT
```

Prioritás: **RTC → NVS fallback**:

1. ha az RTC magic érvényes és `savedZone` 1–3 → onnan,
2. különben ha az NVS-ben van érvényes érték → onnan,
3. különben nincs visszaállítás (fokozat 0).

> **Szándékos** deep sleep utáni ébredésnél (reset ok = `DEEPSLEEP`) **nincs**
> auto-visszaállítás — tiszta lappal indul, ami a kívánt viselkedés.

---

## Diagnosztikai napló

A firmware egy kis (max **512 byte**) naplót vezet a SPIFFS-en
(`/diag.log`), ami BLE-n a `DIAG?` paranccsal lekérdezhető. Csak akkor ír bele,
ha **tényleg történt valami**:

```
[boot]   reason=BROWNOUT(11) heap=... min=...   -> hibás reset (pl. brownout)
[lowmem] heap=... min=... t=...s                -> kevés szabad memória (<20 kB)
[sleep]  src=button-longpress                    -> honnan indult a deep sleep
[ota]    bad magic=0x.. size=...                 -> rossz/sérült firmware fájl
```

A napló MTU-biztos 20 byte-os darabokban streamel (`DIAG_BEGIN` … `DIAG_END`),
így alapértelmezett BLE MTU mellett is sértetlen.

---

## OTA firmware-frissítés

A frissítés a dedikált OTA BLE szolgáltatáson keresztül történik. Védelem:

- **Magic-byte ellenőrzés** (`[FIX-ESP-16]`): az `Update.begin()` előtt ellenőrzi,
  hogy a feltöltött bináris első byte-ja **0xE9** (érvényes ESP32 app image).
  Ha nem, **érthető hibát** ad a félrevezető „Decryption error" helyett, és a
  diag naplóba is bekerül (`[ota] bad magic=0x..`).

> **Tipp:** mindig az alkalmazás `*.ino.bin` fájlját töltsd fel — ne a
> `*.merged.bin`, `*.partitions.bin` vagy gzip-elt (`0x1F`) fájlt. Ellenőrzéshez:
> `python3 ota_diagnostic.py firmware.bin`.

---

## Particionálás

`partitions_custom.csv` (4 MB flash, dual-OTA):

| Név | Típus | Altípus | Offset | Méret |
|---|---|---|---|---|
| `nvs` | data | nvs | 0x9000 | 0x5000 (20 kB) |
| `otadata` | data | ota | 0xE000 | 0x2000 |
| `app0` | app | ota_0 | 0x10000 | 0x150000 (1,3 MB) |
| `app1` | app | ota_1 | 0x160000 | 0x150000 (1,3 MB) |
| `spiffs` | data | spiffs | 0x2B0000 | 0x150000 (1,3 MB) |

Az `nvs` partíció tárolja a fokozatot (`fan/zone`), a `spiffs` a diag naplót.

---

## Diagnosztikai eszközök (Python)

A repóban három Python eszköz található (részletek: `TOOLS_README.md`).
Telepítés: `pip install bleak`.

| Eszköz | Funkció |
|---|---|
| `diag_client.py` | A `/diag.log` lekérése BLE-n (reset okok, lowmem, sleep). |
| `fan_stress.py` | Fokozat-edzés / stressz-teszt a BROWNOUT reprodukálásához. |
| `ota_diagnostic.py` | A firmware `.bin` magic-byte és partíció-ellenőrzése. |

Példák:

```bash
python3 diag_client.py --pin 123456            # napló lekérése
python3 diag_client.py --clear                 # lekérés + törlés
python3 fan_stress.py --duration 3600 --check-interval 60 --log stress.log
python3 ota_diagnostic.py FanController_OTA_debug.ino.bin
```

---

## Időzítések összefoglaló

| Paraméter | Érték | Szerep |
|---|---|---|
| `checkInterval` | 20 ms | állapotgép ütem |
| `RELAY_SWITCH_DELAY_MS` | 10 ms | break-before-make szünet |
| `NVS_SAVE_STABLE_MS` | 30 000 ms | NVS-írás előtti stabilitási idő |
| `INACTIVITY_MS` | 3 600 000 ms (1 óra) | tétlenségi deep sleep |
| `BLE_ZONE_TIMEOUT_MS` | 720 000 ms (12 perc) | BLE-kapcsolat nélküli fokozat-időkorlát |
| Failsafe timeout | 60 000 ms | failsafe → deep sleep |
| `MAX_AUTH_ATTEMPTS` / lockout | 5 / 60 000 ms | BLE auth védelem |
| `LOW_HEAP_THRESHOLD` | 20 000 byte | lowmem napló küszöb |

---

## Verziótörténet (kivonat)

| Verzió | Változás |
|---|---|
| **7.8.0** | Fan relé KIMENET figyelése 3× H11AA1M optóval (GPIO6/7/20), AC-tudatos idő-ablakos detektálással; tartós eltérésnél failsafe + `diag.log`. |
| 7.7.3 | Boot-diagnosztika a soros monitorra (RTC/NVS/`diag.log`). |
| 7.7.2 | NVS force-mentés 5 percenként sűrű váltogatásnál is. |
| 7.7.1 | Boot NVS olvasás default −1 (a fallback javítása). |
| **7.7.0** | Interrupt cleanup visszatéve a deep sleep elé (INT_WDT ellen). |
| 7.6.9 | Hosszabb türelmi szünetek az `enterDeepSleep()`-ben (INT_WDT ellen). |
| 7.6.8 | WDT resetek (INT/TASK/WDT) is a görgő + fokozat visszaállításban. |
| 7.6.7 | Hibrid fokozat-mentés: RTC (resetre) + NVS (áramtalanításra). |
| 7.6.6 | BROWNOUT/UNKNOWN reset után görgő + fokozat auto-visszaállítás. |
| 7.6.5 | RELAY_SWITCH_DELAY_MS → 10 ms (rövidebb táp-tranziens). |
| 7.6.4 | OTA magic-byte ellenőrzés a félrevezető „Decryption error" helyett. |
| 7.6.3 | `enterDeepSleep()` forrásának naplózása (`[sleep] src=…`). |
| 7.6.0 | SPIFFS diag napló, BLE-n lekérdezhető (reset ok + lowmem). |
| 7.5.0 | BROWNOUT reset után deep sleep helyett újraindulás. |

---

## Licenc

Lásd a [`LICENSE`](LICENSE) fájlt.
