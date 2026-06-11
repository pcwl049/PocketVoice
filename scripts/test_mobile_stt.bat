@echo off
setlocal

set "ROOT_DIR=D:\Project\STT"
set "ADB=D:\Android\Sdk\platform-tools\adb.exe"
set "WAV=%ROOT_DIR%\models\zipformer-ctc\test_wavs\0.wav"
set "PORT=27000"

echo ============================================================
echo   PocketVoice Android smoke test
echo ============================================================
echo.

if not exist "%ADB%" (
    echo [Error] adb not found: %ADB%
    exit /b 1
)

if not exist "%WAV%" (
    echo [Error] Test WAV not found: %WAV%
    exit /b 1
)

echo [1/3] Checking device...
"%ADB%" devices
if errorlevel 1 exit /b 1

echo.
echo [2/3] Forwarding tcp:%PORT%...
"%ADB%" forward tcp:%PORT% tcp:%PORT%
if errorlevel 1 exit /b 1

echo.
echo [3/3] Sending test WAV...
node "%ROOT_DIR%\scripts\send_wav_to_phone.js" "%WAV%" 127.0.0.1 %PORT%
if errorlevel 1 exit /b 1

echo.
echo Smoke test complete.
