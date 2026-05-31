@echo off
REM diag_client.bat — FanController diag napló lekérdezése BLE-n
REM Használat: diag_client.bat [--pin 123456] [--address AA:BB:CC:DD:EE:FF]

python3 "%~dp0diag_client.py" %*
