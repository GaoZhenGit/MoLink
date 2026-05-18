$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
# 可选版本号：.\clean_build.ps1 v2026.05.18.1600
$verOverride = $args[0]
$cmakeVersionArg = if ($verOverride -and $verOverride -match '^v\d{4}') { "-DMOLINK_VERSION=$verOverride" } else { "" }

# 1. 停止 daemon（忽略错误，可能未运行）
Push-Location "$root\build"
try {
    .\molink.exe stop 2>$null
} catch {}
Pop-Location

# 2. 备份签名文件（避免重复授权）
$keyBackup = "$root\build\molink_key.bin"
$keyTemp = "$env:TEMP\molink_key.bin"
if (Test-Path $keyBackup) {
    Copy-Item $keyBackup $keyTemp -Force
    Write-Host "Key backed up" -ForegroundColor Yellow
}

# 3. 清理构建目录
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue "$root\build"

# 4. 工具链信息
Write-Host "--- Toolchain ---" -ForegroundColor Cyan
Write-Host "CMake : $(Get-Command cmake | Select-Object -ExpandProperty Source)"
Write-Host "Make  : $(Get-Command mingw32-make -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source)"
Write-Host "GCC   : $(Get-Command gcc | Select-Object -ExpandProperty Source)"
Write-Host "G++   : $(Get-Command g++ | Select-Object -ExpandProperty Source)"

# 5. 配置 CMake
New-Item -ItemType Directory -Force "$root\build" | Out-Null
Push-Location "$root\build"
cmake .. -G "MinGW Makefiles" $cmakeVersionArg

# 6. 编译
try { $nproc = (Get-WmiObject Win32_ComputerSystem).NumberOfLogicalProcessors } catch {}
if (-not $nproc -or $nproc -lt 1) {
    try { $nproc = [Environment]::ProcessorCount } catch {}
}
if (-not $nproc -or $nproc -lt 1) { $nproc = 4 }
Write-Host "Parallel jobs: $nproc" -ForegroundColor Cyan
cmake --build . --config Release -- -j $nproc --output-sync=line

Pop-Location

# 7. 恢复签名文件
if (Test-Path $keyTemp) {
    Copy-Item $keyTemp $keyBackup -Force
    Remove-Item $keyTemp -Force
    Write-Host "Key restored" -ForegroundColor Yellow
}

Write-Host "Done." -ForegroundColor Green
