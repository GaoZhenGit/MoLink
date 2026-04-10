@echo off
setlocal enabledelayedexpansion

echo ============================================
echo 停止 molink-access...
echo ============================================

:: 停止 molink-access（通过窗口标题）
for /f "tokens=2" %%a in ('tasklist /FI "WINDOWTITLE eq molink-access*" /FO LIST 2^|findstr "PID:"') do (
    echo   停止 access 窗口进程 PID: %%a
    taskkill /F /PID %%a >nul 2>nul
)

:: 停止 molink-access（通过命令行参数查找）
for /f "tokens=2" %%a in ('tasklist /FI "IMAGENAME eq java.exe" /FO LIST 2^|findstr "PID:"') do (
    for /f "delims=" %%b in ('wmic process where "ProcessId=%%a" get CommandLine /value 2^|findstr "molink-access"') do (
        echo   停止 molink-access 进程 PID: %%a
        taskkill /F /PID %%a >nul 2>nul
    )
)

:: 停止占用端口的进程（备用方案）
for /f "tokens=5" %%a in ('netstat -ano ^| findstr :8080 ^| findstr LISTENING') do (
    echo   停止端口 8080 进程 PID: %%a
    taskkill /F /PID %%a >nul 2>nul
)
for /f "tokens=5" %%a in ('netstat -ano ^| findstr :1080 ^| findstr LISTENING') do (
    echo   停止端口 1080 进程 PID: %%a
    taskkill /F /PID %%a >nul 2>nul
)

timeout /t 1 /nobreak >nul
echo.
echo 进程已停止
echo.
