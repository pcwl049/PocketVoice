@echo off
setlocal enabledelayedexpansion

set "ROOT_DIR=%~dp0.."
set "EXPECTED=%ROOT_DIR%\test_audio\expected.tsv"
set "ARGS="

if not "%~1"=="" (
    set "FIRST=%~1"
    if not "!FIRST:~0,2!"=="--" (
        set "EXPECTED=%~1"
        shift
    )
)

:collect
if "%~1"=="" goto :run
set "ARGS=!ARGS! "%~1""
shift
goto :collect

:run
node "%ROOT_DIR%\scripts\android_pressure_suite.js" "%EXPECTED%" !ARGS!
exit /b %ERRORLEVEL%
