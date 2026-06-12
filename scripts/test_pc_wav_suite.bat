@echo off
setlocal

call "%~dp0env.bat"
set "EXPECTED=%ROOT_DIR%\test_audio\expected.tsv"

if not "%~1"=="" (
    set "EXPECTED=%~1"
)

node "%ROOT_DIR%\scripts\pc_wav_suite.js" "%EXPECTED%"
exit /b %ERRORLEVEL%
