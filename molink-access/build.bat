@echo off
setlocal enabledelayedexpansion

echo ============================================
echo 构建 molink-access (JDK 21 -> Java 8)
echo ============================================

cd /d "%~dp0"

:: 设置 JDK 21 路径
set JDK21_HOME=C:\Program Files\Java\jdk-21

:: 设置 PATH 使用 JDK 21
set PATH=%JDK21_HOME%\bin;%PATH%

echo 使用 JDK:
java -version 2>&1 | findstr version

if exist "target" (
    echo [清理] 删除 target 目录...
    rd /s /q target 2>nul
)

echo.
echo [构建] 运行 Maven 打包...
call mvn clean package -DskipTests

if %ERRORLEVEL% neq 0 (
    echo.
    echo [错误] 构建失败！
    exit /b 1
)

echo.
echo ============================================
echo 构建完成！
echo ============================================
echo JAR 文件: target\molink-access-1.0.0.jar
echo 运行时要求: JDK 8+
echo.
