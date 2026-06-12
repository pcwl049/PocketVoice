@echo off
setlocal enabledelayedexpansion

set "PAUSE_ON_EXIT=0"
set "CHECK_ONLY=0"
if /I "%~1"=="--pause" set "PAUSE_ON_EXIT=1"
if /I "%~1"=="--check-only" set "CHECK_ONLY=1"
if /I "%~2"=="--pause" set "PAUSE_ON_EXIT=1"
if /I "%~2"=="--check-only" set "CHECK_ONLY=1"

echo ============================================================
echo   PocketVoice - QNN Android Build Script
echo   Build sherpa-onnx with QNN support for Android
echo ============================================================
echo.

:: Configuration
call "%~dp0env.bat"
set "SHERPA_SRC=%ROOT_DIR%\third_party\sherpa-onnx-src"
set "BUILD_DIR=%ROOT_DIR%\build\qnn-android"
set "OUTPUT_DIR=%ROOT_DIR%\third_party\lib\android\arm64-v8a-qnn"
set "LOG_DIR=%ROOT_DIR%\build\logs"
set "INSTALL_PREFIX=%BUILD_DIR%\install"
set "CONFIGURE_LOG=%LOG_DIR%\qnn-configure.log"
set "BUILD_LOG=%LOG_DIR%\qnn-build.log"
set "INSTALL_LOG=%LOG_DIR%\qnn-install.log"

if not exist "%LOG_DIR%" mkdir "%LOG_DIR%"

:: Check prerequisites
echo [1/6] Checking prerequisites...

if not exist "%SHERPA_SRC%" (
    echo [Error] sherpa-onnx source not found: %SHERPA_SRC%
    exit /b 1
)
echo   - sherpa-onnx source: OK

if not exist "%QNN_SDK%" (
    echo [Error] QNN SDK not found: %QNN_SDK%
    exit /b 1
)
echo   - QNN SDK: OK

if not exist "%NDK_PATH%" (
    echo [Error] Android NDK not found: %NDK_PATH%
    exit /b 1
)
echo   - Android NDK: OK

:: Setup paths
set "TOOLCHAIN=%NDK_PATH%\build\cmake\android.toolchain.cmake"
set "MAKE=%NDK_PATH%\prebuilt\windows-x86_64\bin\make.exe"

:: Set QNN SDK environment variable
set "QNN_SDK_ROOT=%QNN_SDK%"
echo   - QNN_SDK_ROOT: %QNN_SDK_ROOT%

:: Convert backslashes to forward slashes for CMake
set "QNN_SDK_ROOT_CMAKE=%QNN_SDK:\=/%"
set "INSTALL_PREFIX_CMAKE=%INSTALL_PREFIX:\=/%"
echo   - QNN_SDK_ROOT_CMAKE: %QNN_SDK_ROOT_CMAKE%

:: Create build directory
if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"
if not exist "%OUTPUT_DIR%" mkdir "%OUTPUT_DIR%"

:: Download onnxruntime if needed
echo.
echo [2/6] Checking onnxruntime...
set "ONNXRUNTIME_VER=1.24.3"
set "ONNXRUNTIME_DIR=%BUILD_DIR%\onnxruntime-%ONNXRUNTIME_VER%"
set "ONNXRUNTIME_DIRECT_DIR=%BUILD_DIR%"
set "ONNXRUNTIME_ZIP=%BUILD_DIR%\onnxruntime-android.zip"

if exist "%ONNXRUNTIME_DIR%\jni\arm64-v8a\libonnxruntime.so" (
    echo   - onnxruntime: OK - versioned cache
    echo   - source: %ONNXRUNTIME_DIR%\jni\arm64-v8a\libonnxruntime.so
) else if exist "%ONNXRUNTIME_DIRECT_DIR%\jni\arm64-v8a\libonnxruntime.so" (
    set "ONNXRUNTIME_DIR=%ONNXRUNTIME_DIRECT_DIR%"
    echo   - onnxruntime: OK - build\qnn-android\jni\arm64-v8a
    echo   - source: %ONNXRUNTIME_DIRECT_DIR%\jni\arm64-v8a\libonnxruntime.so
) else (
    if exist "%ONNXRUNTIME_ZIP%" (
        echo   - found cached onnxruntime zip, extracting...
        echo   - source: %ONNXRUNTIME_ZIP%
    ) else (
        echo   Downloading onnxruntime %ONNXRUNTIME_VER%...
        echo   - url: https://github.com/csukuangfj/onnxruntime-libs/releases/download/v%ONNXRUNTIME_VER%/onnxruntime-android-%ONNXRUNTIME_VER%.zip
        pushd "%BUILD_DIR%"
        curl -L -o onnxruntime-android.zip "https://github.com/csukuangfj/onnxruntime-libs/releases/download/v%ONNXRUNTIME_VER%/onnxruntime-android-%ONNXRUNTIME_VER%.zip" 2>nul
        if errorlevel 1 (
            popd
            echo [Error] Failed to download onnxruntime
            exit /b 1
        )
        popd
    )
    pushd "%BUILD_DIR%"
    tar -xf onnxruntime-android.zip
    if errorlevel 1 (
        popd
        echo [Error] Failed to extract onnxruntime
        exit /b 1
    )
    del onnxruntime-android.zip
    popd
    if not exist "%ONNXRUNTIME_DIRECT_DIR%\jni\arm64-v8a\libonnxruntime.so" (
        echo [Error] onnxruntime extracted but libonnxruntime.so was not found
        exit /b 1
    )
    set "ONNXRUNTIME_DIR=%ONNXRUNTIME_DIRECT_DIR%"
    echo   - onnxruntime ready
)

