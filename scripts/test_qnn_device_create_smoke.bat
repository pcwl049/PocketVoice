@echo off
setlocal

call "%~dp0env.bat"
if "%QNN_SDK%"=="" (
  echo [ERROR] QNN SDK not found. Set QNN_SDK_ROOT.
  exit /b 1
)
if "%NDK_PATH%"=="" (
  echo [ERROR] Android NDK not found. Set ANDROID_NDK_ROOT.
  exit /b 1
)
set "OUT_DIR=%ROOT_DIR%\build\qnn-device-create-smoke"
set "TARGET_DIR=/data/local/tmp/stt_qnn_device_smoke"
set "CXX=%NDK_PATH%\toolchains\llvm\prebuilt\windows-x86_64\bin\aarch64-linux-android35-clang++.cmd"

if not exist "%OUT_DIR%" mkdir "%OUT_DIR%"

"%CXX%" ^
  -std=c++17 ^
  -I"%QNN_SDK%\include\QNN" ^
  -I"%QNN_SDK%\include" ^
  "%ROOT_DIR%\tests\qnn_device_create_smoke.cpp" ^
  -ldl ^
  -o "%OUT_DIR%\qnn_device_create_smoke"
if errorlevel 1 exit /b 1

"%ADB%" shell "rm -rf %TARGET_DIR% && mkdir -p %TARGET_DIR%"
"%ADB%" push "%OUT_DIR%\qnn_device_create_smoke" "%TARGET_DIR%/qnn_device_create_smoke"
"%ADB%" push "%QNN_SDK%\lib\aarch64-android\libQnnHtp.so" "%TARGET_DIR%/libQnnHtp.so"
"%ADB%" push "%QNN_SDK%\lib\aarch64-android\libQnnHtpV79Stub.so" "%TARGET_DIR%/libQnnHtpV79Stub.so"
"%ADB%" push "%QNN_SDK%\lib\hexagon-v79\unsigned\libQnnHtpV79Skel.so" "%TARGET_DIR%/libQnnHtpV79Skel.so"

"%ADB%" shell "cd %TARGET_DIR% && chmod 755 ./qnn_device_create_smoke && export LD_LIBRARY_PATH=%TARGET_DIR% && export ADSP_LIBRARY_PATH=%TARGET_DIR% && ./qnn_device_create_smoke ./libQnnHtp.so"
exit /b %ERRORLEVEL%
