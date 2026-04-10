@echo off
setlocal enabledelayedexpansion

echo ============================================
echo 卸载旧版本并安装 molink-worker...
echo ============================================

cd /d "%~dp0"

set APK_PATH=app\build\outputs\apk\debug\app-debug.apk

:: 检查 APK 是否存在
if not exist "%APK_PATH%" (
    echo [错误] APK 文件不存在: %APK_PATH%
    echo 请先运行 build.bat 构建项目
    exit /b 1
)

:: 检查 ADB
adb devices >nul 2>nul
if %ERRORLEVEL% neq 0 (
    echo [错误] 未找到 ADB，请确保 Android SDK Platform Tools 已安装并配置到 PATH
    exit /b 1
)

echo.
echo [1] 检查设备连接...
adb devices
echo.

:: 卸载旧版本
echo [2] 卸载旧版本...
adb uninstall com.molink.worker >nul 2>nul
echo [完成] 旧版本已卸载或未安装

:: 安装新版本
echo.
echo [3] 安装新版本...
adb install -r "%APK_PATH%"

if %ERRORLEVEL% neq 0 (
    echo.
    echo [错误] 安装失败！
    exit /b 1
)

echo.
echo ============================================
echo 安装完成！
echo ============================================
echo 包名: com.molink.worker
echo.
echo 启动应用: start.bat
echo.
