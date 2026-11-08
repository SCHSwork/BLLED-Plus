@echo off
rem Builds the BLLED Android app with Android Studio's own Java and SDK. Output: BLLED.apk, log: build.log
cd /d "%~dp0"
set PYTHONIOENCODING=utf-8
echo === ENV %DATE% %TIME% === > build.log
set JAVA_HOME=
for %%J in ("%ProgramFiles%\Android\Android Studio\jbr" "%ProgramFiles%\Android\Android Studio1\jbr" "%LOCALAPPDATA%\Programs\Android Studio\jbr" "%ProgramFiles%\Android\Android Studio\jre") do if exist "%%~J\bin\java.exe" if not defined JAVA_HOME set "JAVA_HOME=%%~J"
if not defined JAVA_HOME for %%R in ("%ProgramFiles%" "%LOCALAPPDATA%\Programs" "%LOCALAPPDATA%\JetBrains" "%USERPROFILE%\.gradle\jdks" "%USERPROFILE%\.jdks" "%ProgramFiles(x86)%" "D:\") do if not defined JAVA_HOME if exist "%%~R" for /f "delims=" %%F in ('where /r "%%~R" java.exe 2^>nul') do if not defined JAVA_HOME call :pick "%%~F"
echo Java search done >> build.log
rem No JDK on this PC: use a portable Eclipse Temurin 17 kept inside this folder
if not defined JAVA_HOME if exist "%~dp0jdk\bin\javac.exe" set "JAVA_HOME=%~dp0jdk"
if not defined JAVA_HOME (
  echo Downloading portable Java 17 from adoptium.net, about 190 MB...
  echo Downloading Temurin JDK 17 >> build.log
  curl.exe -L --fail -o "%~dp0jdk.zip" "https://api.adoptium.net/v3/binary/latest/17/ga/windows/x64/jdk/hotspot/normal/eclipse" >> build.log 2>&1
  for %%Z in ("%~dp0jdk.zip") do echo Downloaded %%~zZ bytes >> build.log
  mkdir "%~dp0jdk-tmp" 2>nul
  tar -xf "%~dp0jdk.zip" -C "%~dp0jdk-tmp" >> build.log 2>&1
  for /d %%D in ("%~dp0jdk-tmp\*") do move "%%~fD" "%~dp0jdk" >> build.log 2>&1
  rmdir "%~dp0jdk-tmp" 2>nul
  del "%~dp0jdk.zip" 2>nul
)
if not defined JAVA_HOME if exist "%~dp0jdk\bin\javac.exe" set "JAVA_HOME=%~dp0jdk"
set "SDK=%LOCALAPPDATA%\Android\Sdk"
echo JAVA_HOME=%JAVA_HOME% >> build.log
echo SDK=%SDK% >> build.log
dir /b "%SDK%\platforms" >> build.log 2>&1
dir /b "%SDK%\build-tools" >> build.log 2>&1
set "SDKF=%SDK:\=/%"
> local.properties echo sdk.dir=%SDKF%
echo Building the BLLED app - this window closes when done (first build downloads Gradle)...
echo === BUILD === >> build.log
call gradlew.bat assembleDebug --no-daemon --console=plain >> build.log 2>&1
echo === EXIT CODE %ERRORLEVEL% === >> build.log
if exist app\build\outputs\apk\debug\app-debug.apk copy /y app\build\outputs\apk\debug\app-debug.apk BLLED.apk >> build.log
echo === DONE %DATE% %TIME% === >> build.log
goto :eof

:pick
rem Use a full JDK (it has javac next to java), preferring Android Studio's bundled one
if exist "%~dp1javac.exe" for %%P in ("%~dp1..") do set "JAVA_HOME=%%~fP"
if defined JAVA_HOME echo Found Java: %JAVA_HOME% >> build.log
goto :eof
