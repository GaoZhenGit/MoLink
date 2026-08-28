# MoLink Access C++ 参考文档

> 自实现 ADB 协议栈（libusb + BCrypt RSA），等价 `adb forward` 功能。
> POC 始于 2026-04-30，V2 完成于 2026-05-03。

## 目录

1. [架构](#架构)
2. [ADB 协议实现细节](#adb-协议实现细节)
3. [踩坑记录](#踩坑记录)
4. [命令行参考](#命令行参考)
5. [进程模型](#进程模型)
6. [编译与依赖](#编译与依赖)
7. [文件结构](#文件结构)
8. [设备与环境](#设备与环境)

---

## 架构

### 整体数据流

```
应用 (curl --socks5 127.0.0.1:1080)
  → TCP localhost:1080 (Winsock2 accept)
    → Forwarder relay thread (select + cv.wait_for)
      → AdbClient::writeChannel (USB write mutex)
        → UsbDevice: libusb bulk write
      ← AdbReader: libusb bulk read → Channel::dataQueue → cv.notify
          ↕ USB cable
          → 设备 adbd → worker SOCKS5 (tcp:1081) → 互联网
```

### 并发模型（V2）

```
USB Read Thread (AdbReader, 唯一)
  │ recv() 100ms 循环
  ├── A_WRTE → push Channel[local_id].dataQueue → cv.notify
  ├── A_OKAY → PendingOpen 分发 / 丢弃（write ack）
  ├── A_CLSE → Channel.closed = true / PendingOpen error
  └── bad magic / USB fatal → disconnect callback → 主循环重连

Relay Thread × N (每 TCP 连接一个, max 16)
  loop:
    1. 排空 dataQueue → send to client（所有待发数据一次性发完）
    2. 非阻塞轮询 clientSocket → recv → writeChannel
    3. cv.wait_for(100ms)（Reader notify 时立即唤醒）
```
      否则 → cv.wait_for(50ms)

Main Thread
  ├── 前台模式: acceptLoop 在主线程
  └── 后台模式: daemonMain 事件循环 (NamedPipe + stop event)
```

### 组件层次

```
main.cpp
  ├── AdbClient                          # 设备生命周期管理
  │     ├── UsbDevice                    # libusb 封装（发现/打开/读写）
  │     ├── AdbRsa                       # BCrypt RSA（密钥生成/导入/签名）
  │     ├── AdbTransport                 # ADB 协议（send/recv/handshake）
  │     └── AdbReader（独立线程）         # USB 读 + 消息分发到 Channel
  ├── Forwarder                          # TCP 端口转发（多连接并发）
  │     ├── acceptLoop                   # accept 新连接
  │     └── relay × N (detach)          # 每连接一个转发线程
  └── NamedPipeServer                    # Named Pipe（stop/status 命令）
```

---

## ADB 协议实现细节

### 消息格式

24 字节 header，小端序，packed：

```cpp
#pragma pack(push, 1)
struct AdbMessage {
    uint32_t command;       // A_CNXN / A_AUTH / A_OPEN / A_OKAY / A_CLSE / A_WRTE
    uint32_t arg0;          // local_id (host 分配)
    uint32_t arg1;          // remote_id (设备分配)
    uint32_t data_length;   // payload 字节数
    uint32_t data_crc32;    // 校验和（字节求和，非 CRC32）
    uint32_t magic;         // command ^ 0xFFFFFFFF
};
#pragma pack(pop)
```

### 协议常量

```cpp
constexpr uint32_t A_CNXN = 0x4e584e43;  // "CNXN"
constexpr uint32_t A_AUTH = 0x48545541;  // "AUTH"
constexpr uint32_t A_OPEN = 0x4e45504f;  // "OPEN"
constexpr uint32_t A_OKAY = 0x59414b4f;  // "OKAY"
constexpr uint32_t A_CLSE = 0x45534c43;  // "CLSE"
constexpr uint32_t A_WRTE = 0x45545257;  // "WRTE"

constexpr uint32_t A_VERSION   = 0x01000001;
constexpr uint32_t MAX_PAYLOAD_V2 = 0x00100000;  // 1MB

constexpr uint32_t AUTH_TOKEN        = 1;
constexpr uint32_t AUTH_SIGNATURE     = 2;
constexpr uint32_t AUTH_RSAPUBLICKEY  = 3;
```

### 1. 校验和：字节求和，非 CRC32

ADB 的 `data_check` 是对 payload 逐字节求和，不是 CRC32。字段名叫 `data_crc32` 但实际算法是 32-bit wrapping sum。

```cpp
static uint32_t checksum(const uint8_t* data, uint32_t len) {
    uint32_t sum = 0;
    for (uint32_t i = 0; i < len; i++) sum += data[i];
    return sum;
}
```

### 2. Header(24B) 和 Data 必须分开两次 USB bulk write

魅族设备（可能包括其他厂商）要求 ADB header 和 payload 作为两次独立的 `libusb_bulk_transfer` 调用，不能合并为一次传输。

```cpp
// adb_transport.cpp — send()
if (!writeExact(&msg, sizeof(msg), 5000)) return false;  // header 24B
if (data && data_len > 0) {
    if (!writeExact(data, data_len, 5000)) return false;  // payload
}
```

### 3. Banner 不含 NUL 终止符

A_CNXN 的 banner 字段 `data_length` 应等于字符串长度，**不要 +1**（与 openChannel 不同）。

```cpp
// Banner: data_length = banner.size()
send(A_CNXN, A_VERSION, maxdata, banner.c_str(), (uint32_t)banner.size());

// Destination: data_length = strlen + 1 (含 NUL)
send(A_OPEN, local_id, 0, dest.c_str(), (uint32_t)strlen(dest) + 1);
```

### 4. RSA 签名：原始 token 不哈希

adb.exe 将设备发来的 20 字节 token **直接**放入 PKCS#1 v1.5 DigestInfo 的 OCTET STRING，**不做 SHA1 哈希**。这是 POC 阶段最关键发现。

```
PKCS#1 v1.5 填充块 (256 bytes for 2048-bit RSA):
  00 01 FF FF ... FF 00 <DigestInfo: 35 bytes>

DigestInfo (35 bytes):
  30 21          SEQUENCE (33 bytes)
  30 09          SEQUENCE (9 bytes)
  06 05 2B 0E 03 02 1A   OID 1.3.14.3.2.26 (SHA-1)
  05 00          NULL
  04 14          OCTET STRING (20 bytes)
  <原始 20 字节 token>    ← 不是 SHA1(token)！
```

参考实现 `src/adb/adb_rsa.cpp → signToken()`：

```cpp
const uint8_t kDigestInfo[] = {
    0x30, 0x21, 0x30, 0x09, 0x06, 0x05, 0x2B, 0x0E,
    0x03, 0x02, 0x1A, 0x05, 0x00, 0x04, 0x14
};
// 构建 256 字节填充块
std::vector<uint8_t> padded(256, 0xFF);
padded[0] = 0x00; padded[1] = 0x01;
int di_start = 256 - 1 - 15 - 20; // = 220
padded[di_start] = 0x00;
memcpy(&padded[di_start + 1], kDigestInfo, 15);
memcpy(&padded[di_start + 1 + 15], token, 20);

// BCryptDecrypt(PAD_NONE) 做裸 RSA 私钥操作
BCryptDecrypt(m_key, padded, 256, nullptr, nullptr, 0,
              sig, 256, &sigSize, BCRYPT_PAD_NONE);
```

### 5. 握手流程

**已授权（免弹窗）：**
```
host → A_CNXN(banner)
                   ← A_AUTH(TOKEN, 20 bytes)
host → A_AUTH(SIGNATURE, 256 bytes)
                   ← A_CNXN (直连)
```

**首次授权：**
```
host → A_CNXN(banner)
                   ← A_AUTH(TOKEN, 20 bytes)
host → A_AUTH(SIGNATURE, 256 bytes)
                   ← A_AUTH(TOKEN, 20 bytes)   ← 要求公钥
host → A_AUTH(RSAPUBLICKEY, 720 bytes)
                   ← 设备弹窗"允许USB调试"
                   ← A_CNXN
```

### 6. AUTH_RSAPUBLICKEY 格式

720 字节，无长度前缀：
```
base64(RSAPublicKey 524字节) + " " + user@host + '\0'
```

RSAPublicKey 结构（524 bytes, Little-Endian）：
```
uint32_t key_size;     // 64 (= 2048 bits / 32)
uint32_t n0inv;        // -n[0]⁻¹ mod 2^32 (Montgomery inverse)
uint32_t modulus[64];  // n, LE uint32
uint32_t rr[64];       // 2⁴⁰⁹⁶ mod n (Montgomery R²)
uint32_t exponent;     // e, LE uint32
```

n0inv 计算（Newton 迭代 5 轮）：
```cpp
static uint32_t modinv32(uint32_t a) {
    uint32_t x = 1;
    for (int i = 0; i < 5; i++) x = x * (2 - a * x);
    return (uint32_t)(-(int32_t)x);
}
```

rr 计算：`r = 2^2048 - n`，加倍 2048 次，每次 `>= n` 时减 n。
参考实现：`src/adb/adb_rsa.cpp → buildRsaPublicKey()`

### 7. openChannel 返回 msg.arg0

设备响应 A_OPEN 时：
- `arg0` = 设备分配的通道 ID（= remote_id）
- `arg1` = 我们的 local_id（回显）

```cpp
// 正确: return msg.arg0;
// 错误: return msg.arg1;  // 导致 A_WRTE 发往错误的 remote_id
```

### 8. 通道协议

```
host → A_OPEN(local, 0, "tcp:1081")
                 ← A_OKAY(device_local, local)

host → A_WRTE(local, device_local, data)
                 ← A_OKAY(device_local, local)      ← ack host write
                 ← A_WRTE(device_local, local, response)
host → A_OKAY(local, device_local)                 ← ack device write

关闭:
  host → A_CLSE(local, device_local)
                 ← A_CLSE(device_local, local)
```

### 9. Channel 关闭与 AdbReader 交互

closeChannel 流程（V2）：
1. lock ch.mtx，设置 `draining = true`（阻止 AdbReader 继续入队）
2. 从 ChannelMap 移除（AdbReader 之后的查找返回 end）
3. send A_CLSE（USB write mutex）
4. AdbReader 读到响应 A_CLSE → 查找 ChannelMap → 未找到 → 丢弃

注意：V1 的 closeChannel 自己 recv 排空残留消息，V2 由 AdbReader 统一处理。

### 10. 密钥管理

```
加载优先级:
  1. %USERPROFILE%\.android\molink_key.bin  (BCrypt blob, 自生成)
  2. %USERPROFILE%\.android\adbkey           (PKCS#8 PEM → DER → BCryptImportKeyPair)

AUTH_RSAPUBLICKEY 公钥来源:
  优先: %USERPROFILE%\.android\adbkey.pub (adb 已授权，免弹窗)
  回退: 从密钥参数自建 RSAPublicKey 结构体 (需要设备弹窗授权)
```

`loadPkcs8()` 流程：从 adbkey 的 PKCS#8 DER 提取 n, e, d, p, q, dp, dq, qinv，组装 `BCRYPT_RSAFULLPRIVATE_BLOB`，`BCryptImportKeyPair` 导入。参考 `src/adb/adb_rsa.cpp`。

---

## 踩坑记录

| # | 现象 | 根因 | 修复 | 发现阶段 |
|---|------|------|------|---------|
| 1 | Bulk read 始终超时 | 校验和用了 CRC32，设备校验失败后丢包 | 改为字节求和 `sum(data_bytes)` | POC |
| 2 | 设备不响应握手 | header+data 合并为单次 USB write | 分两次独立 write | POC |
| 3 | 每次连接都弹授权窗 | `sha1(token)` ≠ 原始 token，签名不匹配 | 原始 token 直接放入 DigestInfo | POC |
| 4 | A_WRTE 后设备无响应 | openChannel 返回 `msg.arg1`（自己ID）而非 `msg.arg0`（设备ID） | 改为 `msg.arg0` | POC |
| 5 | 连续请求第 3 次起失败 | closeChannel 发 A_CLSE 后不消耗设备响应，残留消息污染下次 openChannel | closeChannel 循环 recv 直到 A_CLSE | V1 |
| 6 | 错误密码后再正确密码连不上 | 同上 + 设备 SOCKS5 worker 未重置 | closeChannel drain 不提早退出 + runLoop 加 300ms delay | V1 |
| 7 | 并发打开通道全部失败 | openChannel 的 recv 和 AdbReader 抢 USB 读，A_OKAY 被 AdbReader 丢弃 | PendingOpen 机制：AdbReader 分发 A_OKAY 给等待者 | V2 |
| 8 | apush 的文件在设备上变成目录 | 文件名 base64 用标准字母表，编码出的 `/` 被 adbd 当作路径分隔符 | `base64Encode` 改用 URL-safe 字母表（`-` `_`），`base64Decode` 兼容两套 | V2 |
| 9 | apush/apull 后文件 mtime 全部丢失 | (a) 目录走 zip 时 `LocalHdr/CentralHdr.mtime=mdate=0` 写死，没有 UT 扩展；`extractZip` 完全无视 header 里的时间字段。(b) 单文件 pull 没问设备 mtime，写完本地文件后也没 `SetFileTime` | (a) `ZipWriter::addFile` 加 mtime 形参，写 DOS mtime/mdate + PKZIP UT 扩展（0x5455，flag 0x01），`extractZip` 读 UT 扩展并 `SetFileTime`。(b) `adb_shell.cpp` 加 `getRemoteMtime()`（`stat -c '%Y'`），pull 前独立 shell 通道取 mtime，写完后 `SetFileTime` 还原 | V2 |
| 10 | apull 选 0 直接卡死（60s 双超时） | 一版曾用 `syncStat()`（走 sync: 通道）拿 mtime，但 Flyme 等厂商 adbd 对 STAT 响应格式不一致，残留字节在 `ch->syncBuf` 错位后续帧，导致 `syncReadResponse` 永远拼不齐合法 SyncMsg，60s 双超时。Flyme（M1852）上复现 | 撤掉 sync: 上的 STAT，改走 shell: 通道（`getRemoteMtime()`）——shell 通道独立，syncBuf 不被污染 | V2 |

---

## 命令行参考

```
molink                              显示帮助
molink run     [options]            前台运行
molink start   [options]            后台守护进程
molink stop                         停止守护进程
molink status                       查询守护进程状态
molink devices                      列出 ADB 设备 + 授权状态
molink auth    [-s serial]          触发设备授权弹窗
molink forward [options]            启动端口转发（需 daemon 运行）
molink push    <local> <remote>     上传文件/目录
molink pull    <remote> <local>     下载文件/目录
molink apush   <path> [--rdir R]    自动上传（zip + .gitignore）
molink apull   [--rdir R]           交互式自动下载 + 解压
molink adel    [--rdir R]           交互式删除远程文件
molink ls      [remote_path]        列出设备目录
molink del     <remote_path>        删除设备文件
molink shell   <command>            执行 shell 命令
molink -v / --version               显示版本号
```

### 参数

| 参数 | 简写 | 默认值 | 说明 |
|------|------|--------|------|
| `--port` | `-p` | 1080 | 本地 TCP 监听端口 |
| `--rport` | `-r` | 1081 | 设备目标端口 |
| `--serial` | `-s` | 第一个设备 | 指定设备序列号 |
| `--rdir` | — | /sdcard/tmp | 远程目录 (apush/apull/adel) |
| `--help` | `-h` | — | 帮助 |

### 示例

```powershell
# 后台守护 + 转发（分离模式）
.\molink.exe start
.\molink.exe forward -p 1080 -r 1081
.\molink.exe status
.\molink.exe stop

# 设备授权
.\molink.exe auth
# 设备弹出授权对话框 → 点"允许"

# 文件传输
.\molink.exe push .\test.txt /sdcard/test.txt
.\molink.exe pull /sdcard/test.txt .\downloaded.txt

# 目录操作
.\molink.exe push .\myfolder /sdcard/myfolder
.\molink.exe pull /sdcard/myfolder .\downloaded_folder

# 自动打包上传（支持 .gitignore）
.\molink.exe apush .\project --rdir /sdcard/tmp
.\molink.exe apull
.\molink.exe adel

# shell 命令
.\molink.exe ls /sdcard/
.\molink.exe shell "getprop ro.product.model"
```

### devices 输出示例

```
Key: D:\project\...\build\molink_key.bin (found)

#    SERIAL                 AUTH
---  ---------------------- ----
0    RFCWA0K5W8W            yes
```

### status 输出示例

```
# daemon 运行中
daemon=running state=connected serial=RFCWA0K5W8W forwarding=1080->1081 connections=2

# daemon 未运行
daemon=stopped
```

---

## 进程模型

### 前台模式 (`molink run`)

```
main() → foregroundMode()
  ├── AdbClient.connect()
  ├── Forwarder.start() → acceptLoop (主线程)
  │     └── std::thread(relay, client).detach() × N
  ├── SetConsoleCtrlHandler (Ctrl+C → graceful stop)
  └── while(isRunning) Sleep(500)
```

### 后台模式 (`molink start`)

```
main() → cmdStart()
  ├── 已有 daemon? → sendPipeCmd("stop") → Sleep(1.5s)
  ├── CreateProcess(molink.exe --daemon ..., DETACHED_PROCESS)
  ├── 轮询 8s: sendPipeCmd("status") → 等待 "connected"
  └── 超时/进程退出 → 报 FAIL

子进程 → daemonMain()
  ├── tryAcquireDaemonLock() (Global\MoLinkDaemon)
  ├── freopen(molinkd.log) → 日志重定向
  ├── AdbClient.connect()
  ├── Forwarder.start()
  ├── NamedPipeServer.start() (\\.\pipe\molink)
  ├── writePidFile() (molinkd.pid)
  └── 事件循环: WaitForMultipleObjects(hStopEvent, hPipeEvent)
       ├── hStopEvent → 退出
       └── hPipeEvent → processConnection()
            ├── "stop"  → SetEvent(hStopEvent) → "ok"
            └── "status" → 返回设备状态
```

### 停止 (`molink stop`)

```
main() → cmdStop()
  ├── sendPipeCmd("stop")
  ├── 响应 "ok" → 正常退出
  └── 无响应 → readPidFile() → OpenProcess() → TerminateProcess()
```

### 单实例保证

- `CreateMutex("Global\\MoLinkDaemon")` — 跨进程互斥
- PID 文件 `<exe_dir>\molinkd.pid` — 用于 force kill
- Named Pipe `\\.\pipe\molink` — 消息模式，命令通道

---

## 编译与依赖

### 编译环境

- Windows 10/11
- MinGW-w64 64-bit (测试: w64devkit x64, GCC 12.1.0)
- CMake 3.14+

### 编译步骤

```powershell
cd molink-access
mkdir build && cd build
cmake .. -G "MinGW Makefiles"
mingw32-make
```

产物 `build\molink.exe`，**零第三方 DLL 依赖**。libusb 已静态编译进 exe。

### 依赖库

程序依赖全部为 Windows 自带 DLL：

```
ntdll.dll, KERNEL32.DLL, ADVAPI32.dll, USER32.dll, SHELL32.dll,
CRYPT32.dll, bcrypt.dll, WS2_32.dll, ole32.dll, setupapi.dll, winusb.dll,
msvcrt.dll, ucrtbase.dll
```

链接库：
- `ws2_32` — Windows Sockets (TCP 监听)
- `bcrypt` — Windows CNG (RSA 签名)
- `crypt32` — Windows Crypto API (Base64, PKCS#8 解析)
- `winusb` — WinUSB (libusb 后端)
- `setupapi` — Setup API (libusb 设备枚举)
- `ole32` — COM (libusb 后端)

### libusb 静态库编译

项目使用 libusb v1.0.28 静态库（`third_party/libusb/lib/libusb-1.0.a`）。如需重新编译：

```powershell
# 1. 下载源码
# https://github.com/libusb/libusb/releases
curl -L -o libusb.tar.bz2 https://github.com/libusb/libusb/releases/download/v1.0.28/libusb-1.0.28.tar.bz2

# 2. 解压 + 配置
tar xf libusb.tar.bz2
cd libusb-1.0.28
./configure --host=x86_64-w64-mingw32 --enable-static --disable-shared
make -j4

# 3. 复制到项目
cp libusb/.libs/libusb-1.0.a ../third_party/libusb/lib/
cp libusb/libusb.h ../third_party/libusb/include/
```

源码地址：https://github.com/libusb/libusb

---

## 文件结构

```
molink-access/
├── CMakeLists.txt                    # MinGW Makefiles, C++17, 日志开关
├── clean_build.ps1                   # 一键构建脚本
├── version.cmake                     # 版本号生成脚本
├── README.md
├── third_party/
│   └── libusb/
│       ├── include/
│       │   └── libusb.h             # libusb v1.0.28 头文件
│       └── lib/
│           └── libusb-1.0.a         # libusb 静态库
└── src/
    ├── main.cpp                     # CLI 入口 + 命令分发
    ├── version.h/cpp                # 版本号输出
    ├── usb/
    │   └── usb_device.h/cpp         # libusb 封装
    │       - discover()             # 扫描 USB，匹配 ADB 接口 (class=0xFF,sub=0x42,proto=0x01)
    │       - open() / close()       # 打开/关闭设备 + claim interface
    │       - getSerial()            # 读取 USB 字符串描述符（支持临时打开）
    │       - bulkRead() / bulkWrite() # libusb_bulk_transfer 封装
    │       - clearHalt() / drainRead() # 清端点 + 排空残留
    ├── adb/
    │   ├── adb_transport.h/cpp      # ADB 协议层
    │   │   - AdbMessage (24B)       # 消息结构 + pack(1)
    │   │   - send() / recv()        # 消息收发 + magic 校验（payload 30s 超时）
    │   │   - handshake()            # RSA 握手（3 轮循环）
    │   │   - openChannel()          # A_OPEN → A_OKAY
    │   │   - closeChannel()         # A_CLSE
    │   │   - readChannel() / writeChannel()
    │   ├── adb_rsa.h/cpp            # RSA 密钥 + 签名
    │   │   - generateKey()          # BCryptGenerateKeyPair 2048-bit
    │   │   - loadKey() / saveKey()  # BCrypt blob 文件存取（含缓存填充）
    │   │   - signToken()            # PKCS#1 v1.5 填充 + BCryptDecrypt(PAD_NONE)
    │   │   - buildRsaPublicKey()    # 计算 n0inv + rr，构建 524B RSAPublicKey
    │   │   - getPubKeyPayload()     # base64 + user@host + '\0'
    │   │   - getDefaultKeyPath()    # 返回 exe 同级 molink_key.bin 路径
    │   │   - hadProtocolError()     # 协议错误检测（bad magic 等）
    │   ├── adb_reader.h/cpp         # USB 读线程 + 消息分发
    │   │   - readLoop()             # recv() 100ms → Channel/PendingOpen 分发
    │   │   - Channel                # dataQueue + cv + closed/draining flag
    │   │   - PendingOpen            # openChannel 等待 AdbReader 分发 A_OKAY
    │   │   - 协议/硬件错误触发 disconnect callback
    │   ├── adb_client.h/cpp         # 高层客户端
    │   │   - loadOrGenerateKey()    # 加载或自动生成 molink_key.bin
    │   │   - connect() / disconnect() # 设备 open + drain + RSA 握手（3 次重试，含重试后 drain）
    │   │   - openChannel()          # PendingOpen 机制
    │   │   - closeChannel()         # draining flag + A_CLSE
    │   │   - writeChannel()         # 线程安全（write mutex）
    │   ├── adb_sync.h/cpp           # SYNC 协议（文件传输）
    │   └── adb_shell.h/cpp          # Shell 命令执行
    ├── cli/
    │   ├── cli_utils.h/cpp          # 公共定义（RemoteFile 解析、sendPipeCmd 声明）
    │   ├── cli_auth.cpp             # 设备授权（USB 扫描 + RSA 握手）
    │   ├── cli_devices.cpp          # 设备列表（daemon pipe 优先 + USB 回退）
    │   ├── cli_forward.cpp          # 端口转发 CLI
    │   ├── cli_apush.cpp            # 自动打包上传（zip + gitignore）
    │   ├── cli_apull.cpp            # 交互式下载 + 解压
    │   ├── cli_adel.cpp             # 交互式删除
    │   ├── cli_del.cpp              # 单文件删除
    │   └── named_pipe.h/cpp         # Named Pipe Server
    ├── daemon/
    │   └── daemon_app.h/cpp         # 守护进程主类
    │       - run()                  # 事件循环（stop / pipe / disconnect）
    │       - onPipeCommand()        # 管道命令处理（push/pull/forward/ls/del/shell）
    │       - transitionToDisconnected() / tryReconnectDevice()
    ├── forward/
    │   └── forwarder.h/cpp          # TCP 端口转发器
    │       - acceptLoop()           # 接受 TCP 连接（detached relay 线程）
    │       - relay()                # 双向数据转发（优先排空队列，非阻塞轮询）
    │       - pause() / resume()     # 设备断连时暂停/恢复
    ├── transfer/
    │   ├── file_push.cpp            # 文件上传
    │   ├── file_pull.cpp            # 文件下载
    │   └── file_list.cpp            # 目录列表
    └── utils/
        └── log.h                    # 日志宏（LOG_INFO/DEBUG/WARN/ERROR）
``
    └── forward/
        └── forwarder.h/cpp          # TCP 端口转发器
            - start() / stop()       # listen + acceptLoop 线程
            - acceptLoop()           # accept → detach relay thread
            - relay()                # select(100ms) + cv.wait_for(50ms) 双向转发
            - tryAcquireSlot()       # 并发控制 (max 16)
```

---

## 设备与环境

### 测试设备

| 项目 | 值 |
|------|-----|
| 型号 | 魅族 M1852 (Flyme OS) |
| VID/PID | 0x2A45 / 0x4EE7 |
| ADB 接口 | class=0xFF, subclass=0x42, protocol=0x01 |
| 端点 | read_ep=0x81, write_ep=0x01, maxpkt=512 |
| 序列号 | 852QLDV923XMM |
| 设备端口 | tcp:1081 (SOCKS5 proxy, auth: socks5/password123) |

### 驱动要求

设备 ADB 接口需替换为 WinUSB 驱动（使用 [Zadig](https://zadig.akeo.ie/) 工具）。替换后 `libusb_claim_interface` 才能成功。

### worker 端

设备运行 molink-worker (Android)，提供 SOCKS5 代理服务：
- 协议：SOCKS5 / SOCKS5h（支持域名远端解析）
- 认证：用户名/密码 (socks5/password123)
- 端口：1081
