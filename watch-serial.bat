@echo off
cd /d "%~dp0"
echo Recording controller serial output for 90 seconds...
"%USERPROFILE%\.platformio\penv\Scripts\python.exe" watch-serial.py
