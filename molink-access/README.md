# MoLink Access (C++)

通过 USB 连接 Android 设备，自实现 ADB 协议栈，提供端口转发、文件传输和设备管理功能。

零第三方 DLL 依赖（libusb 静态编译），单 exe 复制即运行。

## 架构

```
main.cpp → CLI 命令分发
  ├── 前台模式 (molink run)
  ├── 后台守护 (molink start → DaemonApp)
  │     ├── NamedPipeServer (\\.\pipe\molink)  ← CLI 通信
  │     ├── AdbClient                          # 设备生命周期
  │     │     ├── UsbDevice                    # libusb 封装
  │     │     ├── AdbRsa                       # BCrypt RSA (自生成密钥)
  │     │     ├── AdbTransport                 # ADB 协议 (send/recv/handshake)
  │     │     └── AdbReader（独立线程）         # USB 读 + 消息分发
  │     └── Forwarder（按需启动，每连接独立 relay 线程）
  └── CLI 命令（src/cli/，每个命令独立文件）
```

### 数据流

```
应用 (curl --socks5 127.0.0.1:1080)
  → TCP localhost:1080 (Winsock2 accept)
    → Forwarder relay（优先排空 ADB→client 队列，非阻塞双向转发）
      → AdbClient::writeChannel (USB write mutex)
        → libusb bulk write
      ← AdbReader: libusb bulk read → Channel::dataQueue → cv.notify
          ↕ USB cable
          → 设备 adbd → worker SOCKS5 → 互联网
```

### 并发模型

```
USB Read Thread (AdbReader, 唯一)
  │ recv() 100ms 循环
  ├── A_WRTE → push Channel[local_id].dataQueue → cv.notify
  ├── A_OKAY → PendingOpen 分发 / 丢弃
  └── A_CLSE → Channel.closed / PendingOpen error

Relay Thread × N (每 TCP 连接一个)
  loop:
    1. 排空 dataQueue（所有待发数据一次性发完，无延迟）
    2. 非阻塞轮询 clientSocket → writeChannel
    3. cv.wait_for(100ms)（Reader notify 时立即唤醒）
```

## 命令

```
molink                           显示帮助
molink run      [options]        前台运行（Ctrl+C 退出）
molink start    [options]        启动后台守护进程
molink stop                      停止守护进程
molink status                    查询守护进程状态
molink devices                   列出 ADB 设备
molink auth     [-s serial]      触发设备授权弹窗
molink forward  [options]        启动端口转发（需 daemon 运行）
molink push     <local> <remote> 上传文件/目录
molink pull     <remote> <local> 下载文件/目录
molink apush    <path> [--rdir R] 自动上传（zip + .gitignore）
molink apull    [--rdir R]        交互式自动下载 + 解压
molink adel     [--rdir R]        交互式删除远程文件
molink ls       [remote_path]    列出设备目录
molink del      <remote_path>    删除设备文件
molink shell    <command>        执行 shell 命令
molink install  [options] <apk>   安装 APK（push + pm install）
molink -v / --version            显示版本号
```

| 参数 | 简写 | 默认值 | 说明 |
|------|------|--------|------|
| `--port` | `-p` | 1080 | 本地 TCP 监听端口 |
| `--rport` | `-r` | 1081 | 设备目标端口 |
| `--serial` | `-s` | 第一个设备 | 指定设备序列号 |
| `--rdir` | — | /sdcard/tmp | 远程目录 (apush/apull/adel) |

### 使用示例

```powershell
# 前台运行
.\molink.exe run -p 1080

# 后台守护 + 转发（分离模式）
.\molink.exe start
.\molink.exe status        # → daemon=running state=connected serial=XXX forwarding=off
.\molink.exe forward -p 1080 -r 1081
curl --socks5 127.0.0.1:1080 https://www.baidu.com
.\molink.exe stop

# 设备授权（首次使用或授权过期）
.\molink.exe auth
# 设备弹出授权对话框 → 点"允许" → ok: User accepted

# 文件传输
.\molink.exe push .\test.txt /sdcard/test.txt
.\molink.exe pull /sdcard/test.txt .\downloaded.txt

# APK 安装
.\molink.exe install -r .\app-release.apk

# 版本号
.\molink.exe -v             # → v2026.05.15.1530
```

## 编译

**环境：** Windows 10/11, MinGW-w64 64-bit, CMake 3.14+

```powershell
# 一键构建
.\clean_build.ps1

# 或手动：
cd molink-access
mkdir build && cd build
cmake .. -G "MinGW Makefiles"
cmake --build . --config Release -- -j4 --output-sync=line
```

### 构建脚本

`clean_build.ps1` 执行流程：
1. 停止旧 daemon
2. 备份 `molink_key.bin`（避免重新授权）
3. 清空 build 目录
4. 显示工具链路径
5. CMake 配置 + 编译
6. 恢复 `molink_key.bin`

产物 `build\molink.exe`，**零第三方 DLL 依赖**，复制到任意 Windows 机器直接运行。

### libusb 静态库编译

libusb 已预编译在 `third_party/libusb/`（v1.0.28, MinGW-w64 64-bit）。如需重新编译：

