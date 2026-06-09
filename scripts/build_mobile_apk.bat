@echo off
setlocal enabledelayedexpansion

set "USE_QNN=0"
if /I "%~1"=="--qnn" set "USE_QNN=1"

set BUILD_TOOLS=D:\Android\Sdk\build-tools\36.1.0
set PLATFORM=D:\Android\Sdk\platforms\android-34
set PROJECT_DIR=D:\Project\STT\src\mobile\app
set OUTPUT_DIR=D:\Project\STT\build\mobile-apk
set NDK_PATH=D:\Project\STT\third_party\android-ndk-r27c
set TOOLCHAIN=%NDK_PATH%\build\cmake\android.toolchain.cmake
set MAKE=%NDK_PATH%\prebuilt\windows-x86_64\bin\make.exe
set BUILD_DIR=%OUTPUT_DIR%\apk
set NATIVE_BUILD_DIR=%OUTPUT_DIR%\native-cpu
if not "%STT_MOBILE_NATIVE_BUILD_DIR%"=="" set "NATIVE_BUILD_DIR=%STT_MOBILE_NATIVE_BUILD_DIR%"
set FINAL_APK=%OUTPUT_DIR%\app-signed.apk
set "CMAKE_QNN_ARG=-DSTT_USE_QNN=OFF"
set "SHERPA_LIB_DIR=D:\Project\STT\third_party\lib\android\arm64-v8a"

if "%USE_QNN%"=="1" (
    set "CMAKE_QNN_ARG=-DSTT_USE_QNN=ON"
    set "SHERPA_LIB_DIR=D:\Project\STT\third_party\lib\android\arm64-v8a-qnn"
    if "%STT_MOBILE_NATIVE_BUILD_DIR%"=="" set "NATIVE_BUILD_DIR=%OUTPUT_DIR%\native-qnn"
    echo QNN build: ON
)

echo.
echo === [1/2] Build Native Library ===
cmake -S "%PROJECT_DIR%\src\main\cpp" -B "%NATIVE_BUILD_DIR%" -G "Unix Makefiles" ^
    "-DCMAKE_TOOLCHAIN_FILE=%TOOLCHAIN%" "-DCMAKE_MAKE_PROGRAM=%MAKE%" ^
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24 -DCMAKE_BUILD_TYPE=Release %CMAKE_QNN_ARG% 2>nul
if errorlevel 1 goto :error
%MAKE% -C "%NATIVE_BUILD_DIR%" -j4
if errorlevel 1 goto :error
copy /Y "%NATIVE_BUILD_DIR%\libstt_native.so" "%PROJECT_DIR%\src\main\jniLibs\arm64-v8a\" >nul

echo.
echo === [2/2] Build APK ===
del /q "%FINAL_APK%" 2>nul
rmdir /s /q "%BUILD_DIR%" 2>nul
mkdir "%BUILD_DIR%"
mkdir "%BUILD_DIR%\obj"
mkdir "%BUILD_DIR%\dex_out"
mkdir "%BUILD_DIR%\lib\arm64-v8a"
mkdir "%BUILD_DIR%\assets\ui"

echo  Resources...
%BUILD_TOOLS%\aapt2 compile --dir "%PROJECT_DIR%\src\main\res" -o "%BUILD_DIR%" >nul
if errorlevel 1 goto :error
set "COMPILED_RES="
for %%F in ("%BUILD_DIR%\values_*.arsc.flat") do set "COMPILED_RES=!COMPILED_RES! "%%~fF""
%BUILD_TOOLS%\aapt2 link -I "%PLATFORM%\android.jar" !COMPILED_RES! ^
    --manifest "%PROJECT_DIR%\src\main\AndroidManifest.xml" -o "%BUILD_DIR%\resources.ap_" --auto-add-overlay >nul
if errorlevel 1 goto :error

echo  Java...
javac --release 17 -d "%BUILD_DIR%\obj" -cp "%PLATFORM%\android.jar" "%PROJECT_DIR%\src\main\java\com\stt\mobile\MainActivity.java" "%PROJECT_DIR%\src\main\java\com\stt\mobile\SttForegroundService.java" >nul
if errorlevel 1 goto :error

echo  DEX...
call "%BUILD_TOOLS%\d8.bat" --release --output "%BUILD_DIR%\dex_out" "%BUILD_DIR%\obj\com\stt\mobile\*.class" >nul
if errorlevel 1 goto :error

