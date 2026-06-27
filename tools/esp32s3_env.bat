@echo off
REM ESP32-S3 CAM_AI IDF toolchain environment
REM Usage: call tools\esp32s3_env.bat

cd /d "%~dp0.."

set "IDF_PATH=D:\Espressif\frameworks\esp-idf-v5.3.5"
set "IDF_TOOLS_PATH=D:\Espressif"
set "IDF_PYTHON_ENV_PATH=D:\Espressif\python_env\idf5.3_py3.11_env"
set "IDF_TARGET=esp32s3"
set "ESPPORT=COM7"
set "ESPBAUD=115200"
set "NODE_EXTRA_CA_CERTS="

call "%IDF_PATH%\export.bat" >nul 2>&1
if errorlevel 1 (
    echo [esp32s3_env] export.bat failed: %IDF_PATH%
    exit /b 1
)

exit /b 0
