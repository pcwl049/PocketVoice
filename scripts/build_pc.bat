@echo off
setlocal enabledelayedexpansion

set "PAUSE_ON_EXIT=0"
if /I "%~1"=="--pause" set "PAUSE_ON_EXIT=1"

echo ============================================================
echo   PocketVoice - PC Build Script (Direct MSVC)
echo ============================================================
echo.

set "ROOT_DIR=D:\Project\STT"
set "BUILD_DIR=%ROOT_DIR%\build\pc"
set "SHERPA_DIR=%ROOT_DIR%\third_party\sherpa-onnx-v1.12.39-win-x64-shared-MD-Release"
set "VS_DIR=D:\Program Files\VScode"
set "MSVC_VER=14.50.35717"
set "WIN_SDK=C:\Program Files (x86)\Windows Kits\10"
set "WIN_SDK_VER=10.0.26100.0"
set "WEBVIEW2_PACKAGES=%USERPROFILE%\.nuget\packages\microsoft.web.webview2"

set "CL_PATH=%VS_DIR%\VC\Tools\MSVC\%MSVC_VER%\bin\Hostx64\x64\cl.exe"
set "LINK_PATH=%VS_DIR%\VC\Tools\MSVC\%MSVC_VER%\bin\Hostx64\x64\link.exe"
set "RC_PATH=%WIN_SDK%\bin\%WIN_SDK_VER%\x64\rc.exe"
set "WEBVIEW2_DIR="

if exist "%WEBVIEW2_PACKAGES%" (
  for /f "delims=" %%D in ('dir /b /ad /o-n "%WEBVIEW2_PACKAGES%"') do (
    if not defined WEBVIEW2_DIR set "WEBVIEW2_DIR=%WEBVIEW2_PACKAGES%\%%D"
  )
)

if not defined WEBVIEW2_DIR (
  echo [Error] Microsoft.Web.WebView2 NuGet package was not found.
  echo   Run: dotnet add build\webview2-sdk-probe package Microsoft.Web.WebView2
  goto :error
)

if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"
if not exist "%BUILD_DIR%\obj" mkdir "%BUILD_DIR%\obj"

set "INCLUDE=%WEBVIEW2_DIR%\build\native\include;%ROOT_DIR%\build\qnn-android\headers;%SHERPA_DIR%\include;%SHERPA_DIR%\include\sherpa-onnx\c-api;%VS_DIR%\VC\Tools\MSVC\%MSVC_VER%\include;%WIN_SDK%\Include\%WIN_SDK_VER%\ucrt;%WIN_SDK%\Include\%WIN_SDK_VER%\um;%WIN_SDK%\Include\%WIN_SDK_VER%\shared;%WIN_SDK%\Include\%WIN_SDK_VER%\winrt"

set "LIB=%WEBVIEW2_DIR%\build\native\x64;%SHERPA_DIR%\lib;%VS_DIR%\VC\Tools\MSVC\%MSVC_VER%\lib\x64;%WIN_SDK%\Lib\%WIN_SDK_VER%\ucrt\x64;%WIN_SDK%\Lib\%WIN_SDK_VER%\um\x64"

set CXXFLAGS=/std:c++20 /EHsc /W3 /O2 /utf-8 /c

echo [1/5] Compiling common files...

echo   - vad.cpp
"%CL_PATH%" %CXXFLAGS% /I"%ROOT_DIR%\src\common" /I"%ROOT_DIR%\src\pc" /I"%ROOT_DIR%\src\pc\audio\firered_frontend" "%ROOT_DIR%\src\pc\audio\vad.cpp" /Fo"%BUILD_DIR%\obj\vad.obj"
if errorlevel 1 goto :error

echo   - firered_vad_ort.cpp
"%CL_PATH%" %CXXFLAGS% /I"%ROOT_DIR%\src\common" /I"%ROOT_DIR%\src\pc" /I"%ROOT_DIR%\src\pc\audio\firered_frontend" "%ROOT_DIR%\src\pc\audio\firered_vad_ort.cpp" /Fo"%BUILD_DIR%\obj\firered_vad_ort.obj"
if errorlevel 1 goto :error

echo   - firered_frontend\fft.cpp
"%CL_PATH%" %CXXFLAGS% /I"%ROOT_DIR%\src\pc\audio\firered_frontend" "%ROOT_DIR%\src\pc\audio\firered_frontend\fft.cpp" /Fo"%BUILD_DIR%\obj\firered_frontend_fft.obj"
if errorlevel 1 goto :error

