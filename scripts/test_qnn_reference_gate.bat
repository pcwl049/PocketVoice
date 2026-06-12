@echo off
setlocal

call "%~dp0env.bat"

echo ============================================================
echo   QNN qnn-net-run reference gate
echo ============================================================
echo.

node "%ROOT_DIR%\scripts\qnn_net_run_reference_gate.js" %*