set "ONNXRUNTIME_DIR_CMAKE=%ONNXRUNTIME_DIR:\=/%"
set "SHERPA_ONNXRUNTIME_LIB_DIR=%ONNXRUNTIME_DIR_CMAKE%/jni/arm64-v8a"
set "SHERPA_ONNXRUNTIME_INCLUDE_DIR=%ONNXRUNTIME_DIR_CMAKE%/headers"
echo   - SHERPA_ONNXRUNTIME_LIB_DIR: %SHERPA_ONNXRUNTIME_LIB_DIR%
echo   - SHERPA_ONNXRUNTIME_INCLUDE_DIR: %SHERPA_ONNXRUNTIME_INCLUDE_DIR%

if "%CHECK_ONLY%"=="1" (
    echo.
    echo Check-only mode complete.
    exit /b 0
)

:: Configure CMake
echo.
echo [3/6] Configuring CMake with QNN support...

cmake -S "%SHERPA_SRC%" -B "%BUILD_DIR%" -G "Unix Makefiles" ^
    "-DCMAKE_TOOLCHAIN_FILE=%TOOLCHAIN%" ^
    "-DCMAKE_MAKE_PROGRAM=%MAKE%" ^
    -DANDROID_ABI=arm64-v8a ^
    -DANDROID_PLATFORM=android-24 ^
    -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_INSTALL_PREFIX=%INSTALL_PREFIX_CMAKE% ^
    -DBUILD_SHARED_LIBS=ON ^
    -DSHERPA_ONNX_ENABLE_TTS=OFF ^
    -DSHERPA_ONNX_ENABLE_SPEAKER_DIARIZATION=OFF ^
    -DSHERPA_ONNX_ENABLE_BINARY=OFF ^
    -DSHERPA_ONNX_ENABLE_WEBSOCKET=OFF ^
    -DSHERPA_ONNX_ENABLE_C_API=ON ^
    -DSHERPA_ONNX_ENABLE_JNI=OFF ^
    -DSHERPA_ONNX_ENABLE_PYTHON=OFF ^
    -DSHERPA_ONNX_ENABLE_TESTS=OFF ^
    -DSHERPA_ONNX_ENABLE_CHECK=OFF ^
    -DSHERPA_ONNX_ENABLE_PORTAUDIO=OFF ^
    -DSHERPA_ONNX_LINK_LIBSTDCPP_STATICALLY=OFF ^
    -DSHERPA_ONNX_ENABLE_QNN=ON ^
    -DQNN_SDK_ROOT="%QNN_SDK_ROOT_CMAKE%" > "%CONFIGURE_LOG%" 2>&1

if errorlevel 1 (
    echo [Error] CMake configuration failed
    echo   See: %CONFIGURE_LOG%
    exit /b 1
)
echo   - CMake configured successfully
echo   - log: %CONFIGURE_LOG%

:: Build
echo.
echo [4/6] Building sherpa-onnx with QNN (this may take 10-30 minutes)...
cd /d "%BUILD_DIR%"
"%MAKE%" -j2 > "%BUILD_LOG%" 2>&1

if errorlevel 1 (
    echo [Error] Build failed
    echo   See: %BUILD_LOG%
    exit /b 1
)
echo   - Build completed
echo   - log: %BUILD_LOG%

:: Install
echo.
echo [5/6] Installing...
"%MAKE%" install/strip > "%INSTALL_LOG%" 2>&1

if errorlevel 1 (
    echo [Error] Install failed
    echo   See: %INSTALL_LOG%
    exit /b 1
)

:: Copy outputs
echo.
echo [6/6] Copying outputs to %OUTPUT_DIR%...

