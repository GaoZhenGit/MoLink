# MoLink Access C++ 重写设计（libusb 方案）

> **Status:** 设计阶段
> **Date:** 2026-04-30
> **Goal:** 用 C++17 + libusb 重写 molink-access，直接通过 WinUSB 与 Android 设备通信，不依赖 AdbWinApi.dll

## 背景

上一版方案（`2026-04-30-molink-cpp-rewrite-design.md`）基于动态加载 AdbWinApi.dll 实现 USB 通信。**POC 已失败**：DLL 加载和设备枚举正常，但 `AdbReadEndpointSync` / `AdbWriteEndpointSync` 写入数据后设备完全无响应，而 adb.exe 使用同一个 DLL 可以正常工作。

根因分析：adb.exe 配合 `androidusb.sys` 内核驱动使用，AdbWinApi.dll 内部依赖该驱动的设备声明机制。我们直接调用 DLL 缺少关键的驱动前置步骤，导致 USB 端点不响应。

**新方向**：绕过 AdbWinApi.dll，使用 **libusb** 直接通过 WinUSB 后端与设备通信。libusb 会自动处理 WinUSB 驱动关联（通过 Zadig 或驱动重绑定），无需 androidusb.sys。

AOSP adb 源码中 `client/usb_libusb.cpp` 即采用此路线，已在 Linux/macOS/Windows 上大规模验证。

## 技术栈

- **语言/标准**: C++17
- **编译器**: MinGW GCC 32-bit (`D:/software/w64devkit/x86`)
- **构建**: CMake 3.14+
- **USB 库**: libusb 1.0.29, MinGW32 预编译 DLL + 头文件
- **平台**: Windows, Winsock2

## 架构

```
┌──────────────────────────────────────────┐
│               poc_main.cpp               │
│           (设备发现→握手→通道测试)          │
├──────────────────────────────────────────┤
│  adb_transport.cpp/h    (复用+改造)       │
│  ADB 消息打包/解包/CRC32/握手/通道管理     │
│  改造点：读写回调改为 UsbDevice 引用        │
├──────────────────────────────────────────┤
│  usb_device.cpp/h       (新建)           │
│  libusb 封装：设备发现/打开/序列号/Bulk读写 │
├──────────────────────────────────────────┤
│            libusb-1.0.dll                │
│       (MinGW32 预编译, WinUSB后端)        │
└──────────────────────────────────────────┘
```

**关键变化**：用 `UsbDevice` 类替换 AdbDll + AdbWinApi 函数指针。`AdbTransport` 不再持有 `m_read`/`m_write` 函数指针，改为直接使用 `UsbDevice` 的 `bulkRead()`/`bulkWrite()` 方法。

### 文件结构

```
molink-access-cpp/
├── CMakeLists.txt
├── libs/
│   ├── libusb-1.0.dll          # MinGW32 运行时 DLL
│   └── libusb-1.0.dll.a        # MinGW32 导入库
├── vendor/
│   └── libusb/
│       └── libusb.h            # libusb API 头文件
└── src/
    ├── poc_main.cpp            # POC 入口
    ├── adb/
    │   ├── adb_transport.h     # ADB 消息结构/协议常量
    │   └── adb_transport.cpp   # ADB 消息打包/解包/CRC32/握手
    └── usb/
        ├── usb_device.h        # libusb 封装接口
        └── usb_device.cpp      # 设备发现/打开/序列号/Bulk读写
```

### 数据流

```
poc_main()
  → UsbDevice::discover()           // libusb_get_device_list + 匹配 ADB class
  → UsbDevice::open()               // libusb_open + claim_interface(AID)
  → UsbDevice::getSerial()          // libusb_get_string_descriptor
  → UsbDevice::bulkRead/Write()     // libusb_bulk_transfer
  → AdbTransport::handshake()       // A_CNXN → A_AUTH → A_CNXN
  → AdbTransport::openChannel()     // A_OPEN → A_OKAY
```

## 组件设计

### UsbDevice（新建）

职责：封装 libusb 的 USB 设备操作，提供简洁的 C++ 接口。

