@echo off
REM 快捷入口: tools\idf.bat build | flash | monitor-capture | monitor-log
setlocal
set "ACTION=%~1"
if "%ACTION%"=="" set "ACTION=build"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0idf.ps1" %*
