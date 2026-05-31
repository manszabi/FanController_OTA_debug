#!/usr/bin/env python3
"""
OTA diagnostic utility for FanController.
Analyzes firmware.bin files and checks compatibility with partition tables.
"""

import sys
import struct
import os

def analyze_firmware(bin_path):
    """Analyze firmware.bin file header and basic info."""
    if not os.path.exists(bin_path):
        print(f"Error: {bin_path} not found")
        return False

    try:
        size = os.path.getsize(bin_path)
        print(f"Firmware file: {bin_path}")
        print(f"File size: {size} bytes (0x{size:X})")

        # Read magic number (first 4 bytes should be 0xE9 for ESP32)
        with open(bin_path, 'rb') as f:
            magic = f.read(1)

        if magic[0] == 0xE9:
            print("✓ Valid ESP32 firmware signature (0xE9)")
        else:
            print(f"✗ Invalid magic byte: 0x{magic[0]:02X} (expected 0xE9)")
            return False

        return True
    except Exception as e:
        print(f"Error analyzing firmware: {e}")
        return False

def check_partition_table():
    """Check partition table against app partition size."""
    PARTITION_FILE = "/home/user/FanController_OTA_debug/partitions_custom.csv"

    if not os.path.exists(PARTITION_FILE):
        print(f"Partition file not found: {PARTITION_FILE}")
        return

    print("\n=== Partition Table Analysis ===")
    with open(PARTITION_FILE, 'r') as f:
        for line in f:
            line = line.strip()
            if line.startswith('#') or not line:
                continue
            parts = [p.strip() for p in line.split(',')]
            if len(parts) >= 4:
                name, typ, subtype, offset, size = parts[0], parts[1], parts[2], parts[3], parts[4]
                if typ == 'app':
                    offset_dec = int(offset, 16)
                    size_dec = int(size, 16)
                    print(f"{name:8} @ 0x{offset} (0x{size} = {size_dec:,} bytes)")

def main():
    print("=== FanController OTA Diagnostic ===\n")

    if len(sys.argv) > 1:
        firmware_path = sys.argv[1]
    else:
        # Try to find firmware in Arduino build directories
        print("Usage: python3 ota_diagnostic.py <firmware.bin>")
        print("\nOr provide the full path to the compiled firmware.bin from:")
        print("  ~/.arduino15/packages/esp32/hardware/esp32/*/tools/...")
        print("  or your Arduino project build directory")
        return

    analyze_firmware(firmware_path)
    check_partition_table()

if __name__ == "__main__":
    main()
