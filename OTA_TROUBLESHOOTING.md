# OTA Decryption Error - Troubleshooting Guide

## Issue
```
OTA result: Update.end FAILED: Decryption error
```

The firmware transfers 100% via BLE but fails during `Update.end()` validation.

## Root Causes & Solutions

### 1. Partition Table Mismatch (Most Likely)
If the device was previously flashed with a different partition table, the device partition may not match `partitions_custom.csv`.

**Check:**
```bash
# Determine the COM port of the ESP32
# Then use esptool to read the current partition table:
esptool.py -p /dev/ttyUSB0 read_flash 0x8000 0x1000 ptable.bin
# Or view via Arduino IDE → Tools → Partition Scheme (shows current)
```

**Solution:**
If the partition table is different, you must flash the custom partition table before OTA:
```bash
esptool.py -p /dev/ttyUSB0 write_flash 0x8000 partitions_custom.bin
```

Or via Arduino IDE:
1. Select **Tools → Partition Scheme → Custom**
2. Point to `partitions_custom.csv`
3. Upload sketch (not via OTA, via USB serial)
4. Then OTA should work

---

### 2. Firmware Size Too Large
The custom partition allocates 0x150000 (1.375 MB) for each app partition.
If firmware exceeds this, OTA fails.

**Check:**
```bash
# After Arduino compile, check binary size:
# Build output shows "Sketch uses XXX bytes of program storage"
# Should be < 1,376,256 bytes (0x150000)

# Or inspect the .elf file:
xtensa-esp32c3-elf-size -A .pio/build/esp32-c3-devkitm-1/firmware.elf
```

**If firmware is too large:**
- Enable SPIFFS compression
- Remove unused features (e.g., debug logging if OTA_DEBUG=1)
- Use LTO (Link Time Optimization)

---

### 3. Corrupted Binary During Transfer
Despite 100% progress, the binary may be corrupted in SPIFFS.

**Check:**
```
Serial output during OTA should show:
  - Update.begin OK
  - Update.writeStream returned: [size] (should match sent size)
  - EQUAL written vs updateSize before Update.end()
```

**If mismatch exists:**
- Check BLE MTU negotiation (should be default 23)
- Verify SPIFFS has ~2 MB free before OTA
- Check for file corruption: `FIX-ESP-10` should detect and cancel OTA if write fails

---

### 4. Signature Verification Enabled (Unlikely)
If the device has Secure Boot or Signature Verification enabled:

**Check:**
```
Serial output should show:
  "Secure Boot [v2]: ... [enabled]" or similar
```

**Solution:**
- Firmware must be signed with correct private key
- Or disable signature verification in Arduino IDE:
  - **Tools → Security → Enable Secure Boot: OFF**
  - **Tools → Security → Enable Flash Encryption: OFF**

---

### 5. Update.end() Validation Issue
The Update.end() call validates firmware integrity. May fail if:
- Firmware header is corrupted
- Validation CRC/checksum fails
- Device flash controller error

**Debug:**
Enable serial debugging and capture full log during OTA:
```cpp
// Already in code at line 595-596:
DBG_P("Error code: "); Serial.println(Update.getError());
DBG_P("Error string: "); Serial.println(Update.errorString());
```

---

## Diagnostic Steps (In Order)

1. **Capture full serial output** during next OTA attempt
   - Include "Running partition", "Next OTA partition", partition addresses/sizes
   - Include exact error code and error string from Update.getError()

2. **Verify partition table** on device vs `partitions_custom.csv`
   - If different → flash custom table via USB + Upload sketch

3. **Check firmware size** from Arduino build output
   - Should be < 1,376,256 bytes

4. **Check SPIFFS free space** before OTA
   - Run `diag_client.py` to see heap/memory
   - Firmware size < available free space in SPIFFS

5. **Rebuild firmware** with clean Arduino IDE build cache
   - Arduino IDE → Sketch → Verify (compiles)
   - Check .elf file size via xtensa-esp32c3-elf-size

6. **If none of above work:** Flash via USB-Serial OTA to rule out BLE issues
   - Use Arduino IDE → Upload (serial port)
   - Then verify OTA works for next update

---

## Serial Output to Capture

Run device with serial monitor enabled (115200 baud), trigger OTA, and capture:

```
Running partition:
  addr=0x... size=... label=...
Next OTA partition:
  addr=0x... size=... label=...
updateSize = [N]
Calling Update.begin()...
Update.begin OK
Calling Update.writeStream...
Update.writeStream returned: [N]
Calling Update.end()...
Update.end() returned: false
Update.end FAILED
Error code: [code]
Error string: [string]
```

This will reveal which step is failing and the exact error.
