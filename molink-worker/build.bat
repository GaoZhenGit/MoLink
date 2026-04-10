@echo off
setlocal enabledelayedexpansion

echo ============================================
echo 构建 molink-worker Android 项目...
echo ============================================

cd /d "%~dp0"

:: 检查 Android SDK
if not exist "D:\AndroidSdk" (
    echo [警告] 未找到 Android SDK，请检查路径 D:\AndroidSdk
)

:: 检查 Gradle
where gradle >nul 2>nul
if %ERRORLEVEL% neq 0 (
    echo [警告] 未找到 Gradle，尝试使用 gradlew...
)

:: 清理并构建
if exist "app\build" (
    echo [清理] 删除 build 目录...
    rd /s /q app\build 2>nul
)

echo.
echo [构建] 运行 Gradle 打包...
call gradlew.bat assembleDebug

if %ERRORLEVEL% neq 0 (
    echo.
    echo [错误] 构建失败！
    exit /b 1
)

echo.
echo ============================================
echo 构建完成！
echo ============================================
echo APK 文件: app\build\outputs\apk\debug\app-debug.apk
echo.
