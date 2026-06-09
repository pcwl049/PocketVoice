@echo off
setlocal

set "ROOT_DIR=D:\Project\STT"
set "BUILD_DIR=%ROOT_DIR%\build\mobile-tests"
set "VS_DIR=D:\Program Files\VScode"
set "MSVC_VER=14.50.35717"
set "WIN_SDK=C:\Program Files (x86)\Windows Kits\10"
set "WIN_SDK_VER=10.0.26100.0"

set "CL_PATH=%VS_DIR%\VC\Tools\MSVC\%MSVC_VER%\bin\Hostx64\x64\cl.exe"

if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"

set "INCLUDE=%VS_DIR%\VC\Tools\MSVC\%MSVC_VER%\include;%WIN_SDK%\Include\%WIN_SDK_VER%\ucrt;%WIN_SDK%\Include\%WIN_SDK_VER%\um;%WIN_SDK%\Include\%WIN_SDK_VER%\shared"
set "LIB=%VS_DIR%\VC\Tools\MSVC\%MSVC_VER%\lib\x64;%WIN_SDK%\Lib\%WIN_SDK_VER%\ucrt\x64;%WIN_SDK%\Lib\%WIN_SDK_VER%\um\x64"

"%CL_PATH%" /std:c++20 /EHsc /W3 /O2 /utf-8 ^
  /I"%ROOT_DIR%\src\mobile\app\src\main\cpp" ^
  "%ROOT_DIR%\tests\mobile\runtime_state_test.cpp" ^
  "%ROOT_DIR%\src\mobile\app\src\main\cpp\runtime_state.cpp" ^
  /Fe"%BUILD_DIR%\runtime_state_test.exe" >nul
if errorlevel 1 exit /b 1

"%BUILD_DIR%\runtime_state_test.exe"
exit /b %ERRORLEVEL%
