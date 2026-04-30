# MoLink Access C++ 重写设计文档

## 概述

将 molink-access（Windows 端）从 Spring Boot / Java 重写为 C++ 原生程序，直接调用 AdbWinApi.dll 实现 USB 设备通信和端口转发。

**Worker 端（Android）不变。**

## 约束

- **语言**: C++17
- **编译器**: MinGW GCC 32-bit (D:/software/w64devkit/x86)，因为 AdbWinApi.dll 是 PE32 格式
- **构建**: CMake（已加入 PATH）
- **运行环境**: Windows 10，不允许运行 adb.exe
- **进程模型**: 单进程前台守护
- **对外接口**: 纯 CLI，不提供 HTTP API

## 项目结构

```
molink-access-cpp/
├── CMakeLists.txt
├── libs/
│   ├── AdbWinApi.dll             # 从 Android SDK 复制
│   ├── AdbWinUsbApi.dll          # AdbWinApi.dll 的依赖
│   └── libwinpthread-1.dll       # MinGW 运行时（发行时）
├── vendor/
│   └── adb_api.h                 # AOSP 官方头文件
├── src/
│   ├── main.cpp                  # 入口 + 主循环
│   ├── adb/
│   │   ├── adb_dll.h             # DLL 动态加载 + 函数指针声明
│   │   ├── adb_dll.cpp           # LoadLibrary / GetProcAddress
│   │   ├── adb_transport.h/.cpp  # ADB 协议实现
│   │   └── adb_device.h/.cpp     # 单设备抽象
│   ├── cli/
│   │   └── cli_server.h/.cpp     # Named Pipe 命令服务
│   ├── forward/
│   │   └── forwarder.h/.cpp      # 端口转发管理
│   └── core/
│       └── daemon.h/.cpp         # 守护进程主逻辑
```

## 架构：单线程事件驱动

```
┌─────────────────────────────────────────────┐
│                   molink.exe                │
│                                             │
│  ┌──────────┐  ┌──────────┐  ┌───────────┐ │
│  │ CLI Server│  │  Device   │  │ Forwarder │ │
│  │(Named    │  │  Poller   │  │  Manager  │ │
│  │ Pipe)    │  │  (Timer)  │  │           │ │
│  └────┬─────┘  └────┬─────┘  └─────┬─────┘ │
│       │             │              │        │
│       └─────────────┼──────────────┘        │
│                     │                       │
│            ┌────────┴────────┐              │
│            │   ADB Protocol  │              │
│            │   (A_CNXN/A_OPEN│              │
│            │   /A_WRTE/A_CLSE)│             │
│            └────────┬────────┘              │
│                     │                       │
│            ┌────────┴────────┐              │
│            │ AdbWinApi.dll   │              │
│            └─────────────────┘              │
└─────────────────────────────────────────────┘
```

## ADB 协议层

### 消息格式

24 字节消息头 + 可变数据：

| 偏移 | 字段 | 大小 | 说明 |
|------|------|------|------|
| 0 | command | 4B | A_CNXN / A_AUTH / A_OPEN / A_OKAY / A_WRTE / A_CLSE |
| 4 | arg0 | 4B | local_id |
| 8 | arg1 | 4B | remote_id |
| 12 | length | 4B | 数据体长度 |
| 16 | crc32 | 4B | 数据体 CRC32 校验（0 = 不校验） |
| 20 | magic | 4B | command ^ 0xFFFFFFFF |

### 消息类型

| 消息 | arg0 | arg1 | 用途 |
|------|------|------|------|
| A_CNXN | 0x01000000 | 256K | USB 连接握手，协商协议版本 |
| A_AUTH | type | 0 | RSA 签名认证 |
| A_OPEN | local_id | 0 | 打开到服务的通道 |
| A_OKAY | local_id | remote_id | 通道就绪确认 |
| A_WRTE | local_id | remote_id | 数据传输 |
| A_CLSE | local_id | remote_id | 关闭通道 |

### USB 传输流程

