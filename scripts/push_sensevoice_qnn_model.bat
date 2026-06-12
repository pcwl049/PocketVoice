@echo off
setlocal

call "%~dp0env.bat"
set "MODEL_DIR=%ROOT_DIR%\models\sensevoice"
set "LIBMODEL_PATH=%ROOT_DIR%\build\qnn-model-lib-android\sensevoice-act16-fixed-prompt-expanded-preserve-layout-restrict\libs\arm64-v8a\libmodel.so"
if not exist "%LIBMODEL_PATH%" set "LIBMODEL_PATH=%MODEL_DIR%\libmodel.so"
if not "%STT_SENSEVOICE_QNN_LIBMODEL%"=="" set "LIBMODEL_PATH=%STT_SENSEVOICE_QNN_LIBMODEL%"
set "QNN_VTCM_MB=%STT_QNN_VTCM_MB%"
if "%QNN_VTCM_MB%"=="" set "QNN_VTCM_MB=16"
set "DEVICE_DIR=/sdcard/Android/data/com.stt.mobile/files/models/sensevoice"

echo ============================================================
echo   Push SenseVoice QNN model to Android
echo ============================================================
echo.

if not exist "%ADB%" (
    echo [Error] adb not found: %ADB%
    exit /b 1
)

if not exist "%MODEL_DIR%\model.bin" if not exist "%LIBMODEL_PATH%" (
    echo [Error] Missing %MODEL_DIR%\model.bin or %LIBMODEL_PATH%
    exit /b 1
)
if not exist "%MODEL_DIR%\tokens.txt" (
    echo [Error] Missing %MODEL_DIR%\tokens.txt
    exit /b 1
)

echo [1/2] Creating device directory...
"%ADB%" shell mkdir -p "%DEVICE_DIR%"
if errorlevel 1 exit /b 1

echo [2/2] Pushing model files...
set "PUSH_LIBMODEL=0"
if /I "%STT_SENSEVOICE_QNN_LIBMODEL_FIRST%"=="1" if exist "%LIBMODEL_PATH%" set "PUSH_LIBMODEL=1"
if not exist "%MODEL_DIR%\model.bin" set "PUSH_LIBMODEL=1"

if "%PUSH_LIBMODEL%"=="1" (
    "%ADB%" shell rm -f "%DEVICE_DIR%/model.bin" "%DEVICE_DIR%/libmodel.so"
    if errorlevel 1 exit /b 1
    "%ADB%" push "%LIBMODEL_PATH%" "%DEVICE_DIR%/libmodel.so"
    if errorlevel 1 exit /b 1
) else (
    "%ADB%" push "%MODEL_DIR%\model.bin" "%DEVICE_DIR%/"
    if errorlevel 1 exit /b 1
)
"%ADB%" push "%MODEL_DIR%\tokens.txt" "%DEVICE_DIR%/"
if errorlevel 1 exit /b 1
"%ADB%" shell rm -f "%DEVICE_DIR%/qnn_vtcm_mb.txt"
if errorlevel 1 exit /b 1
"%ADB%" shell "echo %QNN_VTCM_MB% > %DEVICE_DIR%/qnn_vtcm_mb.txt"
if errorlevel 1 exit /b 1

echo.
echo Done: %DEVICE_DIR%
