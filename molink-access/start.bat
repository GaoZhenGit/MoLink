@echo off
setlocal enabledelayedexpansion

echo ============================================
echo 启动 molink-access...
echo ============================================

cd /d "%~dp0"

:: 先停止旧进程
call stop.bat

:: 检查 JAR 文件是否存在
if not exist "target\molink-access-1.0.0.jar" (
    echo [错误] JAR 文件不存在，请先运行 build.bat
    exit /b 1
)

:: 配置参数
set LOCAL_PORT=1080
set REMOTE_PORT=1080
set API_PORT=8080

echo 配置:
echo   本地端口: %LOCAL_PORT%
echo   远端端口: %REMOTE_PORT%
echo   API 端口: %API_PORT%
echo.

:: 启动应用（新窗口）
start "molink-access" cmd /c "java -Dmolink.local.port=%LOCAL_PORT% -Dmolink.remote.port=%REMOTE_PORT% -Dmolink.api.port=%API_PORT% -jar target\molink-access-1.0.0.jar"

echo 等待服务启动...
timeout /t 3 /nobreak >nul

:: 检查是否启动成功
netstat -ano | findstr :%API_PORT% | findstr LISTENING >nul
if %ERRORLEVEL% neq 0 (
    echo [警告] 端口 %API_PORT% 未监听，服务可能启动失败
    exit /b 1
)

echo.
echo ============================================
echo molink-access 已启动
echo ============================================
echo 状态 API: http://localhost:%API_PORT%/api/status
echo 配置 API: http://localhost:%API_PORT%/api/config
echo.
echo 停止服务: stop.bat
echo.
