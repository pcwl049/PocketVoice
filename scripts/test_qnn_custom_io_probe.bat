@echo off
setlocal enabledelayedexpansion

REM === Gate B3 Task 3: Deploy and run tiny custom_io probe with qnn-net-run ===
REM Tests whether custom_io QuantParam survives HTP graphFinalize

call "%~dp0env.bat"

REM === Configuration ===
if "%QNN_SDK_ROOT%"=="" set "QNN_SDK_ROOT=G:\Program Files\qairt\2.45.0.260326"
if "%ADB%"=="" set "ADB=D:\Android\Sdk\platform-tools\adb.exe"
set "PROBE_ROOT=G:\STTModels\qnn-work\tiny-custom-io-probe"
set "RESULTS_DIR=%~dp0..\build\test-results\gate-b3-tiny-custom-io-probe"
set "DEVICE_ROOT=/data/local/tmp/tiny-custom-io-probe"

echo ============================================
echo  Gate B3: Tiny Custom IO Probe - qnn-net-run
echo ============================================
echo.

REM === 1. Create results directory ===
if not exist "%RESULTS_DIR%" mkdir "%RESULTS_DIR%"

REM === 2. Push QNN runtime files ===
echo [1/4] Pushing QNN runtime files...
"%ADB%" shell "mkdir -p %DEVICE_ROOT%"
"%ADB%" push "%QNN_SDK_ROOT%\bin\aarch64-android\qnn-net-run" "%DEVICE_ROOT%/qnn-net-run"
"%ADB%" push "%QNN_SDK_ROOT%\lib\aarch64-android\libQnnHtp.so" "%DEVICE_ROOT%/libQnnHtp.so"
"%ADB%" push "%QNN_SDK_ROOT%\lib\aarch64-android\libQnnSystem.so" "%DEVICE_ROOT%/libQnnSystem.so"
"%ADB%" push "%QNN_SDK_ROOT%\lib\aarch64-android\libQnnHtpPrepare.so" "%DEVICE_ROOT%/libQnnHtpPrepare.so"
"%ADB%" push "%QNN_SDK_ROOT%\lib\aarch64-android\libQnnHtpV73Stub.so" "%DEVICE_ROOT%/libQnnHtpV73Stub.so"
"%ADB%" push "%QNN_SDK_ROOT%\lib\hexagon-v73\unsigned\libQnnHtpV73Skel.so" "%DEVICE_ROOT%/libQnnHtpV73Skel.so"
"%ADB%" push "%ANDROID_NDK_ROOT%\toolchains\llvm\prebuilt\windows-x86_64\sysroot\usr\lib\aarch64-linux-android\libc++_shared.so" "%DEVICE_ROOT%/libc++_shared.so"
"%ADB%" shell "chmod 755 %DEVICE_ROOT%/qnn-net-run"
echo.

REM === 3. Run each variant ===
echo [2/4] Processing variants...
call :run_variant uint8
call :run_variant float32
echo.

REM === 4. Summary ===
echo [4/4] Summary
echo ============================================
echo Results directory: %RESULTS_DIR%
echo.
echo Key files to check per variant:
echo   - VARIANT.zero.log      (zero input run log)
echo   - VARIANT.pattern.log   (pattern input run log)
echo   - VARIANT\output_zero\Result_0\execution_metadata.yaml
echo   - VARIANT\output_pattern\Result_0\execution_metadata.yaml
echo.
echo The execution_metadata.yaml is the KEY output -
echo it shows runtime tensor dtype/scale/offset after HTP finalize.
echo This tells us whether custom_io QuantParam survived.
echo ============================================
goto :eof

REM === Subroutine: run_variant ===
:run_variant
set "V=%~1"
echo --- Variant: %V% ---

set "MODEL_LIB=%PROBE_ROOT%\build_%V%\libs\arm64-v8a\libmodel.so"
if not exist "%MODEL_LIB%" (
    echo   [SKIP] libmodel.so not found for %V% variant
    goto :eof
)

