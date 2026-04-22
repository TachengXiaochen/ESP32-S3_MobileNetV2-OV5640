@echo off
REM ESP32-S3 CAM AI 项目编译准备脚本
REM 此脚本用于修复常见的编译环境问题

echo ========================================
echo ESP32-S3 CAM AI - 环境准备
echo ========================================
echo.

REM 1. 检查ESP-IDF版本
echo [1/4] 检查ESP-IDF版本...
call idf.py --version > version_check.txt 2>&1
findstr /C:"v5.3" version_check.txt >nul
if %ERRORLEVEL% EQU 0 (
    echo     ✓ ESP-IDF版本符合要求 (>= v5.3)
) else (
    findstr /C:"v5.4" version_check.txt >nul
    if %ERRORLEVEL% EQU 0 (
        echo     ✓ ESP-IDF版本符合要求 (>= v5.3)
    ) else (
        findstr /C:"v5.5" version_check.txt >nul
        if %ERRORLEVEL% EQU 0 (
            echo     ✓ ESP-IDF版本符合要求 (>= v5.3)
        ) else (
            echo     ✗ ESP-IDF版本不符合要求！
            echo.
            echo     当前版本不支持esp-dl v3.x
            echo     需要: ESP-IDF >= 5.3.0
            echo.
            echo     请查看 UPGRADE_ESP_IDF.md 了解如何升级
            echo.
            del version_check.txt
            pause
            exit /b 1
        )
    )
)
del version_check.txt
echo.

REM 2. 修复Git所有权警告
echo [2/4] 修复Git所有权警告...
git config --global --add safe.directory D:/Espressif/frameworks/esp-idf-v5.1.2 >nul 2>&1
git config --global --add safe.directory D:/Espressif/frameworks/esp-idf-v5.3 >nul 2>&1
git config --global --add safe.directory D:/Espressif/frameworks/esp-idf-v5.4 >nul 2>&1
git config --global --add safe.directory D:/Espressif/frameworks/esp-idf-v5.5 >nul 2>&1
echo     ✓ Git安全目录已添加
echo.

REM 3. 清理构建目录
echo [3/4] 清理旧的构建文件...
if exist build (
    rmdir /s /q build
    echo     ✓ 构建目录已清理
) else (
    echo     ℹ 构建目录不存在，跳过
)
echo.

REM 4. 提示下一步操作
echo [4/4] 准备完成！
echo.
echo 现在可以执行以下命令进行编译：
echo   idf.py set-target esp32s3
echo   idf.py build
echo   idf.py flash monitor
echo.
echo 或者使用VSCode的ESP-IDF扩展直接编译
echo.
pause