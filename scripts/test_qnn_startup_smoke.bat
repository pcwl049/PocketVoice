@echo off
setlocal enabledelayedexpansion

set "ROOT_DIR=%~dp0.."
call "%~dp0env.bat"
set "PACKAGE=com.stt.mobile"
set "APP=com.stt.mobile/.MainActivity"
set "PORT=27000"
set "TIMEOUT_MS=90000"
if not "%STT_QNN_STARTUP_TIMEOUT_MS%"=="" set "TIMEOUT_MS=%STT_QNN_STARTUP_TIMEOUT_MS%"

set /a "LOOPS=%TIMEOUT_MS% / 1000"
if %LOOPS% LSS 1 set "LOOPS=1"

set "RESULT_DIR=%ROOT_DIR%\build\test-results"
if not exist "%RESULT_DIR%" mkdir "%RESULT_DIR%"
set "LOG_FILE=%RESULT_DIR%\qnn-startup-smoke-latest.txt"

echo ============================================================
echo   QNN Android startup smoke
echo ============================================================
echo   timeout: %TIMEOUT_MS% ms
echo   log: %LOG_FILE%
echo ============================================================
echo.

if not exist "%ADB%" (
    echo [FAIL] adb not found: %ADB%
    exit /b 1
)

"%ADB%" devices > "%LOG_FILE%" 2>&1
findstr /R /C:"device$" "%LOG_FILE%" >nul
if errorlevel 1 (
    type "%LOG_FILE%"
    echo [FAIL] no connected Android device
    exit /b 1
)

"%ADB%" logcat -c >> "%LOG_FILE%" 2>&1
"%ADB%" shell input keyevent KEYCODE_WAKEUP >> "%LOG_FILE%" 2>&1
"%ADB%" shell wm dismiss-keyguard >> "%LOG_FILE%" 2>&1
"%ADB%" shell am force-stop com.stt.mobile >> "%LOG_FILE%" 2>&1
"%ADB%" shell am start -n com.stt.mobile/.MainActivity --ez autoStart true >> "%LOG_FILE%" 2>&1
if errorlevel 1 (
    type "%LOG_FILE%"
    echo [FAIL] failed to start Android app
    exit /b 1
)

"%ADB%" forward tcp:27000 tcp:27000 >> "%LOG_FILE%" 2>&1
if errorlevel 1 (
    type "%LOG_FILE%"
    echo [FAIL] adb forward tcp:27000 tcp:27000 failed
    exit /b 1
)

set "SERVER_READY=0"
set "QNN_READY=0"

for /L %%I in (1,1,%LOOPS%) do (
    "%ADB%" logcat -d -s STT_Native STT_Engine sherpa-onnx QnnDsp QnnHtp QnnDspTransport > "%RESULT_DIR%\qnn-startup-smoke-current.txt" 2>&1
    findstr /C:"Server listening on port 27000" "%RESULT_DIR%\qnn-startup-smoke-current.txt" >nul
    if not errorlevel 1 set "SERVER_READY=1"
    findstr /C:"Recognizer backend: sensevoice_qnn" "%RESULT_DIR%\qnn-startup-smoke-current.txt" >nul
    if not errorlevel 1 set "QNN_READY=1"
    if "!SERVER_READY!"=="1" if "!QNN_READY!"=="1" goto :ready
    powershell -NoProfile -Command "Start-Sleep -Seconds 1" >nul
)

type "%RESULT_DIR%\qnn-startup-smoke-current.txt" >> "%LOG_FILE%"
type "%LOG_FILE%"
echo [FAIL] QNN startup smoke timed out waiting for Server listening on port 27000 and Recognizer backend: sensevoice_qnn
exit /b 1

:ready
type "%RESULT_DIR%\qnn-startup-smoke-current.txt" >> "%LOG_FILE%"
echo [PASS] QNN startup smoke passed
echo [PASS] Server listening on port 27000
echo [PASS] Recognizer backend: sensevoice_qnn
exit /b 0
