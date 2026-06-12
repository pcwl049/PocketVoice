@echo off
setlocal enabledelayedexpansion

call "%~dp0env.bat"
set "PC_BUILD=%ROOT_DIR%\build\pc"
set "MOBILE_BUILD=%ROOT_DIR%\build\mobile-apk"
set "RELEASE_ROOT=%ROOT_DIR%\build\release"
set "PACKAGE_DIR=%RELEASE_ROOT%\PocketVoice-preview"
set "ZIP_PATH=%RELEASE_ROOT%\PocketVoice-preview.zip"
set "ADB_SOURCE=%ROOT_DIR%\tools\platform-tools"
set "LICENSE_DIR=%PACKAGE_DIR%\licenses"

if /I "%~1"=="--adb-source" (
  set "ADB_SOURCE=%~2"
)

if not exist "%ADB_SOURCE%\adb.exe" (
  for %%A in ("%ADB%") do (
    if exist "%%~fA" set "ADB_SOURCE=%%~dpA"
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
mkdir "%LICENSE_DIR%"

copy /Y "%PC_BUILD%\stt_pc.exe" "%PACKAGE_DIR%\PocketVoice.exe" >nul
copy /Y "%PC_BUILD%\*.dll" "%PACKAGE_DIR%\" >nul
copy /Y "%PC_BUILD%\config.json" "%PACKAGE_DIR%\" >nul
if exist "%ROOT_DIR%\LICENSE" copy /Y "%ROOT_DIR%\LICENSE" "%LICENSE_DIR%\POCKETVOICE_LICENSE.txt" >nul
if exist "%ROOT_DIR%\THIRD_PARTY_NOTICES.txt" copy /Y "%ROOT_DIR%\THIRD_PARTY_NOTICES.txt" "%PACKAGE_DIR%\THIRD_PARTY_NOTICES.txt" >nul
if exist "%ROOT_DIR%\third_party\sherpa-onnx-src\LICENSE" copy /Y "%ROOT_DIR%\third_party\sherpa-onnx-src\LICENSE" "%LICENSE_DIR%\SHERPA_ONNX_LICENSE.txt" >nul
if exist "%ROOT_DIR%\build\third_party\FireRedVAD2\LICENSE" copy /Y "%ROOT_DIR%\build\third_party\FireRedVAD2\LICENSE" "%LICENSE_DIR%\FIREREDVAD_LICENSE.txt" >nul
if exist "%ROOT_DIR%\build\qairt-py310-venv\Lib\site-packages\onnxruntime\LICENSE" copy /Y "%ROOT_DIR%\build\qairt-py310-venv\Lib\site-packages\onnxruntime\LICENSE" "%LICENSE_DIR%\ONNXRUNTIME_LICENSE.txt" >nul
if exist "%ROOT_DIR%\build\qairt-sdk\LICENSE.pdf" copy /Y "%ROOT_DIR%\build\qairt-sdk\LICENSE.pdf" "%LICENSE_DIR%\QUALCOMM_QAIRT_LICENSE.pdf" >nul
if exist "%ROOT_DIR%\build\qairt-sdk\QNN_NOTICE.txt" copy /Y "%ROOT_DIR%\build\qairt-sdk\QNN_NOTICE.txt" "%LICENSE_DIR%\QUALCOMM_QNN_NOTICE.txt" >nul
if exist "%ROOT_DIR%\build\qairt-sdk\NOTICE_WINDOWS.txt" copy /Y "%ROOT_DIR%\build\qairt-sdk\NOTICE_WINDOWS.txt" "%LICENSE_DIR%\QUALCOMM_NOTICE_WINDOWS.txt" >nul
if exist "%ROOT_DIR%\models\sensevoice\sherpa-onnx-qnn-SM8550-binary-30-seconds-sense-voice-zh-en-ja-ko-yue-2024-07-17-int8\LICENSE" copy /Y "%ROOT_DIR%\models\sensevoice\sherpa-onnx-qnn-SM8550-binary-30-seconds-sense-voice-zh-en-ja-ko-yue-2024-07-17-int8\LICENSE" "%LICENSE_DIR%\SENSEVOICE_MODEL_LICENSE.txt" >nul

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