echo   - buffer.cpp
"%CL_PATH%" %CXXFLAGS% /I"%ROOT_DIR%\src\common" /I"%ROOT_DIR%\src\pc" "%ROOT_DIR%\src\pc\audio\buffer.cpp" /Fo"%BUILD_DIR%\obj\buffer.obj"
if errorlevel 1 goto :error

echo   - pre_roll_buffer.cpp
"%CL_PATH%" %CXXFLAGS% /I"%ROOT_DIR%\src\common" /I"%ROOT_DIR%\src\pc" "%ROOT_DIR%\src\pc\audio\pre_roll_buffer.cpp" /Fo"%BUILD_DIR%\obj\pre_roll_buffer.obj"
if errorlevel 1 goto :error

echo   - sample_converter.cpp
"%CL_PATH%" %CXXFLAGS% /I"%ROOT_DIR%\src\common" /I"%ROOT_DIR%\src\pc" "%ROOT_DIR%\src\pc\audio\sample_converter.cpp" /Fo"%BUILD_DIR%\obj\sample_converter.obj"
if errorlevel 1 goto :error

echo   - wasapi_capture.cpp
"%CL_PATH%" %CXXFLAGS% /I"%ROOT_DIR%\src\common" /I"%ROOT_DIR%\src\pc" "%ROOT_DIR%\src\pc\audio\wasapi_capture.cpp" /Fo"%BUILD_DIR%\obj\wasapi_capture.obj"
if errorlevel 1 goto :error

echo   - sender.cpp
"%CL_PATH%" %CXXFLAGS% /I"%ROOT_DIR%\src\common" /I"%ROOT_DIR%\src\pc" "%ROOT_DIR%\src\pc\osc\sender.cpp" /Fo"%BUILD_DIR%\obj\sender.obj"
if errorlevel 1 goto :error

echo   - client.cpp
"%CL_PATH%" %CXXFLAGS% /I"%ROOT_DIR%\src\common" /I"%ROOT_DIR%\src\pc" "%ROOT_DIR%\src\pc\network\client.cpp" /Fo"%BUILD_DIR%\obj\client.obj"
if errorlevel 1 goto :error

echo   - chatbox_formatter.cpp
"%CL_PATH%" %CXXFLAGS% /I"%ROOT_DIR%\src\common" /I"%ROOT_DIR%\src\pc" "%ROOT_DIR%\src\pc\chatbox_formatter.cpp" /Fo"%BUILD_DIR%\obj\chatbox_formatter.obj"
if errorlevel 1 goto :error

echo   - chatbox_queue.cpp
"%CL_PATH%" %CXXFLAGS% /I"%ROOT_DIR%\src\common" /I"%ROOT_DIR%\src\pc" "%ROOT_DIR%\src\pc\chatbox_queue.cpp" /Fo"%BUILD_DIR%\obj\chatbox_queue.obj"
if errorlevel 1 goto :error

echo   - pc_app_controller.cpp
"%CL_PATH%" %CXXFLAGS% /I"%ROOT_DIR%\src\common" /I"%ROOT_DIR%\src\pc" "%ROOT_DIR%\src\pc\pc_app_controller.cpp" /Fo"%BUILD_DIR%\obj\pc_app_controller.obj"
if errorlevel 1 goto :error

echo   - pc_runtime.cpp
"%CL_PATH%" %CXXFLAGS% /I"%ROOT_DIR%\src\common" /I"%ROOT_DIR%\src\pc" "%ROOT_DIR%\src\pc\pc_runtime.cpp" /Fo"%BUILD_DIR%\obj\pc_runtime.obj"
if errorlevel 1 goto :error

echo   - pc_logger.cpp
"%CL_PATH%" %CXXFLAGS% /I"%ROOT_DIR%\src\common" /I"%ROOT_DIR%\src\pc" "%ROOT_DIR%\src\pc\pc_logger.cpp" /Fo"%BUILD_DIR%\obj\pc_logger.obj"
if errorlevel 1 goto :error

echo   - pc_status_json.cpp
"%CL_PATH%" %CXXFLAGS% /I"%ROOT_DIR%\src\common" /I"%ROOT_DIR%\src\pc" "%ROOT_DIR%\src\pc\pc_status_json.cpp" /Fo"%BUILD_DIR%\obj\pc_status_json.obj"
if errorlevel 1 goto :error

