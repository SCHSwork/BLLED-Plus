@echo off
rem Flashes BLLED 2.4.0 to the controller over USB, then records 45 s of its boot log.
cd /d "%~dp0"
set PYTHONIOENCODING=utf-8
set PYTHONUTF8=1
set "PIO=%USERPROFILE%\.platformio\penv\Scripts\pio.exe"
set "PY=%USERPROFILE%\.platformio\penv\Scripts\python.exe"
echo Flashing BLLED 2.4.0 over USB - don't unplug the controller...
echo === PORTS === > upload.log
"%PIO%" device list >> upload.log 2>&1
echo === UPLOAD START %DATE% %TIME% === >> upload.log
"%PIO%" run -e esp32dev -t upload >> upload.log 2>&1
echo === EXIT CODE %ERRORLEVEL% === >> upload.log
echo Reading boot log for 45 seconds...
"%PY%" read-serial.py >> upload.log 2>&1
echo === DONE %DATE% %TIME% === >> upload.log
