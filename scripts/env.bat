@echo off

set "SCRIPT_DIR=%~dp0"
for %%I in ("%SCRIPT_DIR%..") do set "DETECTED_ROOT=%%~fI"

if "%ROOT_DIR%"=="" set "ROOT_DIR=%DETECTED_ROOT%"
if "%STT_ROOT%"=="" set "STT_ROOT=%ROOT_DIR%"

if "%ANDROID_SDK_ROOT%"=="" if not "%ANDROID_HOME%"=="" set "ANDROID_SDK_ROOT=%ANDROID_HOME%"
if "%ANDROID_HOME%"=="" if not "%ANDROID_SDK_ROOT%"=="" set "ANDROID_HOME=%ANDROID_SDK_ROOT%"

if "%ADB%"=="" if not "%ANDROID_SDK_ROOT%"=="" if exist "%ANDROID_SDK_ROOT%\platform-tools\adb.exe" set "ADB=%ANDROID_SDK_ROOT%\platform-tools\adb.exe"
if "%ADB%"=="" if exist "%ROOT_DIR%\tools\platform-tools\adb.exe" set "ADB=%ROOT_DIR%\tools\platform-tools\adb.exe"
if "%ADB%"=="" if exist "%ROOT_DIR%\tools\adb.exe" set "ADB=%ROOT_DIR%\tools\adb.exe"
if "%ADB%"=="" set "ADB=adb"

if "%ANDROID_NDK_ROOT%"=="" if not "%NDK_PATH%"=="" set "ANDROID_NDK_ROOT=%NDK_PATH%"
if "%ANDROID_NDK_ROOT%"=="" if exist "%ROOT_DIR%\third_party\android-ndk-r27c" set "ANDROID_NDK_ROOT=%ROOT_DIR%\third_party\android-ndk-r27c"
if "%NDK_PATH%"=="" set "NDK_PATH=%ANDROID_NDK_ROOT%"

if "%ANDROID_BUILD_TOOLS%"=="" if not "%ANDROID_SDK_ROOT%"=="" if exist "%ANDROID_SDK_ROOT%\build-tools" (
  for /f "delims=" %%D in ('dir /b /ad /o-n "%ANDROID_SDK_ROOT%\build-tools"') do (
    if not defined ANDROID_BUILD_TOOLS set "ANDROID_BUILD_TOOLS=%ANDROID_SDK_ROOT%\build-tools\%%D"
  )
)
if "%BUILD_TOOLS%"=="" set "BUILD_TOOLS=%ANDROID_BUILD_TOOLS%"

if "%ANDROID_PLATFORM%"=="" if not "%ANDROID_SDK_ROOT%"=="" if exist "%ANDROID_SDK_ROOT%\platforms" (
  for /f "delims=" %%D in ('dir /b /ad /o-n "%ANDROID_SDK_ROOT%\platforms\android-*"') do (
    if not defined ANDROID_PLATFORM set "ANDROID_PLATFORM=%ANDROID_SDK_ROOT%\platforms\%%D"
  )
)
if "%PLATFORM%"=="" set "PLATFORM=%ANDROID_PLATFORM%"

if "%QNN_SDK%"=="" if not "%QNN_SDK_ROOT%"=="" set "QNN_SDK=%QNN_SDK_ROOT%"
if "%QNN_SDK_ROOT%"=="" if not "%QNN_SDK%"=="" set "QNN_SDK_ROOT=%QNN_SDK%"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if "%VS_DIR%"=="" (
  if exist "%VSWHERE%" (
    for /f "usebackq delims=" %%D in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
      if not defined VS_DIR set "VS_DIR=%%D"
    )
  )
)

if "%MSVC_VER%"=="" if not "%VS_DIR%"=="" if exist "%VS_DIR%\VC\Tools\MSVC" (
  for /f "delims=" %%D in ('dir /b /ad /o-n "%VS_DIR%\VC\Tools\MSVC"') do (
    if not defined MSVC_VER set "MSVC_VER=%%D"
  )
)

if "%WIN_SDK%"=="" set "WIN_SDK=%ProgramFiles(x86)%\Windows Kits\10"
if "%WIN_SDK_VER%"=="" if exist "%WIN_SDK%\Include" (
  for /f "delims=" %%D in ('dir /b /ad /o-n "%WIN_SDK%\Include\10.*"') do (
    if not defined WIN_SDK_VER set "WIN_SDK_VER=%%D"
  )
)
