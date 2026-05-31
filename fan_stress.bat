@echo off
REM fan_stress.bat — FanController fokozat-edzés BLE-n
REM Használat: fan_stress.bat [--dwell 3] [--cycles 50] [--reconnect]

python3 "%~dp0fan_stress.py" %*
