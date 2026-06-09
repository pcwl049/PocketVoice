@echo off
echo ============================================================
echo   VRChat STT - ADB Port Forward
echo ============================================================
echo.

set PORT=27000

echo [1] Checking ADB connection...
adb devices

echo.
echo [2] Setting up port forward (port %PORT%)...
adb forward tcp:%PORT% tcp:%PORT%

if errorlevel 1 (
    echo [Error] Port forward failed
    echo Make sure:
    echo   1. USB debugging is enabled
    echo   2. Phone is connected via USB
    pause
    exit /b 1
)

echo.
echo [3] Verifying...
adb forward --list | findstr "%PORT%"

echo.
echo ============================================================
echo   Port forward active!
echo   Local port %PORT% -^> Phone port %PORT%
echo ============================================================
pause
