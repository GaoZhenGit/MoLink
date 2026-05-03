# MoLink Access (C++)

通过 USB 连接 Android 设备，实现 ADB 协议通信，将本地 TCP 端口转发到设备的 SOCKS5 代理。

## 架构

```
main.cpp
  ├── AdbClient                          # 设备生命周期管理
  │     ├── UsbDevice                    # libusb 封装（发现/打开/读写）
  │     ├── AdbRsa                       # BCrypt RSA（密钥生成/导入/签名）
  │     └── AdbTransport                 # ADB 协议（握手/通道/数据收发）
  └── Forwarder（独立线程）               # TCP → ADB 转发
        └── accept → relay（双向中继）
```

### 数据流

```
应用 (curl --socks5 127.0.0.1:1080)
  → TCP localhost:1080 (Winsock2 accept)
    → Forwarder relay (select + 50ms poll)
      → AdbTransport: A_WRTE ↔ A_OKAY
        → UsbDevice: libusb bulk read/write
          → USB cable
            → 设备 adbd → worker SOCKS5 → 互联网
```

## 编译

**环境要求：** MinGW-w64, CMake 3.14+, libusb-1.0

```powershell
cd molink-access-cpp
mkdir build && cd build
cmake .. -G "MinGW Makefiles"
mingw32-make
```

## 使用

```powershell
.\molink.exe                           # 默认: 127.0.0.1:1080 → device:1081
.\molink.exe -p 2080 -r 1081           # 自定义端口
.\molink.exe -s 852QLDV923XMM           # 指定设备
```

| 参数 | 简写 | 默认值 | 说明 |
|------|------|--------|------|
| `--port` | `-p` | 1080 | 本地 TCP 端口 |
| `--rport` | `-r` | 1081 | 设备目标端口 |
| `--serial` | `-s` | (第一个设备) | 设备序列号 |
| `--help` | `-h` | — | 显示帮助 |

**代理测试：**
```powershell
curl --socks5 127.0.0.1:1080 --proxy-user socks5:password123 https://www.baidu.com
curl --socks5-hostname 127.0.0.1:1080 --proxy-user socks5:password123 https://www.baidu.com
```

## 关键技术细节

- **校验和**：ADB data_check 是字节求和，非 CRC32
- **Header/Data 分离**：ADB header(24B) 和 payload 须分两次 USB bulk write
- **Banner**：不含 NUL 终止符（`data_length = banner.size()`）
- **RSA 签名**：原始 20 字节 token 直接放入 PKCS#1 v1.5 DigestInfo，不做 SHA1
- **BCrypt**：Windows CNG，`BCryptDecrypt(PAD_NONE)` 做裸 RSA 签名
- **openChannel**：设备响应的 `msg.arg0` 是 remote_id（不是 arg1）
- **closeChannel**：发送 A_CLSE 后须循环 recv 消耗残留消息直到收到设备 A_CLSE

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
    ├── main.cpp                     # CLI 入口
    ├── usb/
    │   ├── usb_device.h             # libusb 封装
    │   └── usb_device.cpp
    ├── adb/
    │   ├── adb_transport.h          # ADB 协议
    │   ├── adb_transport.cpp
    │   ├── adb_rsa.h                # RSA 密钥
    │   ├── adb_rsa.cpp
    │   ├── adb_client.h             # 高层客户端
    │   └── adb_client.cpp
    └── forward/
        ├── forwarder.h              # 端口转发
        └── forwarder.cpp
```
