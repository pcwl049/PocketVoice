@echo off
setlocal

call "%~dp0env.bat"
set "STT_QNN_REFERENCE_MODEL=%ROOT_DIR%\build\qnn-model-lib-android\sensevoice-act16-fixed-prompt-expanded-preserve-layout-restrict\libs\arm64-v8a\libmodel.so"
set "STT_QNN_GRAPH_NAME=model"
set "STT_QNN_INPUT_LIST=x:=input0.float.raw"
set "STT_QNN_NATIVE_INPUT_TENSOR_NAMES=none"
set "STT_QNN_INPUT0=%ROOT_DIR%\build\test-results\android-qnn-raw-latest\input0.float.raw"
set "STT_QNN_INPUT0_LAYOUT=onnx"

echo ============================================================
echo   QNN fixed-prompt HTP reference gate
echo ============================================================
echo Optional: set STT_QNN_EXPECT_CONTAINS before running this script.

node "%ROOT_DIR%\scripts\qnn_net_run_reference_gate.js"