:: Copy sherpa-onnx libraries
if exist "%BUILD_DIR%\install\lib\libsherpa-onnx-c-api.so" (
    copy /Y "%BUILD_DIR%\install\lib\libsherpa-onnx-c-api.so" "%OUTPUT_DIR%\" >nul
    echo   - libsherpa-onnx-c-api.so
)

if exist "%BUILD_DIR%\install\lib\libsherpa-onnx-cxx-api.so" (
    copy /Y "%BUILD_DIR%\install\lib\libsherpa-onnx-cxx-api.so" "%OUTPUT_DIR%\" >nul
    echo   - libsherpa-onnx-cxx-api.so
)

:: Copy onnxruntime
if exist "%ONNXRUNTIME_DIR%\jni\arm64-v8a\libonnxruntime.so" (
    copy /Y "%ONNXRUNTIME_DIR%\jni\arm64-v8a\libonnxruntime.so" "%OUTPUT_DIR%\" >nul
    echo   - libonnxruntime.so
)

:: Copy QNN runtime libraries from SDK
set "QNN_LIB_DIR=%QNN_SDK%\lib\aarch64-android"
if exist "%QNN_LIB_DIR%\libQnnHtp.so" (
    copy /Y "%QNN_LIB_DIR%\libQnnHtp.so" "%OUTPUT_DIR%\" >nul
    echo   - libQnnHtp.so
)
if exist "%QNN_LIB_DIR%\libQnnHtpPrepare.so" (
    copy /Y "%QNN_LIB_DIR%\libQnnHtpPrepare.so" "%OUTPUT_DIR%\" >nul
    echo   - libQnnHtpPrepare.so
)
if exist "%QNN_LIB_DIR%\libQnnHtpNetRunExtensions.so" (
    copy /Y "%QNN_LIB_DIR%\libQnnHtpNetRunExtensions.so" "%OUTPUT_DIR%\" >nul
    echo   - libQnnHtpNetRunExtensions.so
)
for %%V in (V68 V69 V73 V75 V79 V81) do (
    if exist "%QNN_LIB_DIR%\libQnnHtp%%VStub.so" (
        copy /Y "%QNN_LIB_DIR%\libQnnHtp%%VStub.so" "%OUTPUT_DIR%\" >nul
        echo   - libQnnHtp%%VStub.so
    )
    if exist "%QNN_LIB_DIR%\libQnnHtp%%VCalculatorStub.so" (
        copy /Y "%QNN_LIB_DIR%\libQnnHtp%%VCalculatorStub.so" "%OUTPUT_DIR%\" >nul
        echo   - libQnnHtp%%VCalculatorStub.so
    )
    set "HEXAGON_VERSION=%%V"
    set "HEXAGON_VERSION=!HEXAGON_VERSION:V=v!"
    if exist "%QNN_SDK%\lib\hexagon-!HEXAGON_VERSION!\unsigned\libQnnHtp%%VSkel.so" (
        copy /Y "%QNN_SDK%\lib\hexagon-!HEXAGON_VERSION!\unsigned\libQnnHtp%%VSkel.so" "%OUTPUT_DIR%\" >nul
        echo   - libQnnHtp%%VSkel.so
    )
)
if exist "%QNN_LIB_DIR%\libQnnSystem.so" (
    copy /Y "%QNN_LIB_DIR%\libQnnSystem.so" "%OUTPUT_DIR%\" >nul
    echo   - libQnnSystem.so
)
if exist "%QNN_LIB_DIR%\libQnnCpu.so" (
    copy /Y "%QNN_LIB_DIR%\libQnnCpu.so" "%OUTPUT_DIR%\" >nul
    echo   - libQnnCpu.so (fallback)
)

:: Verify QNN symbols
echo.
echo Verifying QNN symbols in libsherpa-onnx-c-api.so...
"%NDK_PATH%\toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-readobj.exe" -s "%OUTPUT_DIR%\libsherpa-onnx-c-api.so" 2>nul | findstr -i "Qnn" >nul
if errorlevel 1 (
    echo [Warning] QNN symbols not found - build may not have QNN support
) else (
    echo   - QNN symbols found: OK
)

echo.
echo ============================================================
echo   QNN Build Complete!
echo ============================================================
echo.
echo Output directory: %OUTPUT_DIR%
echo.
echo Files:
dir /b "%OUTPUT_DIR%"
echo.
echo Next steps:
echo   1. Update CMakeLists.txt to use the new libraries
echo   2. Rebuild the Android APK
echo   3. Push model.bin and tokens.txt to device
echo   4. Test on device
echo.

:end
if "%PAUSE_ON_EXIT%"=="1" pause
