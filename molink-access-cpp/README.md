# MoLink Access (C++)

通过 USB 连接 Android 设备，实现 ADB 协议通信，将本地 TCP 端口转发到设备的 SOCKS5 代理。

## 架构

```
main.cpp
  ├── 前台调试模式 (molink)
  └── 后台守护进程 (molink start)
        ├── NamedPipe Server (\\.\pipe\molink)  ← molink stop/status
        ├── AdbClient                          # 设备生命周期
        │     ├── UsbDevice                    # libusb 封装
        │     ├── AdbRsa                       # BCrypt RSA
        │     ├── AdbTransport                 # ADB 协议
        │     └── AdbReader（独立线程）         # USB 读 + 消息分发
        └── Forwarder（accept 线程 + 每连接 relay 线程）
              └── 并发 relay (max 16): Channel queue + cv 等待
```

### 数据流

```
应用 (curl --socks5 127.0.0.1:1080)
  → TCP localhost:1080 (Winsock2 accept)
    → Forwarder relay thread (select + cv.wait_for)
      → AdbClient::writeChannel (USB write mutex)
        → UsbDevice: libusb bulk write
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
  └── A_CLSE → Channel.closed = true / PendingOpen error

Relay Thread × N (每 TCP 连接一个)
  while:
    select(clientSock, 100ms) → recv → writeChannel(mutex)
    lock(ch.mtx)
      dataQueue 有数据? → send to client
      closed? → break
      否则 → cv.wait_for(50ms)
```

## 命令

```powershell
molink                          # 前台调试模式（Ctrl+C 退出）
molink start   [options]        # 启动后台守护进程
molink stop                     # 停止守护进程
molink devices                  # 查询 USB ADB 设备列表
molink status                   # 查询守护进程状态
```

| 参数 | 简写 | 默认值 | 说明 |
|------|------|--------|------|
| `--port` | `-p` | 1080 | 本地 TCP 端口 |
| `--rport` | `-r` | 1081 | 设备目标端口 |
| `--serial` | `-s` | (第一个设备) | 设备序列号 |

## 编译

**环境：** Windows 10/11, MinGW-w64 64-bit, CMake 3.14+

验证工具链：
```powershell
g++ --version      # 应输出 x86_64-w64-mingw32
cmake --version    # 3.14+
```

**步骤：**

```powershell
# 1. 进入项目目录
cd molink-access-cpp

# 2. 创建 build 目录
mkdir build
cd build

# 3. CMake 生成 MinGW Makefile
cmake .. -G "MinGW Makefiles"

# 4. 编译
mingw32-make

# 5. 确认产物
dir molink.exe
```

输出：`build\molink.exe`，libusb-1.0.dll 自动复制到同目录。

**常见问题：**

| 现象 | 解决 |
|------|------|
| `cmake: command not found` | 安装 CMake 并加入 PATH |
| `g++: command not found` | 安装 [w64devkit](https://github.com/skeeto/w64devkit) 或 MinGW-w64，加入 PATH |
| `libusb-1.0.dll.a not found` | 确认 `libs/` 目录在项目根目录 |
| `Permission denied` (link) | 关闭正在运行的 molink.exe 后重试 |

## 使用示例

```powershell
# 前台调试
.\molink.exe -p 1080

# 后台守护
.\molink.exe start -p 1080
curl --socks5 127.0.0.1:1080 --proxy-user socks5:password123 https://www.baidu.com
.\molink.exe status
.\molink.exe stop

# 查询设备
.\molink.exe devices
```

## 进程管理

- **单实例锁**: `Global\MoLinkDaemon` Mutex，防止重复启动
- **PID 文件**: `<exe_dir>\molinkd.pid`，用于 force kill
- **Named Pipe**: `\\.\pipe\molink`，消息模式，支持 `stop` / `status` 命令

## 关键技术细节

- **校验和**：ADB data_check 是字节求和，非 CRC32
- **Header/Data 分离**：ADB header(24B) 和 payload 须分两次 USB bulk write
- **Banner**：不含 NUL 终止符（`data_length = banner.size()`）
- **RSA 签名**：原始 20 字节 token 直接放入 PKCS#1 v1.5 DigestInfo，不做 SHA1
- **BCrypt**：Windows CNG，`BCryptDecrypt(PAD_NONE)` 做裸 RSA 签名
- **openChannel**：设备响应的 `msg.arg0` 是 remote_id（不是 arg1）
- **AdbReader**：唯一 USB 读线程，消息按 `local_id` 分发到 Channel 队列
- **PendingOpen**：openChannel 不自己 recv，由 AdbReader 分发 A_OKAY 响应

## 密钥管理

加载优先级：
1. `%USERPROFILE%\.android\molink_key.bin` — 自生成密钥（BCrypt blob）
2. `%USERPROFILE%\.android\adbkey` — 导入 adb 密钥（PKCS#8）

AUTH_RSAPUBLICKEY 来源：
- 优先 `%USERPROFILE%\.android\adbkey.pub`（adb 已授权免弹窗）
- 回退自建 RSAPublicKey 结构体（需设备弹窗授权）

## 文件结构

```
molink-access-cpp/
├── CMakeLists.txt
├── libs/
│   └── libusb-1.0.dll
├── vendor/libusb/
│   └── libusb.h
└── src/
    ├── main.cpp                     # CLI 入口（多命令）
    ├── usb/
    │   ├── usb_device.h/cpp         # libusb 封装
    ├── adb/
    │   ├── adb_transport.h/cpp      # ADB 协议（send/recv/handshake）
    │   ├── adb_rsa.h/cpp            # RSA 密钥 + 签名
    │   ├── adb_reader.h/cpp         # USB 读线程 + 消息分发
    │   ├── adb_client.h/cpp         # 高层客户端 + ChannelMap
    ├── cli/
    │   ├── named_pipe.h/cpp         # Named Pipe Server
    └── forward/
        ├── forwarder.h/cpp          # TCP 端口转发（多连接并发）
```
