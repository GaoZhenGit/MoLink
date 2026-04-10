@echo off
setlocal enabledelayedexpansion

echo ============================================
echo 停止 molink-worker...
echo ============================================

cd /d "%~dp0"

set PACKAGE=com.molink.worker

echo 停止应用...
adb shell am force-stop %PACKAGE%

echo.
echo 应用已停止
echo.
