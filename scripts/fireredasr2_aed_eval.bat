@echo off
setlocal

call "%~dp0env.bat"

if "%HF_HOME%"=="" set "HF_HOME=%ROOT_DIR%\build\cache\huggingface"
if "%HUGGINGFACE_HUB_CACHE%"=="" set "HUGGINGFACE_HUB_CACHE=%HF_HOME%\hub"
if "%MODELSCOPE_CACHE%"=="" set "MODELSCOPE_CACHE=%ROOT_DIR%\build\cache\modelscope"
if "%TORCH_HOME%"=="" set "TORCH_HOME=%ROOT_DIR%\build\cache\torch"
if "%PIP_CACHE_DIR%"=="" set "PIP_CACHE_DIR=%ROOT_DIR%\build\cache\pip"

set "PYTHON=%FIREREDASR2_PYTHON%"
if "%PYTHON%"=="" set "PYTHON=python"

"%PYTHON%" "%ROOT_DIR%\scripts\fireredasr2_aed_eval.py" %*
exit /b %ERRORLEVEL%
