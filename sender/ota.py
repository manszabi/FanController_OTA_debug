"""
  
MIT License

Copyright (c) 2021 Felix Biego

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

-------------------------------------------------------------------------------
Módosítások / Changelog
-------------------------------------------------------------------------------
2026-05-24  [FIX-1]  Globális 'end' flag átnevezve 'ota_done'-ra, mert az 'end'
                     névütközést okozott a send_part() belső 'end' lokális
                     változójával, ami kiszámíthatatlan viselkedést eredményezett.
2026-05-24  [FIX-2]  send_part() lokális változó átnevezve 'end'-ről 'end_pos'-ra
                     a névütközés másik oldalának feloldásához.
2026-05-24  [FIX-3]  'while end:' feltétel kijavítva 'while not ota_done:'-ra.
                     Az eredeti logika invertált volt: True→False helyett
                     most False→True jelzi a befejezést.
2026-05-24  [FIX-4]  handle_disconnect()-ből eltávolítva a 'global disconnect'
                     és 'disconnect = False' sorok. A 'disconnect' változó sosem
                     volt definiálva globálisan, és sehol sem olvasták.
2026-05-24  [FIX-5]  0xF2 ágban az 'ins = ...' értékadás helyett tényleges
                     print() hívás, a kikommentezett sor visszaállítva.
2026-05-24  [FIX-6]  get_bytes_from_file(): open() helyett 'with open() as f:'
                     használata, hogy a fájl biztosan lezáródjon olvasás után.
2026-05-24  [FIX-7]  send_part() belső ciklus átírva bytearray slice-olással
                     az indexelt loop helyett; egész osztás (//) következetesen
                     alkalmazva a float osztás (/) helyett.
2026-05-24  [FIX-8]  'total = fileParts' áthelyezve az adatküldés elé, így
                     handle_rx() nem hivatkozhat fileParts-ra annak inicializálása
                     előtt (race condition megszüntetése).
2026-05-24  [FIX-8b] Fájl beolvasása és az összes változó (fileBytes, clt,
                     fileParts, total) áthelyezve a start_notify() elé.
                     Az eszköz azonnal küldhet 0xAA jelet csatlakozáskor —
                     ha total=0 volt, ZeroDivisionError keletkezett a
                     progress barban (division by zero).
2026-05-24  [FIX-9]  isValidAddress(): regex egyszerűsítve, match() használata
                     search() helyett, felesleges len() ellenőrzések eltávolítva
                     (a regex maga már kikényszeríti a hosszt).
2026-05-24  [FIX-10] 'if address == None:' kijavítva 'if address is None:'-ra
                     (Python best practice szingleton összehasonlításhoz).
2026-05-24  [FIX-11] Indítás előtt BLE scan: megkeresi az összes látható eszközt
                     és kiírja a nevüket/címüket, hogy ellenőrizhető legyen,
                     látja-e egyáltalán a céleszközt.
2026-05-24  [FIX-12] start_notify() után a várakozás 1.0s-ről 2.0s-re növelve,
                     hogy az eszköznek legyen ideje felkészülni.
2026-05-24  [FIX-13] 0xFD "start" jel response=True-val küldve (volt: False),
                     hogy a host megvárja az eszköz nyugtázását.
2026-05-24  [FIX-14] handle_rx()-be debug print hozzáadva: minden bejövő BLE
                     csomag hex dump-ja megjelenik, így látható, válaszol-e
                     egyáltalán az eszköz.
2026-05-24  [FIX-15] MTU csökkentve 500-ról 182-re. Windows BLE stack nem
                     támogatja a nagy csomagokat explicit MTU negotiation
                     nélkül — 500-as értékkel OSError -2147024809
                     ("A paraméter nem megfelelő") keletkezett íráskor.
2026-05-24  [FIX-16] MTU tovább csökkentve 182-ről 100-ra. Az ESP oldalon
                     a buffer indexelése pos*otaMTU+x — ha Windows csendben
                     csonkítja a csomagot, az ESP rossz helyre ír, sérült
                     firmware keletkezik. 100 byte negotiation nélkül is
                     biztonsággal átmegy.
2026-05-24  [FIX-17] Az utolsó part elküldése után 500ms várakozás, hogy
                     az ESP-nek legyen ideje a SPIFFS write-ot befejezni
                     mielőtt 0xF2-vel mode-ot váltana INSTALL_MODE-ra.
                     Race condition védelem az ESP oldali FIX-ESP-5
                     kiegészítéseként.
2026-05-24  [FIX-18] DEBUG flag bevezetve. Production módban (DEBUG=False)
                     elrejti a "RX:" csomag hex dump-okat és a BLE scan
                     kimenetét. Hibakereséshez DEBUG=True a fájl tetején.
2026-06-10  [FIX-19] Per-part CRC32 (firmware [FIX-ESP-34], v7.9.0). A 0xFC
                     part-vége csomag mostantól 4 byte zlib-CRC32-t hordoz a
                     part hasznos adatára (big-endian): a fogadó a SPIFFS-írás
                     előtt ellenőrzi, és eltérésnél ugyanazt a partot kéri újra
                     (0xF1) — ezt a send_part() a meglévő 0xF1-kiszolgálással
                     automatikusan újraküldi. NINCS visszafelé kompatibilitás:
                     a v7.9.0 firmware az 5 byte-os (CRC nélküli) 0xFC-t eldobja.
2026-06-14  [FIX-20] Hibás OTA után nincs többé "Waiting for disconnect" beragadás.
                     A firmware MINDIG 0x0F-fel jelez (siker: "OTA done" + reboot;
                     hiba: ERR/FAILED/No space, reboot NÉLKÜL). A küldő mostantól
                     csak SIKER esetén vár a disconnectre (20s időkorláttal), hiba
                     esetén azonnal kilép, és az 'async with' bontja a kapcsolatot.
-------------------------------------------------------------------------------
"""

