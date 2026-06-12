@echo off
setlocal

call "%~dp0env.bat" || exit /b 1

set "VENV_DIR=%ROOT_DIR%\build\fireredasr2-aed-venv"
set "PYTHON_EXE=%VENV_DIR%\Scripts\python.exe"

if not exist "%PYTHON_EXE%" (
  echo Python environment not found: %PYTHON_EXE%
  exit /b 1
)

set "HF_HOME=G:\STTModels\cache\huggingface"
set "HUGGINGFACE_HUB_CACHE=%HF_HOME%\hub"
set "MODELSCOPE_CACHE=G:\STTModels\cache\modelscope"
set "TORCH_HOME=G:\STTModels\cache\torch"
set "PIP_CACHE_DIR=G:\STTModels\cache\pip"

"%PYTHON_EXE%" "%ROOT_DIR%\scripts\sherpa_onnx_eval.py" %*
exit /b %ERRORLEVEL%
