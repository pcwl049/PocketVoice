@echo off
setlocal

set "ROOT_DIR=%~dp0.."
set "RUN_STATIC=1"
set "RUN_CPU=1"
set "RUN_QNN=1"

if /I "%~1"=="--static-only" (
    set "RUN_CPU=0"
    set "RUN_QNN=0"
)
if /I "%~1"=="--no-qnn" (
    set "RUN_QNN=0"
)
if /I "%~2"=="--no-qnn" (
    set "RUN_QNN=0"
)

echo ============================================================
echo   PocketVoice - Release Verification
echo ============================================================
echo.

if "%RUN_STATIC%"=="1" (
    echo [1/3] Static tests
    cmd /c "node "%ROOT_DIR%\scripts\qnn_build_script.test.js" && node "%ROOT_DIR%\scripts\qnn_model_metadata.test.js" && node "%ROOT_DIR%\scripts\convert_sensevoice_qnn_variant.test.js" && node "%ROOT_DIR%\scripts\build_qnn_model_lib_android.test.js" && node "%ROOT_DIR%\scripts\qnn_net_run_reference_gate.test.js" && node "%ROOT_DIR%\scripts\qnn_int8_fixed_prompt_gate.test.js" && node "%ROOT_DIR%\scripts\sensevoice_onnx_reference_gate.test.js" && node "%ROOT_DIR%\scripts\pc_wav_suite.test.js" && node "%ROOT_DIR%\scripts\pc_webview_shell.test.js" && node "%ROOT_DIR%\scripts\pc_logger.test.js" && node "%ROOT_DIR%\scripts\app_icon.test.js" && node "%ROOT_DIR%\scripts\package_preview.test.js" && node "%ROOT_DIR%\scripts\android_pressure_suite.test.js" && node "%ROOT_DIR%\scripts\sherpa_qnn_c_api.test.js" && node "%ROOT_DIR%\scripts\mobile_qnn_integration.test.js" && node "%ROOT_DIR%\scripts\verify_release.test.js""
    if errorlevel 1 goto :fail
    call "%ROOT_DIR%\scripts\test_audio_buffer.bat"
    if errorlevel 1 goto :fail
    echo.
) else (
    echo [1/3] Static tests skipped
)

if "%RUN_CPU%"=="1" (
    echo [2/3] CPU fallback WAV suite
    set "STT_EXPECT_BACKEND="
    call "%ROOT_DIR%\scripts\test_pc_wav_suite.bat"
    if errorlevel 1 goto :fail
    echo.
) else (
    echo [2/3] CPU fallback WAV suite skipped
)

if "%RUN_QNN%"=="1" (
    echo [3/3] QNN Android WAV suite
    call "%ROOT_DIR%\scripts\test_qnn_startup_smoke.bat"
    if errorlevel 1 goto :fail
    call "%ROOT_DIR%\scripts\test_qnn_pc_wav_suite.bat"
    if errorlevel 1 goto :fail
    echo.
) else (
    echo [3/3] QNN Android WAV suite skipped
)

echo ============================================================
echo   Release verification passed
echo ============================================================
exit /b 0

:fail
echo.
echo ============================================================
echo   Release verification failed
echo ============================================================
exit /b 1
