@echo off
setlocal

call "%~dp0env.bat"
set "MODEL_NAME=sherpa-onnx-qnn-10-seconds-sense-voice-zh-en-ja-ko-yue-2025-09-09-int8-android-aarch64"
set "URL=https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models-qnn/%MODEL_NAME%.tar.bz2"
set "DOWNLOAD_DIR=%ROOT_DIR%\build\downloads"
set "EXTRACT_DIR=%ROOT_DIR%\build\downloads\sensevoice-qnn-libmodel"
set "OUT_DIR=%ROOT_DIR%\models\sensevoice"
set "ARCHIVE=%DOWNLOAD_DIR%\%MODEL_NAME%.tar.bz2"

echo ============================================================
echo   Download SenseVoice QNN libmodel
echo ============================================================
echo.
echo Source: %URL%
echo Output: %OUT_DIR%
echo.

if not exist "%DOWNLOAD_DIR%" mkdir "%DOWNLOAD_DIR%"
if not exist "%OUT_DIR%" mkdir "%OUT_DIR%"

if not exist "%ARCHIVE%" (
    curl -L --fail --retry 5 --retry-delay 2 -C - -o "%ARCHIVE%" "%URL%"
    if errorlevel 1 exit /b 1
) else (
    echo Using cached archive: %ARCHIVE%
)

for %%F in ("%ARCHIVE%") do (
    if %%~zF LSS 1024 (
        echo [ERROR] Downloaded archive is too small: %%~zF bytes
        del /q "%ARCHIVE%" 2>nul
        exit /b 1
    )
)

rmdir /s /q "%EXTRACT_DIR%" 2>nul
mkdir "%EXTRACT_DIR%"
tar -xjf "%ARCHIVE%" -C "%EXTRACT_DIR%"
if errorlevel 1 exit /b 1

set "SRC_DIR=%EXTRACT_DIR%\%MODEL_NAME%"
if not exist "%SRC_DIR%\libmodel.so" (
    echo [ERROR] Missing libmodel.so in %SRC_DIR%
    exit /b 1
)
if not exist "%SRC_DIR%\tokens.txt" (
    echo [ERROR] Missing tokens.txt in %SRC_DIR%
    exit /b 1
)

copy /Y "%SRC_DIR%\libmodel.so" "%OUT_DIR%\libmodel.so" >nul
copy /Y "%SRC_DIR%\tokens.txt" "%OUT_DIR%\tokens.txt" >nul
if exist "%SRC_DIR%\info.txt" copy /Y "%SRC_DIR%\info.txt" "%OUT_DIR%\info-libmodel.txt" >nul
if exist "%SRC_DIR%\README.md" copy /Y "%SRC_DIR%\README.md" "%OUT_DIR%\README-libmodel.md" >nul

echo.
echo Download complete.
echo   %OUT_DIR%\libmodel.so
echo   %OUT_DIR%\tokens.txt
echo.
echo Existing model.bin is left untouched. Remove it when you want the app to regenerate context from libmodel.so.
