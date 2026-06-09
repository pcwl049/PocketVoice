@echo off
setlocal enabledelayedexpansion

set "ROOT_DIR=%~dp0.."
set "PC_BUILD=%ROOT_DIR%\build\pc"
set "MOBILE_BUILD=%ROOT_DIR%\build\mobile-apk"
set "RELEASE_ROOT=%ROOT_DIR%\build\release"
set "PACKAGE_DIR=%RELEASE_ROOT%\PocketVoice-preview"
set "ZIP_PATH=%RELEASE_ROOT%\PocketVoice-preview.zip"
set "ADB_SOURCE=%ROOT_DIR%\tools\platform-tools"

if /I "%~1"=="--adb-source" (
  set "ADB_SOURCE=%~2"
)

if not exist "%ADB_SOURCE%\adb.exe" (
  if exist "D:\Project\PocketCast\tools\platform-tools\adb.exe" (
    set "ADB_SOURCE=D:\Project\PocketCast\tools\platform-tools"
  ) else if exist "D:\Project\PocketCast\tools\adb.exe" (
    set "ADB_SOURCE=D:\Project\PocketCast\tools"
  )
)

echo ============================================================
echo   PocketVoice Preview Package
echo ============================================================
echo.

call "%ROOT_DIR%\scripts\build_pc.bat"
if errorlevel 1 goto :fail

if not exist "%PC_BUILD%\stt_pc.exe" (
  echo [FAIL] Missing PC executable: %PC_BUILD%\stt_pc.exe
  goto :fail
)

if not exist "%ADB_SOURCE%\adb.exe" (
  echo [FAIL] Missing adb.exe. Use: scripts\package_preview.bat --adb-source C:\path\to\platform-tools
  goto :fail
)
if not exist "%ADB_SOURCE%\AdbWinApi.dll" (
  echo [FAIL] Missing AdbWinApi.dll in %ADB_SOURCE%
  goto :fail
)
if not exist "%ADB_SOURCE%\AdbWinUsbApi.dll" (
  echo [FAIL] Missing AdbWinUsbApi.dll in %ADB_SOURCE%
  goto :fail
)

if exist "%PACKAGE_DIR%" rmdir /s /q "%PACKAGE_DIR%"
if exist "%ZIP_PATH%" del /q "%ZIP_PATH%"
mkdir "%PACKAGE_DIR%"
mkdir "%PACKAGE_DIR%\adb"

copy /Y "%PC_BUILD%\stt_pc.exe" "%PACKAGE_DIR%\PocketVoice.exe" >nul
copy /Y "%PC_BUILD%\*.dll" "%PACKAGE_DIR%\" >nul
copy /Y "%PC_BUILD%\config.json" "%PACKAGE_DIR%\" >nul

copy /Y "%ADB_SOURCE%\adb.exe" "%PACKAGE_DIR%\adb\" >nul
copy /Y "%ADB_SOURCE%\AdbWinApi.dll" "%PACKAGE_DIR%\adb\" >nul
copy /Y "%ADB_SOURCE%\AdbWinUsbApi.dll" "%PACKAGE_DIR%\adb\" >nul
if exist "%ADB_SOURCE%\NOTICE.txt" copy /Y "%ADB_SOURCE%\NOTICE.txt" "%PACKAGE_DIR%\adb\NOTICE.txt" >nul
if exist "%ADB_SOURCE%\source.properties" copy /Y "%ADB_SOURCE%\source.properties" "%PACKAGE_DIR%\adb\source.properties" >nul

if exist "%MOBILE_BUILD%\app-signed.apk" (
  copy /Y "%MOBILE_BUILD%\app-signed.apk" "%PACKAGE_DIR%\PocketVoice-Android.apk" >nul
) else if exist "%MOBILE_BUILD%\apk\app-signed.apk" (
  copy /Y "%MOBILE_BUILD%\apk\app-signed.apk" "%PACKAGE_DIR%\PocketVoice-Android.apk" >nul
) else (
  echo [WARN] Android APK not found. Package will include PC side only.
)

> "%PACKAGE_DIR%\README.txt" (
  echo PocketVoice preview
  echo.
  echo 1. Install PocketVoice-Android.apk on the phone.
  echo 2. Enable USB debugging and connect the phone by USB.
  echo 3. Open the Android app and start the service.
  echo 4. Run PocketVoice.exe on the PC.
  echo.
  echo The PC app uses bundled adb files from the adb folder to create USB port forwarding automatically.
  echo VRChat OSC / ChatBox must be enabled separately.
)

powershell -NoProfile -ExecutionPolicy Bypass -Command "Compress-Archive -Path '%PACKAGE_DIR%' -DestinationPath '%ZIP_PATH%' -Force"
if errorlevel 1 goto :fail

echo.
echo [OK] Package folder: %PACKAGE_DIR%
echo [OK] Zip: %ZIP_PATH%
exit /b 0

:fail
echo.
echo [FAIL] Package failed
exit /b 1