```
AdbEnumInterfaces()  →  发现设备列表
        ↓
AdbCreateInterfaceByName()  →  打开指定设备
        ↓
AdbOpenDefaultBulkReadEndpoint()  →  获取读端点句柄
AdbOpenDefaultBulkWriteEndpoint() →  获取写端点句柄
        ↓
A_CNXN → 握手 (版本 0x01000000, maxdata 256K, "host::")
        ↓  (设备可能要求 A_AUTH)
        ↓
A_OPEN("tcp:1080") → 打开 SOCKS5 代理通道
        ↓
A_WRTE / A_WRTE → 双向数据转发
```

### 认证

从 `%USERPROFILE%\.android\adbkey` 读取已有 ADB 私钥做 RSA 签名，与 `adb.exe` 共享密钥对。

### DLL 加载

启动时通过 `LoadLibrary` 动态加载 `libs/AdbWinApi.dll`，函数指针通过 `GetProcAddress` 绑定。构建时 CMake 复制 DLL 到输出目录。

## CLI 设计

### 双用模式

同一个 `molink.exe`：

- **不带参数**：启动守护进程（前台运行，Ctrl+C 退出）
- **带子命令**：作为 CLI 客户端连接守护进程 Named Pipe，打印结果后退出

### Named Pipe

管道名：`\\.\pipe\molink`

命令格式：纯文本行 `"command arg1 arg2\n"`，响应为 JSON。

### 命令列表

| 命令 | 说明 |
|------|------|
| `molink` | 启动守护进程 |
| `molink devices` | 列出已连接设备及转发状态 |
| `molink status` | 查看守护进程状态（运行时间、设备数） |
| `molink forward <local_port> <remote_port>` | 手动添加端口转发 |

### 输出示例

```
> molink devices
[
  {"serial":"abc123","state":"connected","local_port":1080,"remote_port":1080}
]

> molink status
daemon: running
uptime: 3600s
devices: 1
```

## 端口转发

### 数据流

```
外部程序                     molink                      Android 设备
  │                           │                              │
  │── connect(127.0.0.1:1080) →│                             │
  │                           │── A_OPEN("tcp:1080") ──────→│
  │                           │←──── A_OKAY ──────────────│
  │                           │                              │
  │═══ 数据流 ═══════════════│═══ A_WRTE / A_WRTE ═════════│
  │        (双向转发)          │        (Bulk USB)             │
  │                           │                              │
  │←── close ───────────────│── A_CLSE ──────────────────→│
```

### 实现

- 本地监听 socket（Winsock），accept 后与 ADB 通道建立双向桥接
- `select()` 同时监听本地 socket 和 ADB 端点
- 本地→远程：`recv(local) → A_WRTE(adb_write)`
- 远程→本地：`A_WRTE(adb_read) → send(local)`

### 多设备

每设备分配递增本地端口（1080, 1081, 1082...），各自独立 Forwarder 实例和 ADB 连接。设备拔出时自动释放端口。

## 守护进程主循环

```
molink (不带参数启动守护进程)
     │
     ├─ 加载 AdbWinApi.dll
     ├─ 创建 Named Pipe: \\.\pipe\molink
     ├─ 启动设备轮询定时器 (3s)
     ├─ 自动发现设备、建立 forward
     ├─ 将所有 socket/pipe 加入 select() fd 集合
     └─ 进入主循环
           │
           ├─ select() 返回 → 分派处理
           │    ├─ Named Pipe 有连接 → 处理命令
           │    ├─ 本地监听 socket 有新连接 → accept + 建立 ADB 通道
           │    ├─ 已建立连接有数据 → 双向转发
           │    └─ 定时器到期 → 轮询设备
           └─ Ctrl+C → 清理退出
```

## 依赖

- **运行时 DLL**: AdbWinApi.dll, AdbWinUsbApi.dll, libwinpthread-1.dll
- **Windows API**: setupapi（设备枚举）、winusb（USB I/O）、winsock2（网络）
- **静态链接**: C++ 标准库（libstdc++）

## 废弃项

以下 Java 版功能不迁移：
- Spring Boot / Tomcat
- REST API（`/molink/devices`, `/molink/config` 等）
- OkHttp 代理健康检查
- picocli 命令行解析
- dadb Java 库
