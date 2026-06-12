@echo off
setlocal

call "%~dp0env.bat"
set "MODEL_DIR=G:\STTModels\models\sherpa-onnx-qwen3-asr-0.6B-int8-2026-03-25"
if not "%STT_QWEN3_ASR_MODEL_DIR%"=="" set "MODEL_DIR=%STT_QWEN3_ASR_MODEL_DIR%"
set "HOTWORDS_FILE=%MODEL_DIR%\qwen3_hotwords.txt"
if not "%STT_QWEN3_HOTWORDS_FILE%"=="" set "HOTWORDS_FILE=%STT_QWEN3_HOTWORDS_FILE%"
set "DEVICE_DIR=/sdcard/Android/data/com.stt.mobile/files/models/qwen3-asr-0.6b"

echo ============================================================
echo   Push Qwen3-ASR 0.6B model to Android
echo ============================================================
echo.

if not exist "%ADB%" (
    echo [Error] adb not found: %ADB%
    exit /b 1
)

if not exist "%MODEL_DIR%\conv_frontend.onnx" (
    echo [Error] Missing %MODEL_DIR%\conv_frontend.onnx
    exit /b 1
)
if not exist "%MODEL_DIR%\encoder.int8.onnx" (
    echo [Error] Missing %MODEL_DIR%\encoder.int8.onnx
    exit /b 1
)
if not exist "%MODEL_DIR%\decoder.int8.onnx" (
    echo [Error] Missing %MODEL_DIR%\decoder.int8.onnx
    exit /b 1
)
if not exist "%MODEL_DIR%\tokenizer\vocab.json" (
    echo [Error] Missing %MODEL_DIR%\tokenizer\vocab.json
    exit /b 1
)

echo [1/2] Creating device directory...
"%ADB%" shell mkdir -p "%DEVICE_DIR%/tokenizer"
if errorlevel 1 exit /b 1

echo [2/2] Pushing model files...
"%ADB%" push "%MODEL_DIR%\conv_frontend.onnx" "%DEVICE_DIR%/"
if errorlevel 1 exit /b 1
"%ADB%" push "%MODEL_DIR%\encoder.int8.onnx" "%DEVICE_DIR%/"
if errorlevel 1 exit /b 1
"%ADB%" push "%MODEL_DIR%\decoder.int8.onnx" "%DEVICE_DIR%/"
if errorlevel 1 exit /b 1
"%ADB%" push "%MODEL_DIR%\tokenizer" "%DEVICE_DIR%/"
if errorlevel 1 exit /b 1

if exist "%MODEL_DIR%\cpu_threads.txt" (
    "%ADB%" push "%MODEL_DIR%\cpu_threads.txt" "%DEVICE_DIR%/"
    if errorlevel 1 exit /b 1
) else (
    "%ADB%" shell "echo 4 > %DEVICE_DIR%/cpu_threads.txt"
    if errorlevel 1 exit /b 1
)

if exist "%HOTWORDS_FILE%" (
    "%ADB%" push "%HOTWORDS_FILE%" "%DEVICE_DIR%/qwen3_hotwords.txt"
    if errorlevel 1 exit /b 1
) else (
    "%ADB%" shell rm -f "%DEVICE_DIR%/qwen3_hotwords.txt"
    if errorlevel 1 exit /b 1
)

echo.
echo Done: %DEVICE_DIR%