REM Create variant directories on device
"%ADB%" shell "mkdir -p %DEVICE_ROOT%/%V%/input_zero %DEVICE_ROOT%/%V%/input_pattern %DEVICE_ROOT%/%V%/output_zero %DEVICE_ROOT%/%V%/output_pattern"

REM Push libmodel.so
echo   Pushing libmodel.so...
"%ADB%" push "%MODEL_LIB%" "%DEVICE_ROOT%/%V%/libmodel.so"

REM Determine input files for this variant
if "%V%"=="uint8" (
    set "ZERO_INPUT=cache_key_zero_uint8.raw"
    set "PATTERN_INPUT=cache_key_pattern_uint8.raw"
)
if "%V%"=="float32" (
    set "ZERO_INPUT=cache_key_zero_float32.raw"
    set "PATTERN_INPUT=cache_key_one_float32.raw"
)
if "%V%"=="fixed16" (
    set "ZERO_INPUT=cache_key_zero_uint16.raw"
    set "PATTERN_INPUT=cache_key_pattern_uint16.raw"
)

REM Push input raw files
echo   Pushing input files...
"%ADB%" push "%PROBE_ROOT%\inputs\%ZERO_INPUT%" "%DEVICE_ROOT%/%V%/input_zero/cache_key_0.raw"
"%ADB%" push "%PROBE_ROOT%\inputs\%PATTERN_INPUT%" "%DEVICE_ROOT%/%V%/input_pattern/cache_key_0.raw"

REM Create input_list files on host and push
echo cache_key_0:=./%V%/input_zero/cache_key_0.raw> "%RESULTS_DIR%\input_zero_%V%.txt"
echo cache_key_0:=./%V%/input_pattern/cache_key_0.raw> "%RESULTS_DIR%\input_pattern_%V%.txt"

"%ADB%" push "%RESULTS_DIR%\input_zero_%V%.txt" "%DEVICE_ROOT%/%V%/input_zero.txt"
"%ADB%" push "%RESULTS_DIR%\input_pattern_%V%.txt" "%DEVICE_ROOT%/%V%/input_pattern.txt"

echo   Running %V% with ZERO input...
"%ADB%" shell "cd %DEVICE_ROOT% && export LD_LIBRARY_PATH=. && export ADSP_LIBRARY_PATH=. && ./qnn-net-run --model ./%V%/libmodel.so --backend ./libQnnHtp.so --input_list ./%V%/input_zero.txt --output_dir ./%V%/output_zero --log_level info" 2>&1 > "%RESULTS_DIR%\%V%.zero.log"
echo   Zero run exit code: %errorlevel%

echo   Running %V% with PATTERN input...
"%ADB%" shell "cd %DEVICE_ROOT% && export LD_LIBRARY_PATH=. && export ADSP_LIBRARY_PATH=. && ./qnn-net-run --model ./%V%/libmodel.so --backend ./libQnnHtp.so --input_list ./%V%/input_pattern.txt --output_dir ./%V%/output_pattern --log_level info" 2>&1 > "%RESULTS_DIR%\%V%.pattern.log"
echo   Pattern run exit code: %errorlevel%

echo   Pulling results for %V%...
if not exist "%RESULTS_DIR%\%V%\output_zero\Result_0" mkdir "%RESULTS_DIR%\%V%\output_zero\Result_0"
if not exist "%RESULTS_DIR%\%V%\output_pattern\Result_0" mkdir "%RESULTS_DIR%\%V%\output_pattern\Result_0"

"%ADB%" pull "%DEVICE_ROOT%/%V%/output_zero/Result_0/" "%RESULTS_DIR%\%V%\output_zero\Result_0\"
"%ADB%" pull "%DEVICE_ROOT%/%V%/output_pattern/Result_0/" "%RESULTS_DIR%\%V%\output_pattern\Result_0\"

echo   %V% complete.
goto :eof
