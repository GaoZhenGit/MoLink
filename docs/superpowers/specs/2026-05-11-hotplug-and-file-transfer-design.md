# MoLink Access — 热插拔检测 & 文件传输 设计文档

> 日期：2026-05-11 | 分支：dev

## 一、背景

当前 molink-access-cpp 已具备：USB 设备发现、ADB 握手、通道管理、SOCKS5 端口转发、后台 daemon + Named Pipe 控制。本次新增两个能力：

1. **热插拔检测**：daemon 运行时拔出设备自动进入等待状态，插入后自动恢复
2. **文件传输**：push / pull / ls，通过 daemon pipe 命令暴露，无需 adb.exe

## 二、代码结构（低耦合拆分）

```
src/
├── main.cpp                    # CLI 入口 + daemon 事件循环（纯胶水）
├── usb/usb_device.h/cpp        # libusb 封装
├── adb/
│   ├── adb_transport.h/cpp     # ADB 消息收发 + 握手
│   ├── adb_rsa.h/cpp           # RSA 密钥 + 签名
│   ├── adb_reader.h/cpp        # USB 读线程 + 消息分发
│   ├── adb_client.h/cpp        # 设备生命周期 + 通道管理（唯一门面）
│   ├── adb_sync.h/cpp          # [NEW] sync 协议（SEND/DATA/DONE/RECV）
│   └── adb_shell.h/cpp         # [NEW] shell 命令辅助
├── forward/
│   └── forwarder.h/cpp         # SOCKS5 转发器
├── transfer/                   # [NEW]
│   ├── file_push.h/cpp         # push 操作
│   ├── file_pull.h/cpp         # pull 操作
│   └── file_list.h/cpp         # ls 操作
└── cli/named_pipe.h/cpp        # Named Pipe 服务
```

### 依赖关系

```
UsbDevice → AdbTransport/AdbReader/AdbRsa → AdbClient (唯一门面)
  → Forwarder / FilePush / FilePull / FileList (各自独立)
    → main.cpp (胶水)
```

各 transfer 模块只依赖 `AdbClient` 和底层协议辅助（`adb_sync`/`adb_shell`），互相不知道对方存在。

## 三、功能一：热插拔检测

### 3.1 状态机

```
                  ┌─────────────────┐
   start ─────→   │   CONNECTED     │
                  └───┬─────────┬───┘
                      │         │
        USB error     │         │  stop / Ctrl+C
        detected      │         │
                      ▼         ▼
                  ┌─────────────┐   stop    ┌──────────┐
                  │ DISCONNECTED│──────────→│ STOPPING │
                  └──────┬──────┘           └──────────┘
                         │
         2s 重连成功      │
                         ├──────────→ CONNECTED
         2s 重连失败      │
                         └──────────→ DISCONNECTED (继续等)
```

### 3.2 信号路径

```
AdbReader::readLoop()
  │ recv() 返回 NO_DEVICE / IO 错误
  └─→ AdbClient::notifyDisconnected()
       └─→ SetEvent(hDisconnectEvent)

daemonMain 事件循环:
  WaitForMultipleObjects(hStopEvent, hPipeEvent, hDisconnectEvent)
    ├─ hDisconnectEvent → transitionToDisconnected()
    ├─ hPipeEvent → processConnection()
    │    status → "connected serial=xxx" 或 "disconnected serial=xxx"
    │    stop   → transitionToStopping()
    └─ hStopEvent → transitionToStopping()
```

### 3.3 状态转移逻辑

**CONNECTED → DISCONNECTED：**
1. `ResetEvent(hDisconnectEvent)` — 防重入
2. `forwarder.pause()` — 原子标志阻止 acceptLoop，已有 relay 自清理
3. `client.disconnect()` — 清理 m_channels / m_pending / m_transport / close USB
4. 状态设为 DISCONNECTED

**DISCONNECTED 等待循环：**
1. 超时 2s `WaitForMultipleObjects`
2. 每轮尝试 `client.connect(m_serial)`（用记录的串号）
3. 成功 → `forwarder.resume()` → 状态回到 CONNECTED
4. 如果 wait 返回 hStopEvent 或 hPipeEvent("stop") → 进 STOPPING

**STOPPING：**
1. `forwarder.stop()` — 关闭 listen socket + join acceptLoop
2. `client.disconnect()` — 清理
3. Pipe stop、remove pid file、release daemon lock
4. 退出

### 3.4 防错处理

- **防重入**：hDisconnectEvent 是 Manual-Reset，CONNECTED 进入 DISCONNECTED 时立即 Reset，状态不回到 CONNECTED 前不会被重新触发
- **relay 自清理**：设备拔出后 USB I/O 失败 → writeChannel 返回 false / AdbReader 标记 closed → relay break → releaseSlot()。pause() 后等待 activeCount 归零（最多 5s 超时）再 resume()
- **乒乓保护**：每次 DISCONNECTED → CONNECTED 是完整状态转移，没有中间态
- **pipe 不丢命令**：DISCONNECTED 状态仍处理 pipe 事件，stop/status 正常响应

## 四、功能二：文件传输（push / pull / ls）

### 4.1 整体架构

文件传输通过 daemon pipe 命令完成。CLI 发送命令文本，daemon 在本地读写文件，结果通过 pipe 返回。文件数据不需要跨 pipe 传输。

