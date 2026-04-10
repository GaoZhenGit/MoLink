@echo off
setlocal enabledelayedexpansion

echo ============================================
echo 启动 molink-worker...
echo ============================================

cd /d "%~dp0"

:: 检查包名
set PACKAGE=com.molink.worker
set ACTIVITY=com.molink.worker.MainActivity

:: 停止旧进程
echo [1] 停止旧进程...
adb shell am force-stop %PACKAGE%

:: 启动应用
echo.
echo [2] 启动应用...
adb shell am start -n %ACTIVITY%

echo.
echo ============================================
echo 应用已启动
echo ============================================
echo.
echo 查看日志: logcat.bat
echo 停止应用: stop.bat
echo.
