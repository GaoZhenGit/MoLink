# MoLink

## Context

内网电脑需要通过 Android 设备代理上网。项目包含两个独立端：worker（Android）和 access（Windows C++）。通过 USB 连接，实现 ADB 协议通信，建立 SOCKS5 代理通道。

## 项目结构

两个独立项目目录：

```
D:/project/MoLink/
├── molink-worker/         # Android App
└── molink-access-cpp/     # Windows CLI (C++, 自实现 ADB 协议栈)
```

## 通用说明

- **自实现 ADB 协议栈**：libusb + BCrypt RSA，零第三方 DLL 依赖，静态编译
- **开发环境**：Windows 10，MinGW-w64 64-bit，CMake 3.14+
- **构建脚本**：`molink-access-cpp/clean_build.ps1`（停服→清理→编译→恢复 key）
- **配置优先级**：环境变量 > 配置文件 > 默认值

## 一、molink-access-cpp（Windows 端）

### 技术栈
- **语言**：C++17
- **编译器**：MinGW-w64 (GCC 12.1.0)
- **构建**：CMake + MinGW Makefiles
- **依赖**：libusb 1.0.28（静态编译）、BCrypt、Winsock2
- **自实现**：ADB 协议栈、RSA 密钥管理、USB 设备发现、端口转发

### 架构

```
main.cpp → CLI 命令分发
  ├── DaemonApp（后台守护进程）
  │     ├── AdbClient（设备连接生命周期）
  │     │     ├── UsbDevice（libusb 封装）
  │     │     ├── AdbRsa（BCrypt RSA，自生成 molink_key.bin）
  │     │     ├── AdbTransport（ADB 协议：send/recv/handshake）
  │     │     └── AdbReader（独立读线程，消息分发到 Channel 队列）
  │     ├── NamedPipeServer（\\.\pipe\molink，CLI 通信）
  │     └── Forwarder（TCP 端口转发，按需启动）
  └── CLI 命令（src/cli/，每个命令独立文件）
```

### 日志系统

- **头文件**：`src/utils/log.h`（~40 行，零依赖）
- **格式**：`YYYY-MM-DD HH:MM:SS.mmm [LEVEL] [TAG] message`
- **级别**：INFO（生命周期）、DEBUG（逐包数据，编译宏控制）、WARN（异常关闭）、ERROR（致命错误）
- **Channel 标记**：forwarder relay 使用 `FWD:localId/remoteId` 动态 tag
- **编译开关**：CMakeLists.txt 中 `-DMOLINK_DEBUG_LOG` 控制 DEBUG 输出

### 密钥管理

- **路径**：exe 同级目录 `molink_key.bin`
- **自生成**：BCryptGenerateKeyPair + BCryptFinalizeKeyPair（2048-bit RSA）
- **授权**：`molink auth` 触发设备弹窗，用户确认后设备存储公钥
- **不再依赖** adb 的 adbkey/adbkey.pub

### 版本号

- `molink -v` / `molink --version` 输出 `vYYYY.MM.DD.HHmm`
- CMake 每次构建前自动生成 `version_gen.h`，确保增量构建版本号也更新

### 命令参考

```
molink run      [options]         前台运行
molink start    [options]         启动后台守护进程
molink stop                       停止守护进程
molink status                     查询守护进程状态（daemon=running/stopped）
molink devices                    列出 ADB 设备
molink auth     [-s serial]       触发设备授权弹窗
molink forward  [options]         启动端口转发（需 daemon 运行）
molink push     <local> <remote>  上传文件/目录到设备
molink pull     <remote> <local>  下载文件/目录到设备
molink ls       [remote_path]     列出设备目录
molink del      <remote_path>     删除设备文件
molink shell    <command>         执行设备 shell 命令
molink -v / --version             显示版本号
molink -h / --help                显示帮助
```

## 二、molink-worker（Android 端）

### 技术栈
- Android Studio + Gradle
- 纯 Java 开发
- **Gradle 版本：6.9.4，不要修改**
- **AGP 版本：不要修改**
- **Android 框架版本：不要修改**
- Android SDK：D:\AndroidSdk
- 最低支持 Android 8.1（API 27）

### 核心组件
- **Socks5ProxyService**：后台 Service，实现 SOCKS5 协议，仅支持 CONNECT 命令，默认端口 1080
- **MainActivity**：主界面，显示连接状态、本地端口、可开启/停止服务
- **AdbConnectionManager**：通过 ADB 协议管理 USB 连接

### 数据流
```
[molink-access-cpp] → [USB/ADB] → [Socks5ProxyService] → [互联网]
```

## 测试
- 测试脚本：`test/test.py`，包含清理日志、停止服务、构建、运行服务、测试、停止服务
