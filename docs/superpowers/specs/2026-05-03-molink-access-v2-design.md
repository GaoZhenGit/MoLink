# MoLink Access V2 设计文档

> 三个功能：并发转发、devices 命令、前后台模式

## 1. USB 读线程 + Channel 分发

### 新文件 `src/adb/adb_reader.h/cpp`

```
AdbReader:
  start(AdbTransport*, ChannelMap*)  → 启动读线程
  stop()                             → 设置标志 + join
  readLoop():                        → 唯一 USB 读线程
    recv(msg, data, 100ms)
    ch = ChannelMap[msg.arg1]        ← arg1 = host local_id
    switch msg.command:
      A_WRTE: ch.dataQueue.push → cv.notify
      A_OKAY: 忽略
      A_CLSE: ch.closed=true → cv.notify
```

### 数据结构（在 AdbClient 中）

```cpp
struct Channel {
    uint32_t localId, remoteId;
    std::queue<std::vector<uint8_t>> dataQueue;
    std::mutex mtx;
    std::condition_variable cv;
    bool closed = false;   // AdbReader 设 true，通知 relay 设备已关
    bool draining = false;  // closeChannel 设 true，通知 reader 丢弃后续消息
};
using ChannelMap = std::unordered_map<uint32_t, std::shared_ptr<Channel>>;
```

### AdbClient 改动

- **移除** `readChannel()` — relay 不再直接读 USB
- **新增** `m_channels` (ChannelMap) + mutex, `m_reader` (AdbReader)
- `openChannel` → 创建 Channel 加入 map, local_id 递增
- `closeChannel` → send A_CLSE (USB write mutex) + 设置 ch.draining=true，**不自己 recv**
  - AdbReader 读到响应 A_CLSE 时设置 ch.closed=true
  - Relay 看到 closed 后退出，从 map 移除 Channel (lock ChannelMap mutex)
  - shared_ptr 引用归零后自动析构
- AdbReader 读到不在 map 中的消息（stale/race）→ 丢弃
- `connect` 时 `startReader()`, `disconnect` 时 `stopReader()`
- `writeChannel` 保持不变（mutex 保护 USB 写）

### Relay Thread 等待数据

```
while(running):
  select(clientSock, 100ms)
  可读 → recv → writeChannel(ch, data)

  lock(ch.mtx)
  if dataQueue 非空: pop → send to client
  elif closed: break
  else: cv.wait_for(50ms)
```

---

## 2. Forwarder 多连接并发

### 并发控制

用 `m_activeCount` + mutex 限制最大连接数 16：

```cpp
bool tryAcquireSlot() { lock; if (n>=16) false; else n++; }
void releaseSlot()    { lock; n--; }
```

### acceptLoop

```
while(running):
  accept → INVALID_SOCKET? break
  tryAcquireSlot()? no → closesocket, continue
  yes → std::thread(&Forwarder::relay, client).detach()
```

### relay(clientSock)

```
openChannel("tcp:<port>") → 失败? closesocket, releaseSlot, return
while(running):
  select→recv→writeChannel  // client→ADB
  读 channel.queue           // ADB→client
  任一失败/关闭 → break
closeChannel → Channel 从 map 移除
closesocket; releaseSlot
```

### 并发安全

- USB 写: AdbClient::m_writeMutex（已有）
- USB 读: AdbReader 唯一线程（与新 Channel 可见性用 ChannelMap mutex 同步）
- ChannelMap 操作: openChannel / AdbReader 查找 / closeChannel-remove 均持有 ChannelMap mutex
- closeChannel 流程: send A_CLSE (write mutex) → lock ch.mtx 设 draining=true → AdbReader 读到响应 A_CLSE → set ch.closed → relay 退出 → lock ChannelMap mutex remove Channel → shared_ptr 析构
- AdbReader 读到 local_id 不在 ChannelMap → 丢弃（stale 消息或已关闭通道）

---

## 3. CLI 命令 + 前后台模式

### 命令矩阵

```
molink                      前台调试模式（保留）
molink start   [options]     启动后台守护进程
molink stop                  停止守护进程
molink devices               查询 USB 设备（独立 discover）
molink status                查询守护进程状态
```

### 后台守护进程 (`molink start`)

1. **单实例锁**: `CreateMutex("Global\\MoLinkDaemon")`, 重复启动则 exit
2. **写 PID 文件**: `<exe_dir>\molinkd.pid`
3. **连接设备**: AdbClient.connect(serial), 失败→删 PID→exit
4. **启动转发**: Forwarder.start()
5. **Named Pipe Server**: `\\.\pipe\molink`, 消息模式, OVERLAPPED
6. **FreeConsole()**: 脱离终端
7. **事件循环**: WaitForMultipleObjects(NamedPipe, stop Event)
8. **清理**: stop转发→断开设备→删PID→关Pipe→ReleaseMutex

### 停止守护进程 (`molink stop`)

1. 连接 Named Pipe `\\.\pipe\molink`
2. 发送 `stop\n`
3. 等 3s → daemon 响应 `ok\n` → 正常退出
4. 超时 → 读 PID 文件 → OpenProcess → TerminateProcess

### 设备查询 (`molink devices`)

独立执行，不走 daemon：`UsbDevice::discover()` → 打印列表

```
ADB Devices (1):
  852QLDV923XMM    VID=0x2A45 PID=0x4EE7
```

### 状态查询 (`molink status`)

连接 Named Pipe → `status\n` → 打印响应：

```
connected  serial=852QLDV923XMM  port=1080  connections=3  uptime=3600s
```

### Named Pipe 命令协议

| client→server | server→client | 说明 |
|--------------|---------------|------|
| `stop\n` | `ok\n` | 优雅退出 |
| `status\n` | `<单行>\n` | 查询状态 |

---

## 4. 新增文件清单

| 文件 | 职责 |
|------|------|
| `src/adb/adb_reader.h/cpp` | USB 读线程，消息分发到 Channel |
| `src/cli/named_pipe.h/cpp` | Named Pipe Server 封装 |

## 5. 修改文件清单

| 文件 | 改动 |
|------|------|
| `src/main.cpp` | 重写：命令分发，前后台模式 |
| `src/adb/adb_client.h/cpp` | +ChannelMap, +AdbReader, +reader 生命周期 |
| `src/adb/adb_transport.h/cpp` | readChannel 改为 AdbReader 内部使用 |
| `src/forward/forwarder.h/cpp` | 多线程 accept+relay |
| `CMakeLists.txt` | +新文件 |
