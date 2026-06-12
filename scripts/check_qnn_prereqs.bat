@echo off
setlocal

call "%~dp0env.bat"

node "%ROOT_DIR%\scripts\check_qnn_prereqs.js"
exit /b %ERRORLEVEL%
