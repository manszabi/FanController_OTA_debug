@echo off
REM ota_diagnostic.bat — OTA firmware.bin diagnózis
REM Használat: ota_diagnostic.bat <firmware.bin>

python3 "%~dp0ota_diagnostic.py" %*