```
molink push <local> <remote>
  → CLI: sendPipeCmd("push <local> <remote>")
  → daemon: 读本地文件 → openChannel("sync:") → SEND/DATA/DONE → closeChannel
  → 返回 "ok" 或 "fail: <reason>"

molink pull <remote> <local>
  → CLI: sendPipeCmd("pull <remote> <local>")
  → daemon: openChannel("sync:") → RECV → DATA 循环 → DONE → 写本地文件 → closeChannel
  → 返回 "ok" 或 "fail: <reason>"

molink ls [remote_path]
  → CLI: sendPipeCmd("ls <remote_path>")
  → daemon: openChannel("shell:ls -laR <path>") → 读回文本 → closeChannel
  → 返回文本输出
```

### 4.2 ADB Sync 协议（adb_sync.h/cpp）

消息格式（8 字节 header + payload）：

```
[4 bytes: ID] [4 bytes: payload_len LE] [payload]
```

| ID | 方向 | 含义 |
|----|------|------|
| `SEND` | host→device | 开始写文件，payload="<path>,<mode>" |
| `DATA` | host→device | 文件数据块，payload=raw bytes |
| `DONE` | host→device | 写完成，payload=mtime (4 bytes LE) |
| `RECV` | host→device | 开始读文件，payload=<path> |
| `DATA` | device→host | 文件数据块 |
| `DONE` | device→host | 读完成 |
| `OKAY` | device→host | 操作成功 |
| `FAIL` | device→host | 操作失败，payload=错误消息 |
| `LIST` | host→device | 列目录（暂不用，见 4.4） |
| `QUIT` | 双向 | 关闭 sync 连接 |

Sync 通道读取需要独立于 AdbReader。FilePush/FilePull 通过 AdbClient 打开通道后，自己从 Channel::dataQueue 读取 sync 响应。

### 4.3 FilePush（transfer/file_push.h/cpp）

```
FilePush::push(client, localPath, remotePath):
  1. 打开 localPath，获取文件大小和 mtime
  2. AdbClient::openChannel("sync:")
  3. 发送 SEND("<remotePath>,0644")
  4. 从 dataQueue 等待 OKAY（60s 超时）
  5. 循环读取本地文件，每次发 DATA(chunk, max 64KB)
  6. 发送 DONE(mtime)
  7. 从 dataQueue 等待 OKAY 或 FAIL
  8. 发送 QUIT → closeChannel
  9. 返回成功或错误消息
```

### 4.4 FilePull（transfer/file_pull.h/cpp）

```
FilePull::pull(client, remotePath, localPath):
  1. AdbClient::openChannel("sync:")
  2. 发送 RECV("<remotePath>")
  3. 等待第一个 DATA 块到达（确认文件存在且可读）
  4. 创建/覆盖 localPath 文件
  5. 循环从 dataQueue 读取 DATA 块，追加写入文件
  6. 收到 DONE → 关闭本地文件
  7. 发送 OKAY → QUIT → closeChannel
  8. 返回成功或错误消息
```

### 4.5 FileList（transfer/file_list.h/cpp）

```
FileList::list(client, remotePath):
  1. AdbClient::openChannel("shell:ls -laR " + remotePath)
  2. 循环从 dataQueue 读取输出，拼接字符串
  3. channel 关闭后返回拼接的文本
  4. 返回目录列表或错误消息
```

### 4.6 Pipe 命令扩展

daemon pipe handler 新增：

| 命令 | 请求 | 响应 |
|------|------|------|
| `push <local> <remote>` | CLI → daemon | `ok` 或 `fail: <error>` |
| `pull <remote> <local>` | CLI → daemon | `ok` 或 `fail: <error>` |
| `ls <path>` | CLI → daemon | 目录列表文本，或 `fail: <error>` |

### 4.7 CLI 命令

```
molink push <local> <remote>
molink pull <remote> [local]          # local 默认当前目录 + basename
molink ls [remote_path]               # 默认 /sdcard/
```

### 4.8 错误处理

- daemon 未运行 → 报错 "Daemon is not running. Use 'molink start' first."
- daemon 状态为 disconnected → 报错 "Device is disconnected, waiting for reconnect..."
- 本地文件不存在（push）→ 报错 "Local file not found: <path>"
- 远程路径不存在（pull/ls）→ 返回设备端 FAIL 消息
- 同步协议超时 → 60s 超时，返回 "Sync timeout"

## 五、测试

### 热插拔检测

1. `molink start` → `molink status` 确认 connected
2. 拔出设备 → `molink status` 确认 disconnected
3. 插入设备 → `molink status` 确认 connected（2s 内）
4. 快速重复拔插 5 次 → 确认不卡死
5. 拔出状态下发 `molink stop` → 确认正常退出
6. 前台模式 Ctrl+C → 确认正常退出

### 文件传输

1. `molink push test.txt /sdcard/test.txt` → 确认设备上文件存在
2. `molink pull /sdcard/test.txt test2.txt` → 确认本地文件内容一致
3. `molink ls /sdcard/` → 确认看到目录列表
4. push 大文件（50MB）→ 确认不超时
5. push 不存在的本地文件 → 确认报错
6. pull 不存在的远程文件 → 确认报错
