@echo off
setlocal

call "%~dp0env.bat" || exit /b 1

set "VENV_DIR=%ROOT_DIR%\build\fireredasr2-aed-venv"
set "PYTHON_EXE=%VENV_DIR%\Scripts\python.exe"

if not exist "%PYTHON_EXE%" (
  echo Python environment not found: %PYTHON_EXE%
  echo Create it under build\ or set up another environment with funasr-onnx.
  exit /b 1
)

set "HF_HOME=%ROOT_DIR%\build\cache\huggingface"
set "HUGGINGFACE_HUB_CACHE=%HF_HOME%\hub"
set "MODELSCOPE_CACHE=%ROOT_DIR%\build\cache\modelscope"
set "TORCH_HOME=%ROOT_DIR%\build\cache\torch"
set "PIP_CACHE_DIR=%ROOT_DIR%\build\cache\pip"

"%PYTHON_EXE%" "%ROOT_DIR%\scripts\paraformer_contextual_eval.py" %*
exit /b %ERRORLEVEL%