```cpp
class UsbDevice {
public:
    // 扫描所有 Android ADB 接口设备
    static std::vector<UsbDevice> discover();

    UsbDevice(libusb_device* dev, libusb_context* ctx);
    ~UsbDevice();

    // 打开设备，声明 ADB 接口
    bool open();

    // 获取序列号（ASCII，与 adb 行为一致 ansi=true 模式）
    std::string getSerial();

    // 获取 USB 描述符信息（VID/PID/接口类/Bulk 端点地址）
    uint16_t getVendorId() const;
    uint16_t getProductId() const;
    uint8_t  getReadEndpoint() const;   // Bulk IN 端点地址
    uint8_t  getWriteEndpoint() const;  // Bulk OUT 端点地址

    // 同步 Bulk 读写
    bool bulkRead(void* buf, int len, int* transferred, int timeout_ms);
    bool bulkWrite(const void* buf, int len, int* transferred, int timeout_ms);

    void close();
    bool isOpen() const;

private:
    libusb_device*      m_device;
    libusb_device_handle* m_handle = nullptr;
    libusb_context*     m_ctx;
    uint8_t             m_read_ep;   // 0x81 典型值
    uint8_t             m_write_ep;  // 0x01 典型值
    int                 m_interface_number;
};
```

**discover() 实现要点**：
- 遍历 `libusb_get_device_list()` 获取所有 USB 设备
- 对于每个设备，读取设备描述符（VID/PID）和配置描述符
- 匹配 ADB 接口：class=0xFF, subclass=0x42, protocol=0x01
- 找到后记录接口号和端点地址

**open() 实现要点**：
- `libusb_open()` 获取设备句柄
- 如果内核驱动已声明接口，调用 `libusb_detach_kernel_driver()` 释放
- `libusb_claim_interface()` 声明 ADB 接口
- Windows 上默认驱动为 WinUSB，通常无需 detach

**adb.exe 对 Windows 上的做法**：adb.exe 使用 `winusb` 而非 `libusb`。但识别 ADB 接口的参数完全一致（class/subclass/protocol = 0xFF/0x42/0x01）。

### AdbTransport（改造）

**现有实现保持不变的部分**：
- ADB 消息结构（`AdbMessage`, `A_CNXN`, `A_OPEN` 等）
- CRC32 计算
- `send()` / `recv()` 方法
- `handshake()` 握手流程

**改造点**：
- 移除 `PFN_AdbReadEndpointSync` / `PFN_AdbWriteEndpointSync` 函数指针
- 构造函数改为接收 `UsbDevice&` 引用
- `readExact()` / `writeExact()` 内部调用 `m_device.bulkRead()` / `m_device.bulkWrite()`

### libusb 依赖

**获取方式**：从 [libusb GitHub Releases](https://github.com/libusb/libusb/releases) 下载 `libusb-1.0.29.7z`

**提取文件**：
| 源路径 (7z内) | 目标路径 | 用途 |
|---|---|---|
| `include/libusb.h` | `vendor/libusb/libusb.h` | 编译时头文件 |
| `MinGW32/dll/libusb-1.0.dll` | `libs/libusb-1.0.dll` | 运行时动态库 |
| `MinGW32/static/libusb-1.0.dll.a` | `libs/libusb-1.0.dll.a` | 链接时导入库 |

**CMake 配置**：
```cmake
include_directories(${CMAKE_SOURCE_DIR}/vendor/libusb)
link_directories(${CMAKE_SOURCE_DIR}/libs)
target_link_libraries(molink libusb-1.0.dll.a ws2_32)
```

## POC 门禁标准

POC 成功标准（与上一版一致）：

1. ✅ 能发现已连接的 Android 设备
2. ✅ 能打开设备并获取 USB 序列号
3. ✅ 能发送 A_CNXN 并收到设备响应（A_CNXN 或 A_AUTH）
4. ✅ 能打开 TCP 通道（A_OPEN "tcp:1080" → A_OKAY）

## Phase 2 概要（POC 通过后）

- RSA 认证（读取 `%USERPROFILE%\.android\adbkey` 私钥，SHA1+RSA 签名）
- 守护进程主循环（设备轮询、自动重连）
- Named Pipe CLI 服务器
- 端口转发器（本地 TCP → ADB 通道 → 设备端口）
- 多设备支持

## 风险与缓解

| 风险 | 影响 | 缓解措施 |
|---|---|---|
| WinUSB 驱动未关联到 ADB 接口 | USB 设备打不开 | 用 Zadig 工具将设备接口驱动替换为 WinUSB |
| libusb MinGW32 DLL 不兼容 w64devkit | 链接或运行时错误 | 备选：从源码用 MinGW 编译 libusb |
| 多设备竞争 ADB 接口声明 | claim_interface 失败 | 检查并 detach 内核驱动后重试 |
| 设备连接/断开检测延迟 | 轮询间隔不准确 | POC 阶段不处理，Phase 2 解决 |
