@echo off
rem Builds BLLED 2.4.0 with the PlatformIO already installed on this PC.
rem Output goes to build.log next to this file.
cd /d "%~dp0"
rem PlatformIO crashes writing emoji to a log file in the default Windows code page, so force UTF-8
set PYTHONIOENCODING=utf-8
set PYTHONUTF8=1
set "PIO=%USERPROFILE%\.platformio\penv\Scripts\pio.exe"
if not exist "%PIO%" set "PIO=pio"
echo Building BLLED 2.4.0 - this window closes when done (first build downloads the ESP32 toolchain)...
echo === BUILD START %DATE% %TIME% === > build.log
"%PIO%" run -e esp32dev >> build.log 2>&1
echo === EXIT CODE %ERRORLEVEL% === >> build.log
echo === BUILD END %DATE% %TIME% === >> build.log
dir /b .firmware >> build.log 2>&1
