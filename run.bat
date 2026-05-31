@echo off
REM run.bat — FanController eszkezok menu
REM Ez a file letöltéskor nem blokkolódik, mert .bat, csak a Python-ből hívja őket

setlocal enabledelayedexpansion
cd /d "%~dp0"

:menu
cls
echo.
echo ============================================
echo   FanController Diagnosztikai Eszközök
echo ============================================
echo.
echo 1) Diag naplo lekerdezes (diag_client.py)
echo 2) Fokozat-edzos 45 perc (fan_stress.py durva)
echo 3) Fokozat-edzos vegtelen (fan_stress.py light)
echo 4) OTA firmware check (ota_diagnostic.py)
echo 5) PowerShell ablak (teljes kontrol)
echo 0) Kilepes
echo.
set /p choice="Valassz (0-5): "

if "%choice%"=="1" goto diag
if "%choice%"=="2" goto stress_heavy
if "%choice%"=="3" goto stress_light
if "%choice%"=="4" goto ota_check
if "%choice%"=="5" goto powershell
if "%choice%"=="0" exit /b 0
echo Ervenytelen valasztas!
timeout /t 2 >nul
goto menu

:diag
echo.
echo Diag naplo lekerdezes... (gombnyomas nelkul: Ctrl+C szakit meg)
echo.
python3 diag_client.py %*
echo.
pause
goto menu

:stress_heavy
echo.
echo Fokozat-edzos 45 perc, durva reloe-stressz, percenkenti reset-ellenorzes...
echo.
python3 fan_stress.py --duration 2700 --roller-toggle --check-interval 60 --log stress.csv
echo.
pause
goto menu

:stress_light
echo.
echo Fokozat-edzos vegtelen, 1-2-3 fokozat, 3mp kozott, konnyu terhelés
echo (Ctrl+C szakit meg)
echo.
python3 fan_stress.py
echo.
pause
goto menu

:ota_check
echo.
set /p firmware="Add meg a firmware.bin eleres: "
if not exist "!firmware!" (
    echo Fajl nem letezik: !firmware!
    pause
    goto menu
)
echo.
python3 ota_diagnostic.py "!firmware!"
echo.
pause
goto menu

:powershell
echo.
echo PowerShell (teljes kontrol, itt futtathatod a python parancsokat a kezitol)
echo Pl: python3 diag_client.py --pin 123456 --address AA:BB:CC:DD:EE:FF
echo Kilepes: exit
echo.
powershell.exe -NoExit -Command "cd '%cd%'"
goto menu
