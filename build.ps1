$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path

$ver = "v" + (Get-Date -Format "yyyy.MM.dd.HHmm")
$parts = $ver.Substring(1).Split('.')
$buildTime = Get-Date -Year $parts[0] -Month $parts[1] -Day $parts[2] -Hour $parts[3].Substring(0,2) -Minute $parts[3].Substring(2,2) -Second 0 -Millisecond 0
$epoch = Get-Date -Year 2025 -Month 1 -Day 1 -Hour 0 -Minute 0 -Second 0 -Millisecond 0
$verCode = [int]($buildTime - $epoch).TotalMinutes

Write-Host "========================================" -ForegroundColor Cyan
Write-Host " MoLink Build - $ver (code=$verCode)" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan

Write-Host "`n[1/3] Building access..." -ForegroundColor Yellow
Push-Location "$root\molink-access"
& .\clean_build.ps1 $ver
Pop-Location
Write-Host "  Access: $( & "$root\molink-access\build\molink.exe" -v )" -ForegroundColor Green

Write-Host "[2/3] Building worker..." -ForegroundColor Yellow
Push-Location "$root\molink-worker"
$gradleCmd = if (Test-Path .\gradlew.bat) { ".\gradlew.bat" } else { "gradle" }
$buildResult = & $gradleCmd clean assembleDebug "-PversionName=$ver" "-PversionCode=$verCode" 2>&1
if ($LASTEXITCODE -ne 0) {
    Write-Host "  Worker build failed!" -ForegroundColor Red
    Write-Host $buildResult
} else {
    Write-Host "  Worker: $ver" -ForegroundColor Green
}

$apkDir = "$root\molink-worker\app\build\outputs\apk\debug"
$apkFile = Get-ChildItem -Path $apkDir -Filter "*.apk" -ErrorAction SilentlyContinue | Select-Object -First 1
Pop-Location

Write-Host "[3/3] Collecting artifacts..." -ForegroundColor Yellow
$outDir = "$root\build"
if (Test-Path $outDir) { Remove-Item -Recurse -Force $outDir }
New-Item -ItemType Directory -Force $outDir | Out-Null
Copy-Item "$root\molink-access\build\molink.exe" "$outDir\molink.exe" -Force
if ($apkFile) {
    Copy-Item $apkFile.FullName "$outDir\molink-worker.apk" -Force
} else {
    Write-Host "  WARNING: Worker APK not found, skipping" -ForegroundColor Yellow
}
Set-Content "$outDir\version.txt" -Value $ver

Write-Host "Done." -ForegroundColor Yellow
Write-Host ""
Write-Host "  $outDir\molink.exe" -ForegroundColor Green
if ($apkFile) {
    Write-Host "  $outDir\molink-worker.apk" -ForegroundColor Green
}
Write-Host "  Version: $ver (code=$verCode)" -ForegroundColor Green
