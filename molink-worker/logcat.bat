@echo off
setlocal enabledelayedexpansion

echo ============================================
echo 查看 molink-worker 日志...
echo ============================================
echo 按 Ctrl+C 停止查看
echo.

cd /d "%~dp0"

set PACKAGE=com.molink.worker

:: 使用 logcat 查看应用日志
adb logcat -c
adb logcat | findstr /I /C:"molink" /C:"Socks5" /C:"MoLink" /C:"AndroidRuntime"
