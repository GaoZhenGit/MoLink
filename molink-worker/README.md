# MoLink Worker

Android 端 SOCKS5 代理应用，通过 USB 连接到 Windows 端实现代理上网。

## 前置条件

- Android SDK（位于 D:\AndroidSdk）
- Gradle 6.9.4+ 或 Gradle Wrapper
- Android 设备已开启 USB 调试
- Windows 端已安装 ADB（Android SDK Platform Tools）

## 快速开始

### 1. 构建
```cmd
build.bat
```

### 2. 安装
```cmd
install.bat
```
> 自动卸载旧版本并安装新版本

### 3. 启动
```cmd
start.bat
```

### 4. 查看日志
```cmd
logcat.bat
```

## 脚本说明

| 脚本 | 功能 |
|------|------|
| `build.bat` | 清理并构建 Android 项目 |
| `install.bat` | 卸载旧版本，安装新 APK |
| `start.bat` | 停止旧进程，启动应用 |
| `stop.bat` | 停止应用 |
| `logcat.bat` | 查看应用日志（实时） |

## 手动操作

```bash
# 构建
gradlew assembleDebug

# 安装
adb install -r app\build\outputs\apk\debug\app-debug.apk

# 启动
adb shell am start -n com.molink.worker/.MainActivity

# 停止
adb shell am force-stop com.molink.worker

# 查看日志
adb logcat | findstr molink
```
