@echo off
rem One-time move to the bigger firmware layout, keeping all settings.
rem 1) back up settings from the controller  2) flash new layout + firmware  3) write settings back
cd /d "%~dp0"
set PYTHONIOENCODING=utf-8
set PYTHONUTF8=1
set "PIO=%USERPROFILE%\.platformio\penv\Scripts\pio.exe"
set "PY=%USERPROFILE%\.platformio\penv\Scripts\python.exe"
set "IP=blled.local"
if not "%~1"=="" set "IP=%~1"
echo === MIGRATE START %DATE% %TIME% === > migrate.log

echo Step 1 of 3: backing up settings from the controller...
if not exist data mkdir data
curl.exe -s --fail -m 15 -o data\blledconfig.json "http://%IP%/configfile.json" >> migrate.log 2>&1
findstr /c:"\"ssid\"" data\blledconfig.json >nul 2>&1 || goto nobackup
findstr /c:"\"serialNumber\"" data\blledconfig.json >nul 2>&1 || goto nobackup
copy /y data\blledconfig.json "settings-backup.json" >nul
for %%F in (data\blledconfig.json) do echo Backup OK: %%~zF bytes >> migrate.log

if not exist "%USERPROFILE%\.platformio\packages\framework-arduinoespressif32\tools\partitions\min_spiffs.csv" (
  echo min_spiffs.csv not found in the ESP32 framework - nothing was flashed >> migrate.log
  goto end
)

echo Step 2 of 3: flashing the new layout and firmware over USB - don't unplug...
echo === UPLOAD === >> migrate.log
"%PIO%" run -e esp32dev -t upload >> migrate.log 2>&1
echo === UPLOAD EXIT CODE %ERRORLEVEL% === >> migrate.log
if errorlevel 1 goto end

echo Step 3 of 3: writing your settings back...
echo === UPLOADFS === >> migrate.log
"%PIO%" run -e esp32dev -t uploadfs >> migrate.log 2>&1
echo === UPLOADFS EXIT CODE %ERRORLEVEL% === >> migrate.log

echo Reading the boot log for 45 seconds...
"%PY%" read-serial.py >> migrate.log 2>&1
goto end

:nobackup
echo === BACKUP FAILED - nothing was flashed. Is the controller reachable at %IP%? === >> migrate.log
type data\blledconfig.json >nul 2>&1 && echo (got a response, but it doesn't look like a settings file) >> migrate.log

:end
echo === DONE %DATE% %TIME% === >> migrate.log
