# MoLink 技术文档

> 随开发持续更新。协议内幕、踩坑记录等深度参考见 `docs/molink-access-reference.md`。

## 一、molink-access（Windows 端）

### 技术栈

- **语言**：C++17
- **编译器**：MinGW-w64 (GCC 12.1.0)
- **构建**：CMake + MinGW Makefiles
- **依赖**：libusb 1.0.28（静态编译）、BCrypt、Winsock2
- **自实现**：ADB 协议栈、RSA 密钥管理、USB 设备发现、端口转发

### 编码约定

- **内部编码**：全程 UTF-8。`main()` 通过 `GetCommandLineW()` + `CommandLineToArgvW()` 绕过 MinGW `argv[]` 的 ANSI 代码页损耗，转为 UTF-8
- **路径处理**：`fs::u8path(str)` 构造，`path.u8string()` 导出，禁止用 `path.string()`（C locale 丢中文）
- **文件 I/O**：`fopenUtf8()` 封装 `_wfopen`，Win32 API 统一用 `W` 版本（`GetTempPathW`、`CreateDirectoryW`、`CreateProcessW` 等）
- **控制台输出**：`utf8ForConsole()` 将 UTF-8 转为当前控制台代码页（Win10 1903+ 直接设 `CP_UTF8`，旧版回退到 `CP_ACP`/GBK）
- **工具头文件**：`src/utils/win_utils.h`（`WideArgv`、`Utf8Args`、`wideToUtf8`/`utf8ToWide`、`fopenUtf8`、`utf8ForConsole`）

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
molink install  [options] <apk>   安装 APK（push + pm install）
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
[molink-access] → [USB/ADB] → [Socks5ProxyService] → [互联网]
```

## 测试

- 测试脚本：`test/test.py`，包含清理日志、停止服务、构建、运行服务、测试、停止服务