from __future__ import print_function
import os.path
from os import path
import asyncio
import platform
import math
import sys
import re
import zlib  # [FIX-19] per-part CRC32 (zlib.crc32, egyezik a firmware crc32_zlib-jével)

from bleak import BleakClient, BleakScanner
from bleak.exc import BleakError

header = """#####################################################################
    ------------------------BLE OTA update---------------------
    Arduino code @ https://github.com/fbiego/ESP32_BLE_OTA_Arduino
#####################################################################"""

UART_SERVICE_UUID = "fb1e4001-54ae-4a28-9f74-dfccb248601d"
UART_RX_CHAR_UUID = "fb1e4002-54ae-4a28-9f74-dfccb248601d"
UART_TX_CHAR_UUID = "fb1e4003-54ae-4a28-9f74-dfccb248601d"

PART = 16000
# [FIX-15] 2026-05-24: MTU csökkentve 500-ról 182-re. Windows BLE stack
#           alapból ~20 byte-ra limitál explicit MTU negotiation nélkül,
#           és a Bleak Windows backenden nincs közvetlen MTU lekérés.
#           182 = 185 byte (Windows BLE safe max) - 3 byte BLE header.
#           500-as értékkel OSError: [WinError -2147024809] keletkezett.
# [FIX-16] 2026-05-24: MTU tovább csökkentve 182-ről 100-ra. Az ESP oldalon
#           az otaBuf buffer indexelése: pos * otaMTU + x — ha a Windows BLE
#           stack csendben csonkítja a csomagot, az ESP rossz pozícióra írja
#           az adatot, ami sérült firmware-t eredményez. 100 byte biztonsággal
#           átmegy minden Windows BLE implementációban negotiation nélkül is.
MTU = 100

# [FIX-18] 2026-05-24: DEBUG flag — production módban False, hibakeresési
#           módban True. Vezérli a "RX:" csomag hex dump-okat, a BLE scan
#           kimenetét és egyéb hibakereséshez hasznos kiírásokat.
DEBUG = False

# [FIX-1] 2026-05-24: Átnevezve 'end'-ről 'ota_done'-ra a send_part()-beli
#          lokális 'end' változóval való névütközés elkerülése végett.
# [FIX-3] 2026-05-24: Kezdőérték False (volt: True); a logika is meg volt
#          fordítva — most True jelenti a befejezést, nem a False.
ota_done = False
ota_success = False  # [FIX-20] a 0x0F eredmény "OTA done"-t jelzett-e (siker)
clt = None
fileBytes = None
total = 0


def get_bytes_from_file(filename):
    print("Reading from: ", filename)
    # [FIX-6] 2026-05-24: 'with' blokk használata, hogy a fájl
    #          biztosan lezáródjon olvasás után (volt: open().read() lezárás nélkül).
    with open(filename, "rb") as f:
        return f.read()


