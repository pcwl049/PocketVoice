@echo off
setlocal

set "ROOT_DIR=D:\Project\STT"

node "%ROOT_DIR%\scripts\check_qnn_prereqs.js"
exit /b %ERRORLEVEL%