echo   - pc_status_page.cpp
"%CL_PATH%" %CXXFLAGS% /I"%ROOT_DIR%\src\common" /I"%ROOT_DIR%\src\pc" "%ROOT_DIR%\src\pc\pc_status_page.cpp" /Fo"%BUILD_DIR%\obj\pc_status_page.obj"
if errorlevel 1 goto :error

echo   - status_http_server.cpp
"%CL_PATH%" %CXXFLAGS% /I"%ROOT_DIR%\src\common" /I"%ROOT_DIR%\src\pc" "%ROOT_DIR%\src\pc\status_http_server.cpp" /Fo"%BUILD_DIR%\obj\status_http_server.obj"
if errorlevel 1 goto :error

echo   - adb_bridge.cpp
"%CL_PATH%" %CXXFLAGS% /I"%ROOT_DIR%\src\common" /I"%ROOT_DIR%\src\pc" "%ROOT_DIR%\src\pc\adb_bridge.cpp" /Fo"%BUILD_DIR%\obj\adb_bridge.obj"
if errorlevel 1 goto :error

echo   - pc_webview_window.cpp
"%CL_PATH%" %CXXFLAGS% /I"%ROOT_DIR%\src\common" /I"%ROOT_DIR%\src\pc" "%ROOT_DIR%\src\pc\pc_webview_window.cpp" /Fo"%BUILD_DIR%\obj\pc_webview_window.obj"
if errorlevel 1 goto :error

echo   - embedded_vad_model.cpp
"%CL_PATH%" %CXXFLAGS% /I"%ROOT_DIR%\src\common" /I"%ROOT_DIR%\src\pc" "%ROOT_DIR%\src\pc\embedded_vad_model.cpp" /Fo"%BUILD_DIR%\obj\embedded_vad_model.obj"
if errorlevel 1 goto :error

echo   - main.cpp
"%CL_PATH%" %CXXFLAGS% /I"%ROOT_DIR%\src\common" /I"%ROOT_DIR%\src\pc" "%ROOT_DIR%\src\pc\main.cpp" /Fo"%BUILD_DIR%\obj\main.obj"
if errorlevel 1 goto :error

echo   - pc_resources.rc
"%RC_PATH%" /nologo /i "%ROOT_DIR%\src\pc" /fo "%BUILD_DIR%\obj\pc_resources.res" "%ROOT_DIR%\src\pc\pc_resources.rc"
if errorlevel 1 goto :error

echo.
echo [2/5] Linking...
"%LINK_PATH%" "%BUILD_DIR%\obj\*.obj" "%BUILD_DIR%\obj\pc_resources.res" /LIBPATH:"%SHERPA_DIR%\lib" /LIBPATH:"%WEBVIEW2_DIR%\build\native\x64" sherpa-onnx-c-api.lib onnxruntime.lib WebView2LoaderStatic.lib version.lib advapi32.lib ws2_32.lib user32.lib ole32.lib avrt.lib mmdevapi.lib propsys.lib /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup /OUT:"%BUILD_DIR%\stt_pc.exe"
if errorlevel 1 goto :error

echo.
echo [3/5] Copying DLLs...
copy /Y "%SHERPA_DIR%\bin\*.dll" "%BUILD_DIR%\" >nul

echo.
echo [4/5] Copying config...
copy /Y "%ROOT_DIR%\config.json" "%BUILD_DIR%\" >nul
if not exist "%BUILD_DIR%\models\fireredvad" mkdir "%BUILD_DIR%\models\fireredvad"
copy /Y "%ROOT_DIR%\models\fireredvad\fireredvad_stream_vad_with_cache.onnx" "%BUILD_DIR%\models\fireredvad\" >nul
copy /Y "%ROOT_DIR%\models\fireredvad\cmvn.ark" "%BUILD_DIR%\models\fireredvad\" >nul

echo.
echo [5/5] FireRedVAD model copied; Silero fallback embedded in stt_pc.exe

echo.
echo ============================================================
echo   Build complete!
echo   Output: %BUILD_DIR%\stt_pc.exe
echo ============================================================
goto :end

:error
echo.
echo [Error] Build failed!
if "%PAUSE_ON_EXIT%"=="1" pause
exit /b 1

:end
if "%PAUSE_ON_EXIT%"=="1" pause
