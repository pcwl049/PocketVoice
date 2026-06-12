@echo off
setlocal

call "%~dp0env.bat"
set "MODEL_DIR=%ROOT_DIR%\models\zipformer-ctc"
set "DEVICE_DIR=/sdcard/Android/data/com.stt.mobile/files/models/zipformer-ctc"

echo ============================================================
echo   Push Zipformer CTC model to Android
echo ============================================================
echo.

if not exist "%ADB%" (
    echo [Error] adb not found: %ADB%
    exit /b 1
)

if not exist "%MODEL_DIR%\model.int8.onnx" (
    echo [Error] Missing %MODEL_DIR%\model.int8.onnx
    exit /b 1
)
if not exist "%MODEL_DIR%\bbpe.model" (
    echo [Error] Missing %MODEL_DIR%\bbpe.model
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
"%ADB%" push "%MODEL_DIR%\model.int8.onnx" "%DEVICE_DIR%/"
if errorlevel 1 exit /b 1
"%ADB%" push "%MODEL_DIR%\bbpe.model" "%DEVICE_DIR%/"
if errorlevel 1 exit /b 1
"%ADB%" push "%MODEL_DIR%\tokens.txt" "%DEVICE_DIR%/"
if errorlevel 1 exit /b 1

echo.
echo Done: %DEVICE_DIR%