```powershell
# 1. 下载源码 https://github.com/libusb/libusb
curl -L -o libusb.tar.bz2 https://github.com/libusb/libusb/releases/download/v1.0.28/libusb-1.0.28.tar.bz2

# 2. 解压编译
tar xf libusb.tar.bz2 && cd libusb-1.0.28
./configure --host=x86_64-w64-mingw32 --enable-static --disable-shared
make -j4

# 3. 安装
cp libusb/.libs/libusb-1.0.a ../third_party/libusb/lib/
cp libusb/libusb.h ../third_party/libusb/include/
```

## 进程管理

| 机制 | 说明 |
|------|------|
| 单实例锁 | `Global\MoLinkDaemon` Mutex |
| PID 文件 | `<exe_dir>\molinkd.pid` |
| Named Pipe | `\\.\pipe\molink`，消息模式 |
| 日志文件 | `<exe_dir>\molinkd.log`，daemon 启动时创建 |

## 日志系统

**格式**：`2026-05-15 15:22:21.800 [INFO] [ADB] handshake attempt 1`

| 宏 | 编译开关 | 用途 |
|---|---|---|
| `LOG_INFO(tag, ...)` | 始终 | 生命周期、连接、启动 |
| `LOG_DEBUG(tag, ...)` | `-DMOLINK_DEBUG_LOG` | 逐包数据、握手细节 |
| `LOG_WARN(tag, ...)` | 始终 | 异常关闭、重试 |
| `LOG_ERROR(tag, ...)` | 始终 | 致命错误、连接断开 |

Tag 示例：`ADB`、`USB`、`RSA`、`FWD`、`DAEMON`、`SYNC`、`PUSH`、`SHELL`。Forwarder relay 用动态 tag：`FWD:2/175`（localId/remoteId）。

CMakeLists.txt 中注释 `add_definitions(-DMOLINK_DEBUG_LOG)` 即可关闭 DEBUG 输出。

## 密钥管理

- **位置**：exe 同级目录 `molink_key.bin`
- **首次启动**：自动生成 2048-bit RSA 密钥对
- **授权**：`molink auth` 触发设备弹窗，用户确认后设备存储公钥
- **免授权**：设备已存公钥 → 后续连接只验证签名，不弹窗
- **不依赖** adb 的 adbkey / adbkey.pub

## 关键技术细节

- **校验和**：ADB data_check 是字节求和，非 CRC32
- **Header/Data 分离**：ADB header(24B) 和 payload 分两次 USB bulk write
- **Banner**：不含 NUL 终止符（`data_length = banner.size()`）
- **RSA 签名**：原始 20 字节 token 直接放入 PKCS#1 v1.5 DigestInfo，不做 SHA1
- **BCrypt**：Windows CNG，`BCryptDecrypt(PAD_NONE)` 做裸 RSA 签名
- **RSAPublicKey**：自建 524 字节结构体（n0inv + rr + modulus + exponent），base64 编码
- **openChannel**：设备响应的 `msg.arg0` 是 remote_id
- **AdbReader**：唯一 USB 读线程，消息按 `local_id` 分发到 Channel 队列
- **Payload 超时**：读 header 用调用者 timeout（100ms），读 payload 用 30s（USB 传输是硬件速度）
- **Bad magic 处理**：协议帧错误触发重连，重连前 drain 残留 USB 数据避免握手失败

## 文件结构

```
molink-access/
├── CMakeLists.txt                 # 构建配置（含版本号自动生成）
├── clean_build.ps1                # 一键构建脚本
├── version.cmake                  # 版本号生成 CMake 脚本
├── third_party/libusb/
│   ├── include/libusb.h           # libusb 头文件
│   └── lib/libusb-1.0.a           # libusb 静态库（v1.0.28）
├── docs/                          # 设计文档和规范
└── src/
    ├── main.cpp                   # CLI 入口
    ├── version.h/cpp              # 版本号输出
    ├── usb/
    │   └── usb_device.h/cpp       # libusb 封装
    ├── adb/
    │   ├── adb_transport.h/cpp    # ADB 协议
    │   ├── adb_rsa.h/cpp          # BCrypt RSA 密钥
    │   ├── adb_reader.h/cpp       # USB 读取线程
    │   ├── adb_client.h/cpp       # 高层客户端
    │   ├── adb_sync.h/cpp         # SYNC 协议
    │   └── adb_shell.h/cpp        # Shell 命令
    ├── cli/
    │   ├── cli_utils.h/cpp        # CLI 公共（RemoteFile 解析等）
    │   ├── cli_auth.cpp           # 设备授权
    │   ├── cli_devices.cpp        # 设备列表
    │   ├── cli_forward.cpp        # 端口转发命令
    │   ├── named_pipe.h/cpp       # Named Pipe Server
    │   └── cli_{apush,apull,adel,del,install}.cpp  # 文件管理命令
    ├── daemon/
    │   └── daemon_app.h/cpp       # 守护进程主类
    ├── forward/
    │   └── forwarder.h/cpp        # TCP 端口转发器
    ├── transfer/
    │   ├── file_push.cpp          # 文件上传
    │   ├── file_pull.cpp          # 文件下载
    │   └── file_list.cpp          # 目录列表
    └── utils/
        └── log.h                  # 日志宏
```
