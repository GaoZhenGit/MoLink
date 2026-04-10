@echo off
setlocal enabledelayedexpansion

echo ============================================
echo 测试 molink-access 服务...
echo ============================================

set API_PORT=8080

echo 等待服务启动...
timeout /t 2 /nobreak >nul

echo.
echo [1] 检查端口 %API_PORT% 是否监听...
netstat -ano | findstr :%API_PORT% | findstr LISTENING >nul
if %ERRORLEVEL% neq 0 (
    echo   [失败] 端口 %API_PORT% 未监听
    exit /b 1
)
echo   [成功] 端口 %API_PORT% 正在监听

echo.
echo [2] 调用 /api/status...
curl.exe -s -X GET "http://localhost:%API_PORT%/api/status"
echo.

echo.
echo [3] 调用 /api/config...
curl.exe -s -X GET "http://localhost:%API_PORT%/api/config"
echo.

echo.
echo ============================================
echo 测试完成！
echo ============================================
