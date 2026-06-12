@echo off
setlocal

call "%~dp0env.bat"
set "BUILD_DIR=%ROOT_DIR%\build\pc-tests"

set "CL_PATH=%VS_DIR%\VC\Tools\MSVC\%MSVC_VER%\bin\Hostx64\x64\cl.exe"

if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"

set "INCLUDE=%VS_DIR%\VC\Tools\MSVC\%MSVC_VER%\include;%WIN_SDK%\Include\%WIN_SDK_VER%\ucrt;%WIN_SDK%\Include\%WIN_SDK_VER%\um;%WIN_SDK%\Include\%WIN_SDK_VER%\shared"
set "LIB=%VS_DIR%\VC\Tools\MSVC\%MSVC_VER%\lib\x64;%WIN_SDK%\Lib\%WIN_SDK_VER%\ucrt\x64;%WIN_SDK%\Lib\%WIN_SDK_VER%\um\x64"

"%CL_PATH%" /std:c++20 /EHsc /W3 /O2 /utf-8 ^
  /I"%ROOT_DIR%\src\pc" ^
  "%ROOT_DIR%\tests\pc\chatbox_queue_test.cpp" ^
  "%ROOT_DIR%\src\pc\chatbox_queue.cpp" ^
  /Fe"%BUILD_DIR%\chatbox_queue_test.exe" >nul
if errorlevel 1 exit /b 1

"%BUILD_DIR%\chatbox_queue_test.exe"
exit /b %ERRORLEVEL%
