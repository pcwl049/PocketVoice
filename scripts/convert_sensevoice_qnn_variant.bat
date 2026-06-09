@echo off
setlocal

set "ROOT_DIR=%~dp0.."
set "QAIRT_ROOT=G:\Program Files\qairt\2.45.0.260326"
set "PYTHON=%ROOT_DIR%\build\qairt-py310-venv\Scripts\python.exe"
set "ONNX_MODEL=%ROOT_DIR%\build\downloads\sensevoice-small-src\model-10-seconds-fixed-prompt-expanded.onnx"
set "INPUT_LIST=%ROOT_DIR%\build\qnn-calibration\fixed-prompt-expanded-input-list.txt"

if "%~1"=="" (
  echo Usage: scripts\convert_sensevoice_qnn_variant.bat ^<variant^>
  echo.
  echo Variants:
  echo   int8-preserve-layout-bias32
  echo   act16-preserve-layout-restrict
  echo   int8-preserve-layout-per-row-bias32
  exit /b 2
)

if /I "%~1"=="int8-preserve-layout-bias32" (
  set "VARIANT=sensevoice-int8-fixed-prompt-expanded-preserve-layout-bias32"
  set "EXTRA_ARGS=--act_bitwidth 8 --weights_bitwidth 8 --bias_bitwidth 32 --preserve_io layout logits"
) else if /I "%~1"=="act16-preserve-layout-restrict" (
  set "VARIANT=sensevoice-act16-fixed-prompt-expanded-preserve-layout-restrict"
  set EXTRA_ARGS=--act_bitwidth 16 --weights_bitwidth 8 --bias_bitwidth 32 --restrict_quantization_steps "-0x8000 0x7F7F" --preserve_io layout logits
) else if /I "%~1"=="int8-preserve-layout-per-row-bias32" (
  set "VARIANT=sensevoice-int8-fixed-prompt-expanded-preserve-layout-per-row-bias32"
  set "EXTRA_ARGS=--act_bitwidth 8 --weights_bitwidth 8 --bias_bitwidth 32 --use_per_row_quantization --enable_per_row_quantized_bias --preserve_io layout logits"
) else (
  echo [Error] Unknown variant: %~1
  exit /b 2
)

if not exist "%QAIRT_ROOT%" (
  echo [Error] QAIRT root not found: %QAIRT_ROOT%
  exit /b 1
)
if not exist "%PYTHON%" (
  echo [Error] Python venv not found: %PYTHON%
  exit /b 1
)
if not exist "%ONNX_MODEL%" (
  echo [Error] ONNX model not found: %ONNX_MODEL%
  exit /b 1
)
if not exist "%INPUT_LIST%" (
  echo [Error] Calibration input list not found: %INPUT_LIST%
  exit /b 1
)

set "OUT_DIR=%ROOT_DIR%\build\qnn-convert\%VARIANT%"
set "LOG_FILE=%ROOT_DIR%\build\qnn-convert\%VARIANT%.convert.log"

if not exist "%ROOT_DIR%\build\qnn-convert" mkdir "%ROOT_DIR%\build\qnn-convert"
if exist "%OUT_DIR%" rmdir /s /q "%OUT_DIR%"
mkdir "%OUT_DIR%"

set "PYTHONPATH=%QAIRT_ROOT%\lib\python"

echo Converting %VARIANT%...
"%PYTHON%" "%QAIRT_ROOT%\bin\x86_64-windows-msvc\qnn-onnx-converter" ^
  --input_network "%ONNX_MODEL%" ^
  --output_path "%OUT_DIR%\model.cpp" ^
  --input_list "%INPUT_LIST%" ^
  --out_node logits ^
  %EXTRA_ARGS% > "%LOG_FILE%" 2>&1

if errorlevel 1 (
  echo [Error] Conversion failed
  echo   See: %LOG_FILE%
  exit /b 1
)

echo Conversion complete:
echo   %OUT_DIR%
echo   %LOG_FILE%
