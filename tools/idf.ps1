# ESP32-S3 CAM_AI — IDF 命令封装（供 Cursor Agent / 终端调用）
# 用法:
#   .\tools\idf.ps1 build
#   .\tools\idf.ps1 flash
#   .\tools\idf.ps1 monitor-capture -Seconds 8
#   .\tools\idf.ps1 monitor-log                     # 读取最近一次捕获日志

param(
    [Parameter(Position = 0)]
    [ValidateSet('env', 'build', 'flash', 'flash-monitor', 'monitor-capture', 'monitor-log', 'size', 'clean', 'ports')]
    [string]$Action = 'build',

    [string]$Port = 'COM7',
    [int]$Seconds = 8,
    [int]$Tail = 80
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path $PSScriptRoot -Parent
$LogDir = Join-Path $Root 'logs'
$CaptureLog = Join-Path $LogDir 'monitor_capture.log'
$EnvBat = Join-Path $PSScriptRoot 'esp32s3_env.bat'

function Invoke-Idf {
    param([string[]]$IdfArgs)
    $argLine = $IdfArgs -join ' '
    $cmd = "call `"$EnvBat`" && idf.py $argLine"
    cmd /c $cmd
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

function Invoke-IdfCapture {
    $py = 'D:\Espressif\python_env\idf5.3_py3.11_env\Scripts\python.exe'
    $script = Join-Path $PSScriptRoot 'capture_serial.py'
    if (-not (Test-Path $py)) { throw "IDF Python not found: $py" }
    New-Item -ItemType Directory -Force -Path $LogDir | Out-Null
    & $py $script --port $Port --baud 115200 --seconds $Seconds --out $CaptureLog
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    Write-Host "`n--- monitor_capture.log (last $Tail lines) ---"
    Get-Content $CaptureLog -Tail $Tail -ErrorAction SilentlyContinue
}

switch ($Action) {
    'env' {
        cmd /c "call `"$EnvBat`" && set IDF_PATH && set IDF_TARGET && set ESPPORT"
    }
    'build' { Invoke-Idf @('build') }
    'flash' { Invoke-Idf @('-p', $Port, 'flash') }
    'flash-monitor' {
        Write-Host 'Interactive monitor needs a real TTY. Use: idf.py -p COM7 monitor'
        Write-Host 'Or: .\tools\idf.ps1 monitor-capture'
        exit 1
    }
    'monitor-capture' { Invoke-IdfCapture }
    'monitor-log' {
        if (-not (Test-Path $CaptureLog)) {
            Write-Host "No capture yet. Run: .\tools\idf.ps1 monitor-capture"
            exit 1
        }
        Get-Content $CaptureLog -Tail $Tail
    }
    'size' { Invoke-Idf @('size') }
    'clean' { Invoke-Idf @('fullclean') }
    'ports' {
        [System.IO.Ports.SerialPort]::GetPortNames() | ForEach-Object { Write-Host $_ }
    }
}
