# BLE OTA küldő (CRC-s, firmware ≥ 7.9.0)

Ez a `ota.py` a [BLE_OTA_Python-main](https://github.com/manszabi/BLE_OTA_Python-main)
küldő **CRC-s változata**, ami együttműködik a `FanController_OTA_debug` **v7.9.0+**
firmware-rel (`[FIX-ESP-34]` per-part CRC32 + újraküldés).

> A küldő repó nincs ennek a sessionnek a hatókörében, ezért a módosított fájl
> ide került. **Másold át** a `ota.py`-t a `BLE_OTA_Python-main` repóba (a régi
> `ota.py` helyére).

## Mi változott (`[FIX-19]`)

A `send_part()` a `0xFC` part-vége csomagba mostantól **4 byte CRC32-t** is tesz
(big-endian), a part hasznos adatára számolva:

```
[0xFC][len_hi][len_lo][pos_hi][pos_lo][crc24][crc16][crc8][crc0]
```

A CRC `zlib.crc32` — pontosan egyezik a firmware `crc32_zlib()`-jével
(önteszt: `crc32("123456789") == 0xCBF43926`).

Az **újraküldés** külön kód nélkül működik: ha a fogadó CRC-hibát talál, ugyanazt
a partot kéri újra `0xF1`-gyel, amit a meglévő `handle_rx` → `send_part(nxt)`
automatikusan kiszolgál. Hibás vonalon `MAX_PART_RETRY` (firmware: 5) felett az
eszköz abortál (`0x0F "ERR: CRC fail part N"`), amit a küldő kiír és kilép.

## ⚠️ Nincs visszafelé kompatibilitás

A v7.9.0 firmware az **5 byte-os** (CRC nélküli) `0xFC`-t **eldobja**. Tehát:
- a **régi** `ota.py` a v7.9.0 firmware-rel **nem** működik,
- ez az **új** `ota.py` a **régi** firmware-rel **nem** működik.

Az új firmware első feltöltése USB-n történjen.

## Használat

```bash
pip install bleak
python ota.py "01:23:45:67:89:ab" "FanController_OTA_debug.ino.bin"
```

A feltöltendő fájl a sima **`*.ino.bin`** app-image (0xE9 magic) legyen — lásd a
fő `OTA_TROUBLESHOOTING.md`-t. Hibakereséshez a fájl tetején `DEBUG = True`.
