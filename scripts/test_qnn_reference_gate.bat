@echo off
setlocal

set "ROOT_DIR=D:\Project\STT"

echo ============================================================
echo   QNN qnn-net-run reference gate
echo ============================================================
echo.

node "%ROOT_DIR%\scripts\qnn_net_run_reference_gate.js" %*

