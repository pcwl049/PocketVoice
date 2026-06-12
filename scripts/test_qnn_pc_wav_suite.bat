@echo off
setlocal

call "%~dp0env.bat"
set "STT_SENSEVOICE_QNN_LIBMODEL=%ROOT_DIR%\build\qnn-model-lib-android\sensevoice-act16-fixed-prompt-expanded-preserve-layout-restrict\libs\arm64-v8a\libmodel.so"
set "STT_SKIP_QNN_LIBMODEL_REPAIR=1"
set "STT_SENSEVOICE_QNN_LIBMODEL_FIRST=1"

echo ============================================================
echo   QNN PC WAV regression suite
echo ============================================================
echo.

call "%ROOT_DIR%\scripts\check_qnn_prereqs.bat"
if errorlevel 1 exit /b 1

call "%ROOT_DIR%\scripts\build_qnn_android.bat"
if errorlevel 1 exit /b 1

call "%ROOT_DIR%\scripts\build_mobile_apk.bat" --qnn
if errorlevel 1 exit /b 1

"%ADB%" install -r "%ROOT_DIR%\build\mobile-apk\app-signed.apk"
if errorlevel 1 exit /b 1

call "%ROOT_DIR%\scripts\push_sensevoice_qnn_model.bat"
if errorlevel 1 exit /b 1

set "STT_EXPECT_BACKEND=sensevoice_qnn"
call "%ROOT_DIR%\scripts\test_pc_wav_suite.bat"