echo  Native libs...
copy /Y "%SHERPA_LIB_DIR%\*.so" "%PROJECT_DIR%\src\main\jniLibs\arm64-v8a\" >nul
if errorlevel 1 goto :error
set "SENSEVOICE_QNN_LIBMODEL=D:\Project\STT\models\sensevoice\libmodel.so"
if "%USE_QNN%"=="1" if not "%STT_SENSEVOICE_QNN_LIBMODEL%"=="" set "SENSEVOICE_QNN_LIBMODEL=%STT_SENSEVOICE_QNN_LIBMODEL%"
if "%USE_QNN%"=="1" if exist "%SENSEVOICE_QNN_LIBMODEL%" (
    if /I "%STT_SKIP_QNN_LIBMODEL_REPAIR%"=="1" (
        copy /Y "%SENSEVOICE_QNN_LIBMODEL%" "%PROJECT_DIR%\src\main\jniLibs\arm64-v8a\libmodel.so" >nul
        if errorlevel 1 goto :error
    ) else (
        set "FIXED_SENSEVOICE_QNN_LIBMODEL=D:\Project\STT\build\qnn-model-fixed\libmodel.so"
        node "D:\Project\STT\scripts\repair_qnn_libmodel_elf.js" "%SENSEVOICE_QNN_LIBMODEL%" "!FIXED_SENSEVOICE_QNN_LIBMODEL!"
        if errorlevel 1 goto :error
        copy /Y "!FIXED_SENSEVOICE_QNN_LIBMODEL!" "%PROJECT_DIR%\src\main\jniLibs\arm64-v8a\libmodel.so" >nul
        if errorlevel 1 goto :error
    )
)
if "%USE_QNN%"=="1" (
    copy /Y "%NDK_PATH%\toolchains\llvm\prebuilt\windows-x86_64\sysroot\usr\lib\aarch64-linux-android\libc++_shared.so" "%PROJECT_DIR%\src\main\jniLibs\arm64-v8a\" >nul
    if errorlevel 1 goto :error
)
copy /Y "%PROJECT_DIR%\src\main\jniLibs\arm64-v8a\*.so" "%BUILD_DIR%\lib\arm64-v8a\" >nul
if errorlevel 1 goto :error
copy /Y "%BUILD_DIR%\resources.ap_" "%BUILD_DIR%\app.apk" >nul
if errorlevel 1 goto :error
copy /Y "%BUILD_DIR%\dex_out\classes.dex" "%BUILD_DIR%\" >nul
if errorlevel 1 goto :error
if exist "%PROJECT_DIR%\src\main\assets\ui" (
    copy /Y "%PROJECT_DIR%\src\main\assets\ui\*" "%BUILD_DIR%\assets\ui\" >nul
    if errorlevel 1 goto :error
)

pushd "%BUILD_DIR%"
%BUILD_TOOLS%\aapt add "app.apk" lib/arm64-v8a/libonnxruntime.so lib/arm64-v8a/libsherpa-onnx-c-api.so lib/arm64-v8a/libstt_native.so >nul
if errorlevel 1 (
    popd
    goto :error
)
if "%USE_QNN%"=="1" (
    for %%F in (%BUILD_DIR%\lib\arm64-v8a\libQnn*.so) do (
        %BUILD_TOOLS%\aapt add "app.apk" "lib/arm64-v8a/%%~nxF" >nul
        if errorlevel 1 (
            popd
            goto :error
        )
    )
    if exist "%BUILD_DIR%\lib\arm64-v8a\libmodel.so" (
        %BUILD_TOOLS%\aapt add "app.apk" "lib/arm64-v8a/libmodel.so" >nul
        if errorlevel 1 (
            popd
            goto :error
        )
    )
    if exist "%BUILD_DIR%\lib\arm64-v8a\libc++_shared.so" (
        %BUILD_TOOLS%\aapt add "app.apk" "lib/arm64-v8a/libc++_shared.so" >nul
        if errorlevel 1 (
            popd
            goto :error
        )
    )
)
%BUILD_TOOLS%\aapt add "app.apk" classes.dex >nul
if errorlevel 1 (
    popd
    goto :error
)
if exist "%BUILD_DIR%\assets\ui\index.html" (
    %BUILD_TOOLS%\aapt add "app.apk" assets/ui/index.html assets/ui/styles.css assets/ui/app.js >nul
    if errorlevel 1 (
        popd
        goto :error
    )
)
popd

%BUILD_TOOLS%\zipalign -v 4 "%BUILD_DIR%\app.apk" "%BUILD_DIR%\app-aligned.apk" >nul
if errorlevel 1 goto :error
set "KEYSTORE=%USERPROFILE%\.android\debug.keystore"
call "%BUILD_TOOLS%\apksigner.bat" sign --ks "%KEYSTORE%" --ks-pass pass:android --key-pass pass:android ^
    --out "%BUILD_DIR%\app-signed.apk" "%BUILD_DIR%\app-aligned.apk" >nul
if errorlevel 1 goto :error

copy /Y "%BUILD_DIR%\app-signed.apk" "%OUTPUT_DIR%\app-signed.apk" >nul
if errorlevel 1 goto :error
echo.
echo ========================================
echo  Build complete!
echo  Output: %OUTPUT_DIR%\app-signed.apk
echo ========================================
goto :end

:error
echo [ERROR] Build failed!
pause
exit /b 1
:end
