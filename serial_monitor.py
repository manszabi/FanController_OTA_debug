#!/usr/bin/env python3
"""Simple serial port monitor for debugging."""

import serial
import serial.tools.list_ports
import sys
import threading
from datetime import datetime


class SerialMonitor:
    def __init__(self, port=None, baudrate=115200, timeout=1):
        self.port = port
        self.baudrate = baudrate
        self.timeout = timeout
        self.ser = None
        self.running = False
        self.rx_thread = None

    def list_ports(self):
        """List available serial ports."""
        ports = serial.tools.list_ports.comports()
        if not ports:
            print("Nincs elérhető soros port!")
            return []

        print("\nElérhető soros portok:")
        for i, port in enumerate(ports):
            print(f"  {i}: {port.device} - {port.description}")
        return ports

    def connect(self):
        """Connect to serial port."""
        try:
            self.ser = serial.Serial(
                port=self.port,
                baudrate=self.baudrate,
                timeout=self.timeout,
                parity=serial.PARITY_NONE,
                stopbits=serial.STOPBITS_ONE,
                bytesize=serial.EIGHTBITS
            )
            print(f"\n✓ Csatlakozva: {self.port} @ {self.baudrate} baud")
            self.running = True

            # Start receive thread
            self.rx_thread = threading.Thread(target=self.read_loop, daemon=True)
            self.rx_thread.start()
            return True
        except serial.SerialException as e:
            print(f"✗ Hiba a csatlakozásnál: {e}")
            return False

    def read_loop(self):
        """Read from serial port continuously."""
        while self.running:
            try:
                if self.ser and self.ser.in_waiting:
                    data = self.ser.read(self.ser.in_waiting)
                    timestamp = datetime.now().strftime("%H:%M:%S.%f")[:-3]

                    # Try to decode as text, fall back to hex
                    try:
                        text = data.decode('utf-8', errors='ignore')
                        print(f"[{timestamp}] RX: {text.rstrip()}")
                    except:
                        hex_str = ' '.join(f'{b:02x}' for b in data)
                        print(f"[{timestamp}] RX: {hex_str}")
            except Exception as e:
                if self.running:
                    print(f"Olvasási hiba: {e}")
                break

    def send(self, data):
        """Send data to serial port."""
        if not self.ser or not self.running:
            print("Nincs aktív kapcsolat!")
            return False

        try:
            if isinstance(data, str):
                data = data.encode('utf-8')
            self.ser.write(data)
            timestamp = datetime.now().strftime("%H:%M:%S.%f")[:-3]

            try:
                text = data.decode('utf-8', errors='ignore')
                print(f"[{timestamp}] TX: {text.rstrip()}")
            except:
                hex_str = ' '.join(f'{b:02x}' for b in data)
                print(f"[{timestamp}] TX: {hex_str}")
            return True
        except Exception as e:
            print(f"Küldési hiba: {e}")
            return False

    def close(self):
        """Close serial connection."""
        self.running = False
        if self.ser:
            self.ser.close()
            print("\nLezárva.")

    def interactive_mode(self):
        """Interactive command prompt."""
        print("\nParancsok:")
        print("  send <szöveg>  - Szöveg küldése")
        print("  hex <hex>      - Hex adat küldése (pl: FF 00 AA)")
        print("  baudrate <szám> - Baud rate módosítása")
        print("  quit           - Kilépés")
        print()

        try:
            while self.running:
                try:
                    cmd = input("> ").strip()

                    if cmd.lower() == "quit" or cmd.lower() == "exit":
                        break
                    elif cmd.lower().startswith("send "):
                        text = cmd[5:]
                        self.send(text + "\n")
                    elif cmd.lower().startswith("hex "):
                        hex_str = cmd[4:]
                        try:
                            data = bytes.fromhex(hex_str.replace(" ", ""))
                            self.send(data)
                        except ValueError:
                            print("Érvénytelen hex formátum!")
                    elif cmd.lower().startswith("baudrate "):
                        try:
                            new_baud = int(cmd[9:])
                            self.baudrate = new_baud
                            if self.ser:
                                self.ser.baudrate = new_baud
                                print(f"Baud rate: {new_baud}")
                        except ValueError:
                            print("Érvénytelen szám!")
                    elif cmd:
                        self.send(cmd + "\n")
                except KeyboardInterrupt:
                    break
        except EOFError:
            pass


def main():
    """Main function."""
    print("=== Soros Port Monitor ===")

    monitor = SerialMonitor()

    # List and select port
    ports = monitor.list_ports()
    if not ports:
        return

    while True:
        try:
            choice = input("\nVálassz port indexet (vagy Enter az 1. porthoz): ").strip()
            idx = int(choice) if choice else 0
            if 0 <= idx < len(ports):
                monitor.port = ports[idx].device
                break
        except (ValueError, IndexError):
            print("Érvénytelen választás!")

    # Optional: select baudrate
    baudrate_str = input("Baud rate (alapértelmezett: 115200): ").strip()
    if baudrate_str:
        try:
            monitor.baudrate = int(baudrate_str)
        except ValueError:
            print("Alapértelmezett érték használva: 115200")

    # Connect
    if not monitor.connect():
        return

    # Interactive mode
    try:
        monitor.interactive_mode()
    except KeyboardInterrupt:
        print("\nMegszakítva.")
    finally:
        monitor.close()


if __name__ == "__main__":
    main()