# [FIX-11] 2026-05-24: Új segédfüggvény — megkeresi az összes látható BLE eszközt
#           és kiírja a nevüket/címüket. Ha a céleszköz nem szerepel a listában,
#           a csatlakozás biztosan meghiúsul.
async def scan_devices():
    print("Scanning for BLE devices (10s)...")
    devices = await BleakScanner.discover(timeout=10.0)
    if not devices:
        print("No BLE devices found.")
    else:
        print(f"Found {len(devices)} device(s):")
        for d in devices:
            print(f"  {d.address}  {d.name or '(no name)'}")
    print()


async def start_ota(ble_address: str, file_name: str):
    device = await BleakScanner.find_device_by_address(ble_address, timeout=20.0)
    disconnected_event = asyncio.Event()

    # [FIX-4] 2026-05-24: Eltávolítva a 'global disconnect' és 'disconnect = False'
    #          sorok. A 'disconnect' változó sosem volt definiálva globálisan,
    #          és sehol sem olvasták — a disconnected_event.set() elegendő.
    def handle_disconnect(_: BleakClient):
        print(": Device disconnected")
        disconnected_event.set()

    async def handle_rx(_: int, data: bytearray):
        # [FIX-14] 2026-05-24: Debug print hozzáadva — minden bejövő csomag
        #           hex dump-ja megjelenik, így látható, válaszol-e az eszköz.
        # [FIX-18] 2026-05-24: DEBUG flag mögé téve — production módban
        #           (DEBUG=False) elrejtve a "RX:" spam, csak hibakereséskor
        #           kell.
        if DEBUG:
            print(f"RX: {data.hex()}")

        if data[0] == 0xAA:
            print("Transfer mode:", data[1])
            printProgressBar(0, total, prefix='Progress:', suffix='Complete', length=50)
            if data[1] == 1:
                for x in range(0, fileParts):
                    await send_part(x, fileBytes, clt)
                    printProgressBar(x + 1, total, prefix='Progress:', suffix='Complete', length=50)
            else:
                await send_part(0, fileBytes, clt)

        if data[0] == 0xF1:
            nxt = int.from_bytes(bytearray([data[1], data[2]]), "big")
            await send_part(nxt, fileBytes, clt)
            printProgressBar(nxt + 1, total, prefix='Progress:', suffix='Complete', length=50)
            # [FIX-17] 2026-05-24: Az utolsó part elküldése után várunk 500ms-t,
            #           hogy az ESP-nek legyen ideje a SPIFFS write-ot befejezni,
            #           mielőtt az 0xF2 jelet várnánk vissza. Enélkül race condition
            #           keletkezhet: az ESP `otaWriteFile`-t false-ra állítja a 41.
            #           part után, és mire a 42. part megérkezik, már mode-ot
            #           váltott INSTALL_MODE-ra anélkül, hogy a 42. partot kiírta volna.
            #           Ez a védelem kiegészíti az ESP oldali FIX-ESP-5 javítást.
            if nxt + 1 == total:
                await asyncio.sleep(0.5)

        if data[0] == 0xF2:
            # [FIX-5] 2026-05-24: Kikommentezett print() visszaállítva; az eredeti
            #          kód 'ins = "Installing firmware"' értékadást tartalmazott,
            #          amit soha nem használt fel.
            print("Installing firmware")

        if data[0] == 0x0F:
            result = bytearray(data[1:])
            msg = str(result, 'utf-8')
            print("OTA result: ", msg)
            # [FIX-1] 2026-05-24: Átnevezett flag beállítása True-ra a befejezés jelzésére.
            # [FIX-20] 2026-06-14: siker vs hiba megkülönböztetése. A firmware MINDIG
            #          0x0F-fel jelez: sikernél a szöveg tartalmazza az "OTA done"-t és
            #          ezután ÚJRAINDUL (a kapcsolat megszakad); hibánál (ERR/FAILED/
            #          No space) NEM indul újra. Enélkül a küldő a "Waiting for
            #          disconnect"-nél örökre várt egy hibás OTA után.
            global ota_done, ota_success
            ota_success = ("OTA done" in msg) and ("FAILED" not in msg) and ("Not finished" not in msg)
            ota_done = True

    def printProgressBar(iteration, total, prefix='', suffix='', decimals=1, length=100, fill='█', printEnd="\r"):
        """
        Call in a loop to create terminal progress bar
        @params:
            iteration   - Required  : current iteration (Int)
            total       - Required  : total iterations (Int)
            prefix      - Optional  : prefix string (Str)
            suffix      - Optional  : suffix string (Str)
            decimals    - Optional  : positive number of decimals in percent complete (Int)
            length      - Optional  : character length of bar (Int)
            fill        - Optional  : bar fill character (Str)
            printEnd    - Optional  : end character (e.g. "\r", "\r\n") (Str)
        """
        percent = ("{0:." + str(decimals) + "f}").format(100 * (iteration / float(total)))
        filledLength = int(length * iteration // total)
        bar = fill * filledLength + '-' * (length - filledLength)
        print(f'\r{prefix} |{bar}| {percent}% {suffix}', end=printEnd)
        if iteration == total:
            print()

    async def send_part(position: int, data: bytearray, client: BleakClient):
        start = position * PART
        # [FIX-2] 2026-05-24: Átnevezve 'end'-ről 'end_pos'-ra, hogy ne takarja
        #          el a globális 'ota_done' (korábban 'end') változót.
        end_pos = (position + 1) * PART
        if len(data) < end_pos:
            end_pos = len(data)
        chunk_size = end_pos - start
        # [FIX-7] 2026-05-24: Egész osztás (//) alkalmazva következetesen
        #          a korábbi float osztás (/) helyett; belső ciklus átírva
        #          bytearray slice-olással az indexelt loop helyett.
        full_chunks = chunk_size // MTU
        for i in range(full_chunks):
            toSend = bytearray([0xFB, i])
            toSend += data[(position * PART) + (MTU * i):(position * PART) + (MTU * i) + MTU]
            await send_data(client, toSend, False)
        remainder = chunk_size % MTU
        if remainder != 0:
            toSend = bytearray([0xFB, full_chunks])
            toSend += data[(position * PART) + (MTU * full_chunks):(position * PART) + (MTU * full_chunks) + remainder]
            await send_data(client, toSend, False)
        # [FIX-19] CRC32 a part hasznos adatára (a firmware crc32_zlib()-jével
        # egyező zlib.crc32). 4 byte big-endian a 0xFC végén → a fogadó a
        # SPIFFS-írás előtt ellenőrzi, hibánál ugyanezt a partot kéri újra.
        part_data = data[start:end_pos]
        crc = zlib.crc32(part_data) & 0xFFFFFFFF
        update = bytearray([
            0xFC,
            (chunk_size >> 8) & 0xFF,
            chunk_size & 0xFF,
            (position >> 8) & 0xFF,
            position & 0xFF,
            (crc >> 24) & 0xFF,
            (crc >> 16) & 0xFF,
            (crc >> 8) & 0xFF,
            crc & 0xFF,
        ])
        await send_data(client, update, True)

    async def send_data(client: BleakClient, data: bytearray, response: bool):
        await client.write_gatt_char(UART_RX_CHAR_UUID, data, response)

    if not device:
        print("-----------Failed--------------")
        print(f"Device with address {ble_address} could not be found.")
        return

    async with BleakClient(device, disconnected_callback=handle_disconnect) as client:
        # [FIX-8b] 2026-05-24: Fájl beolvasása és az összes változó (fileBytes, clt,
        #           fileParts, total) beállítása a start_notify() ELŐTT történik.
        #           Az eszköz azonnal küldhet 0xAA jelet a csatlakozás után —
        #           ha total=0, ZeroDivisionError keletkezik a progress barban.
        global fileBytes
        fileBytes = get_bytes_from_file(file_name)
        global clt
        clt = client
        fileParts = math.ceil(len(fileBytes) / PART)
        fileLen = len(fileBytes)
        global total
        total = fileParts

        await client.start_notify(UART_TX_CHAR_UUID, handle_rx)

        # [FIX-12] 2026-05-24: Várakozás 1.0s-ről 2.0s-re növelve, hogy az eszköznek
        #           legyen ideje felkészülni a notify feliratkozás után.
        await asyncio.sleep(2.0)

        # [FIX-13] 2026-05-24: 0xFD "start" jel response=True-val küldve (volt: False),
        #           hogy a host megvárja az eszköz nyugtázását.
        await send_data(client, bytearray([0xFD]), True)

        fileSize = bytearray([
            0xFE,
            (fileLen >> 24) & 0xFF,
            (fileLen >> 16) & 0xFF,
            (fileLen >> 8) & 0xFF,
            fileLen & 0xFF,
        ])
        await send_data(client, fileSize, False)
        otaInfo = bytearray([0xFF, fileParts >> 8, fileParts & 0xFF, MTU >> 8, MTU & 0xFF])
        await send_data(client, otaInfo, False)

        # [FIX-3] 2026-05-24: Feltétel kijavítva 'while not ota_done:'-ra.
        #          Az eredeti 'while end:' invertált logikát használt.
        while not ota_done:
            await asyncio.sleep(1.0)

        # [FIX-20] 2026-06-14: sikernél a firmware ~5s múlva újraindul → megvárjuk a
        #          disconnectet (időkorláttal, hogy egy elmaradt disconnect-event se
        #          akasszon be). Hibánál a firmware NEM indul újra → nem várunk
        #          disconnectre, azonnal kilépünk; az 'async with' bontja a kapcsolatot.
        if ota_success:
            print("Waiting for reboot/disconnect... ", end="", flush=True)
            try:
                await asyncio.wait_for(disconnected_event.wait(), timeout=20.0)
                print("\n-----------Complete--------------")
            except asyncio.TimeoutError:
                print("\n(timeout: nincs disconnect 20s alatt — ellenőrizd az eszközt)")
        else:
            print("-----------OTA FAILED — lásd a fenti 'OTA result'-ot--------------")


def isValidAddress(address):
    # [FIX-9] 2026-05-24: Regex egyszerűsítve és match() alkalmazva search() helyett;
    #          a felesleges len() ellenőrzések eltávolítva (a regex maga kikényszeríti
    #          a helyes hosszt). Az eredeti regex a {17} részt hibásan a UUID ágra
    #          alkalmazta, ami helytelen illesztést okozhatott.
    # MAC cím: XX:XX:XX:XX:XX:XX vagy XX-XX-XX-XX-XX-XX
    mac_regex = re.compile(r"^([0-9A-Fa-f]{2}[:\-]){5}[0-9A-Fa-f]{2}$")
    # UUID (macOS BLE): xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
    uuid_regex = re.compile(r"^[{]?[0-9a-fA-F]{8}-([0-9a-fA-F]{4}-){3}[0-9a-fA-F]{12}[}]?$")

    # [FIX-10] 2026-05-24: '== None' kijavítva 'is None'-ra
    #           (Python best practice szingleton összehasonlításhoz).
    if address is None:
        return False

    return bool(mac_regex.match(address)) or bool(uuid_regex.match(address))


if __name__ == "__main__":
    print(header)
    # [FIX-21] 2026-06-14: a program a végén NEM lép ki magától — az ablak nyitva
    #          marad, hogy az eredmény (siker/hiba) olvasható legyen. A BLE-kapcsolatot
    #          ekkorra már az 'async with' lebontotta; itt csak ENTER-re várunk.
    #          A 'finally' miatt akkor is megáll, ha kivétel (pl. BleakError) történt.
    try:
        if len(sys.argv) > 2:
            print("Trying to start OTA update")
            if isValidAddress(sys.argv[1]) and path.exists(sys.argv[2]):
                # [FIX-11] 2026-05-24: Scan futtatása az OTA előtt, hogy látható legyen
                #           az eszköz egyáltalán megtalálható-e BLE-n keresztül.
                # [FIX-18] 2026-05-24: Scan csak DEBUG módban fut — production-ben
                #           lassítja az OTA-t és nem szükséges.
                if DEBUG:
                    asyncio.run(scan_devices())
                asyncio.run(start_ota(sys.argv[1], sys.argv[2]))
            else:
                if not isValidAddress(sys.argv[1]):
                    print("Invalid Address: ", sys.argv[1])
                if not path.exists(sys.argv[2]):
                    print("File not found: ", sys.argv[2])
        else:
            print("Specify the device address and firmware file")
            print(">python ota.py \"01:23:45:67:89:ab\" \"firmware.bin\"")
    finally:
        try:
            input("\nNyomj ENTER-t az ablak bezárásához...")
        except (EOFError, KeyboardInterrupt):
            pass