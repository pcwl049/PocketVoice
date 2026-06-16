@echo off
echo ============================================================
echo   Push Paraformer offline model to Android device
echo ============================================================
echo.

set "LOCAL_DIR=%~dp0..\models\paraformer-offline"
set "DEVICE_DIR=/sdcard/Android/data/com.stt.mobile/files/models/paraformer-offline"

echo Pushing from: %LOCAL_DIR%
echo Pushing to:   %DEVICE_DIR%
echo.

:: Check if model directory exists at all
if not exist "%LOCAL_DIR%" (
    echo [Error] Model directory not found: %LOCAL_DIR%
    echo.
    echo The offline Paraformer model is not in the repository (too large for git).
    echo Download it manually before pushing:
    echo.
    echo   mkdir models\paraformer-offline
    echo   curl -L -o models\paraformer-offline\model.int8.onnx ^
    echo       "https://huggingface.co/csukuangfj/paraformer-offline-zh/resolve/main/model.int8.onnx"
    echo   curl -L -o models\paraformer-offline\tokens.txt ^
    echo       "https://huggingface.co/csukuangfj/paraformer-offline-zh/resolve/main/tokens.txt"
    echo.
    echo Source: https://huggingface.co/csukuangfj/paraformer-offline-zh
    echo Required files: model.int8.onnx (232 MB) + tokens.txt (76 KB)
    exit /b 1
)

adb shell mkdir -p %DEVICE_DIR%

:: Push the combined offline model (int8 preferred, full precision fallback)
if exist "%LOCAL_DIR%\model.int8.onnx" (
    echo Pushing model.int8.onnx...
    adb push "%LOCAL_DIR%\model.int8.onnx" %DEVICE_DIR%/
) else if exist "%LOCAL_DIR%\model.onnx" (
    echo Pushing model.onnx (full precision)...
    adb push "%LOCAL_DIR%\model.onnx" %DEVICE_DIR%/
) else (
    echo [Error] No model file found at %LOCAL_DIR%
    echo.
    echo The offline Paraformer model uses a single combined ONNX file.
    echo Download from: csukuangfj/paraformer-offline-zh on HuggingFace
    exit /b 1
)

if exist "%LOCAL_DIR%\tokens.txt" (
    echo Pushing tokens.txt...
    adb push "%LOCAL_DIR%\tokens.txt" %DEVICE_DIR%/
) else (
    echo [Warning] tokens.txt not found at %LOCAL_DIR%
)

echo.
echo Done! Model pushed to %DEVICE_DIR%
