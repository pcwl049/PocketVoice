@echo off
setlocal

set "ROOT_DIR=D:\Project\STT"
set "BUILD_DIR=%ROOT_DIR%\build\pc-tests"
set "VS_DIR=D:\Program Files\VScode"
set "MSVC_VER=14.50.35717"
set "WIN_SDK=C:\Program Files (x86)\Windows Kits\10"
set "WIN_SDK_VER=10.0.26100.0"

set "CL_PATH=%VS_DIR%\VC\Tools\MSVC\%MSVC_VER%\bin\Hostx64\x64\cl.exe"
set "RC_PATH=%WIN_SDK%\bin\%WIN_SDK_VER%\x64\rc.exe"

if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"

set "INCLUDE=%VS_DIR%\VC\Tools\MSVC\%MSVC_VER%\include;%WIN_SDK%\Include\%WIN_SDK_VER%\ucrt;%WIN_SDK%\Include\%WIN_SDK_VER%\um;%WIN_SDK%\Include\%WIN_SDK_VER%\shared"
set "LIB=%VS_DIR%\VC\Tools\MSVC\%MSVC_VER%\lib\x64;%WIN_SDK%\Lib\%WIN_SDK_VER%\ucrt\x64;%WIN_SDK%\Lib\%WIN_SDK_VER%\um\x64"

"%RC_PATH%" /nologo /fo "%BUILD_DIR%\pc_resources_test.res" "%ROOT_DIR%\src\pc\pc_resources.rc"
if errorlevel 1 exit /b 1

"%CL_PATH%" /std:c++20 /EHsc /W3 /O2 /utf-8 ^
  /I"%ROOT_DIR%\src\pc" ^
  "%ROOT_DIR%\tests\pc\embedded_vad_model_test.cpp" ^
  "%ROOT_DIR%\src\pc\embedded_vad_model.cpp" ^
  "%BUILD_DIR%\pc_resources_test.res" ^
  /Fe"%BUILD_DIR%\embedded_vad_model_test.exe" >nul
if errorlevel 1 exit /b 1

"%BUILD_DIR%\embedded_vad_model_test.exe"
exit /b %ERRORLEVEL%
