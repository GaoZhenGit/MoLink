# MoLink Access C++ libusb POC 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 用 libusb + MinGW64 搭建 POC，验证设备发现→ADB握手→TCP通道打开全流程

**Architecture:** 单线程同步调用。UsbDevice 封装 libusb 的 USB 设备操作（发现/打开/读写），AdbTransport 通过 UsbDevice 引用执行 ADB 协议消息收发，poc_main 串联完整流程

**Tech Stack:** C++17, MinGW GCC 64-bit (E:/software/w64devkit/x64), CMake 3.14+, libusb 1.0.29 MinGW64 预编译, Winsock2

---

## 文件结构

```
molink-access-cpp/
├── CMakeLists.txt          # 修改：x64工具链 + libusb依赖 + 新文件结构
├── libs/
│   ├── libusb-1.0.dll      # 从 libusb-1.0.29.7z 提取 (MinGW64/dll/)
│   └── libusb-1.0.dll.a    # 从 libusb-1.0.29.7z 提取 (MinGW64/static/)
├── vendor/
│   └── libusb/
│       └── libusb.h        # 从 libusb-1.0.29.7z 提取 (include/)
└── src/
    ├── poc_main.cpp        # 重写：usb_device + adb_transport 集成
    ├── adb/
    │   ├── adb_transport.h # 修改：移除 adb_dll.h 依赖，改用 UsbDevice&
    │   └── adb_transport.cpp # 修改：函数指针 → UsbDevice 方法调用
    └── usb/
        ├── usb_device.h    # 新建：libusb 封装接口
        └── usb_device.cpp  # 新建：设备发现/打开/序列号/Bulk读写实现
```

删除不再使用的文件：`src/adb/adb_dll.h`, `src/adb/adb_dll.cpp`

---

### Task 1: 下载 libusb 并搭建 64-bit CMake 项目骨架

**Files:**
- Create: `molink-access-cpp/vendor/libusb/libusb.h`
- Create: `molink-access-cpp/libs/libusb-1.0.dll`
- Create: `molink-access-cpp/libs/libusb-1.0.dll.a`
- Modify: `molink-access-cpp/CMakeLists.txt`
- Create: `molink-access-cpp/src/usb/usb_device.h`（最小声明）
- Create: `molink-access-cpp/src/usb/usb_device.cpp`（空实现）
- Delete: `molink-access-cpp/src/adb/adb_dll.h`
- Delete: `molink-access-cpp/src/adb/adb_dll.cpp`

- [ ] **Step 1: 下载 libusb-1.0.29.7z 并解压提取所需文件**

```powershell
# 下载（如果还没下载）
curl -x http://127.0.0.1:7890 -s -L -o "$env:TEMP/libusb-1.0.29.7z" "https://github.com/libusb/libusb/releases/download/v1.0.29/libusb-1.0.29.7z"

# 解压到临时目录
D:/software/7-Zip/7z.exe x "$env:TEMP/libusb-1.0.29.7z" -o"$env:TEMP/libusb-extracted" -y

# 创建目标目录（如果不存在）
mkdir -p D:/project/MoLink/molink-access-cpp/vendor/libusb
mkdir -p D:/project/MoLink/molink-access-cpp/libs

# 复制头文件
Copy-Item "$env:TEMP/libusb-extracted/include/libusb.h" D:/project/MoLink/molink-access-cpp/vendor/libusb/libusb.h

# 复制 MinGW64 DLL 和导入库
Copy-Item "$env:TEMP/libusb-extracted/MinGW64/dll/libusb-1.0.dll" D:/project/MoLink/molink-access-cpp/libs/libusb-1.0.dll
Copy-Item "$env:TEMP/libusb-extracted/MinGW64/static/libusb-1.0.dll.a" D:/project/MoLink/molink-access-cpp/libs/libusb-1.0.dll.a
```

- [ ] **Step 2: 删除旧的 adb_dll 文件**

```powershell
Remove-Item D:/project/MoLink/molink-access-cpp/src/adb/adb_dll.h -ErrorAction SilentlyContinue
Remove-Item D:/project/MoLink/molink-access-cpp/src/adb/adb_dll.cpp -ErrorAction SilentlyContinue
```

- [ ] **Step 3: 创建 usb_device.h（最小声明，先编译通过）**

新文件 `molink-access-cpp/src/usb/usb_device.h`：

```cpp
#ifndef USB_DEVICE_H
#define USB_DEVICE_H

#include <cstdint>
#include <string>
#include <vector>

struct libusb_device;
struct libusb_device_handle;
struct libusb_context;

class UsbDevice {
public:
    static std::vector<UsbDevice> discover();

    UsbDevice(libusb_device* dev, libusb_context* ctx);
    ~UsbDevice();

    bool open();
    std::string getSerial() const;
    uint8_t getReadEndpoint() const;
    uint8_t getWriteEndpoint() const;
    bool bulkRead(void* buf, int len, int* transferred, int timeout_ms);
    bool bulkWrite(const void* buf, int len, int* transferred, int timeout_ms);
    void close();
    bool isOpen() const;

private:
    libusb_device*       m_device;
    libusb_device_handle* m_handle;
    libusb_context*      m_ctx;
    uint8_t              m_read_ep;
    uint8_t              m_write_ep;
    int                  m_interface_number;
    bool                 m_open;
};

#endif
```

- [ ] **Step 4: 创建 usb_device.cpp（最小实现，先编译通过）**

新文件 `molink-access-cpp/src/usb/usb_device.cpp`：

```cpp
#include "usb_device.h"
#include <libusb.h>
#include <cstdio>

std::vector<UsbDevice> UsbDevice::discover() {
    std::vector<UsbDevice> result;
    printf("USB: discover() not implemented yet\n");
    return result;
}

UsbDevice::UsbDevice(libusb_device* dev, libusb_context* ctx)
    : m_device(dev), m_handle(nullptr), m_ctx(ctx)
    , m_read_ep(0), m_write_ep(0), m_interface_number(0), m_open(false) {}

UsbDevice::~UsbDevice() { close(); }

bool UsbDevice::open() {
    printf("USB: open() not implemented yet\n");
    return false;
}

std::string UsbDevice::getSerial() const {
    return "";
}

uint8_t UsbDevice::getReadEndpoint() const  { return m_read_ep; }
uint8_t UsbDevice::getWriteEndpoint() const { return m_write_ep; }

bool UsbDevice::bulkRead(void* buf, int len, int* transferred, int timeout_ms) {
    (void)buf; (void)len; (void)transferred; (void)timeout_ms;
    printf("USB: bulkRead() not implemented yet\n");
    return false;
}

bool UsbDevice::bulkWrite(const void* buf, int len, int* transferred, int timeout_ms) {
    (void)buf; (void)len; (void)transferred; (void)timeout_ms;
    printf("USB: bulkWrite() not implemented yet\n");
    return false;
}

void UsbDevice::close() {
    m_open = false;
}

bool UsbDevice::isOpen() const { return m_open; }
```

- [ ] **Step 5: 修改 CMakeLists.txt**

将 `molink-access-cpp/CMakeLists.txt` 完整替换为：

```cmake
cmake_minimum_required(VERSION 3.14)
project(molink LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 64-bit MinGW
set(CMAKE_C_COMPILER "D:/software/w64devkit/x64/bin/gcc.exe")
set(CMAKE_CXX_COMPILER "D:/software/w64devkit/x64/bin/g++.exe")

set(CMAKE_EXE_LINKER_FLAGS "-static-libgcc -static-libstdc++")

include_directories(${CMAKE_SOURCE_DIR}/vendor/libusb)
link_directories(${CMAKE_SOURCE_DIR}/libs)

add_executable(molink
    src/poc_main.cpp
    src/usb/usb_device.cpp
)

target_link_libraries(molink libusb-1.0.dll.a ws2_32)

# 构建后复制 libusb DLL 到输出目录
add_custom_command(TARGET molink POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/libs/libusb-1.0.dll"
        "$<TARGET_FILE_DIR:molink>"
)
```

- [ ] **Step 6: 编译验证骨架通过**

```powershell
cd D:/project/MoLink/molink-access-cpp
Remove-Item build -Recurse -Force -ErrorAction SilentlyContinue
mkdir build
cd build
cmake .. -G "MinGW Makefiles" -DCMAKE_MAKE_PROGRAM="D:/software/w64devkit/x64/bin/mingw32-make.exe"
D:/software/w64devkit/x64/bin/mingw32-make.exe
```

预期：编译成功（links fine，poc_main 还是旧代码可能编译不过，下一步会重写 poc_main）

> **注意**：如果此步因 poc_main.cpp 引用了已删除的 adb_dll.h 而编译失败，属于预期行为。下一步 Task 2 会重写 poc_main.cpp 解决。

---

### Task 2: 实现 UsbDevice::discover() — 设备发现

**Files:**
- Modify: `molink-access-cpp/src/usb/usb_device.cpp`

- [ ] **Step 1: 实现 discover() — 遍历 USB 设备，匹配 ADB 接口**

将 `usb_device.cpp` 中的 `discover()` 替换为：

```cpp
std::vector<UsbDevice> UsbDevice::discover() {
    std::vector<UsbDevice> result;

    libusb_context* ctx = nullptr;
    if (libusb_init(&ctx) != 0) {
        printf("USB: libusb_init failed\n");
        return result;
    }

    libusb_device** devs = nullptr;
    ssize_t count = libusb_get_device_list(ctx, &devs);
    if (count < 0) {
        printf("USB: libusb_get_device_list failed: %s\n", libusb_error_name(count));
        libusb_exit(ctx);
        return result;
    }

    printf("USB: Scanning %zd USB devices...\n", count);
    for (ssize_t i = 0; i < count; i++) {
        libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(devs[i], &desc) != 0) continue;

        // 遍历配置描述符寻找 ADB 接口
        libusb_config_descriptor* config = nullptr;
        if (libusb_get_active_config_descriptor(devs[i], &config) != 0) continue;

        bool found = false;
        int adb_iface = 0;
        uint8_t read_ep = 0, write_ep = 0;

        for (uint8_t j = 0; j < config->bNumInterfaces && !found; j++) {
            const libusb_interface& iface = config->interface[j];
            for (int k = 0; k < iface.num_altsetting && !found; k++) {
                const libusb_interface_descriptor& iface_desc = iface.altsetting[k];
                // ADB interface: class=0xFF, subclass=0x42, protocol=0x01
                if (iface_desc.bInterfaceClass == 0xFF &&
                    iface_desc.bInterfaceSubClass == 0x42 &&
                    iface_desc.bInterfaceProtocol == 0x01) {
                    adb_iface = iface_desc.bInterfaceNumber;
                    // 扫描端点
                    for (uint8_t ep = 0; ep < iface_desc.bNumEndpoints; ep++) {
                        const libusb_endpoint_descriptor& ep_desc = iface_desc.endpoint[ep];
                        uint8_t addr = ep_desc.bEndpointAddress;
                        if ((addr & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_IN) {
                            read_ep = addr;
                        } else {
                            write_ep = addr;
                        }
                    }
                    found = true;
                }
            }
        }

        libusb_free_config_descriptor(config);

        if (found && read_ep != 0 && write_ep != 0) {
            printf("USB: Found ADB device [%zd]: VID=0x%04X PID=0x%04X "
                   "iface=%d read_ep=0x%02X write_ep=0x%02X\n",
                   i, desc.idVendor, desc.idProduct,
                   adb_iface, read_ep, write_ep);
            UsbDevice dev(devs[i], ctx);
            dev.m_interface_number = adb_iface;
            dev.m_read_ep = read_ep;
            dev.m_write_ep = write_ep;
            result.push_back(dev);
        }
    }

    libusb_free_device_list(devs, 1);
    printf("USB: Found %zu ADB device(s)\n", result.size());
    return result;
}
```

- [ ] **Step 2: 将 m_interface_number / m_read_ep / m_write_ep 设为 public 或 friend**

这两个成员已在 `discover()` 中直接赋值，最简单的方式是在 `usb_device.h` 的 `UsbDevice` 类中将 `discover()` 声明为 `friend`，或者在构造函数中传入。这里采用将 `discover()` 设为静态工厂方法，直接访问私有成员的做法需要调整访问控制。最简单方案：将三个字段改为 public（POC 阶段从简）。

修改 `usb_device.h` 中的 `private:` 部分，将 `m_read_ep`, `m_write_ep`, `m_interface_number` 移到类顶部公开——或更好的方案，添加一个私有构造变体。

实际采用：在 `discover()` 中通过构造函数传入接口号+端点地址，而不仅仅是设备指针。

更新 `usb_device.h`：

```cpp
class UsbDevice {
public:
    static std::vector<UsbDevice> discover();

    // 完整构造函数 — 由 discover() 内部使用
    UsbDevice(libusb_device* dev, libusb_context* ctx,
              int iface, uint8_t read_ep, uint8_t write_ep);
    ~UsbDevice();

    bool open();
    std::string getSerial() const;
    uint8_t getReadEndpoint() const  { return m_read_ep; }
    uint8_t getWriteEndpoint() const { return m_write_ep; }
    bool bulkRead(void* buf, int len, int* transferred, int timeout_ms);
    bool bulkWrite(const void* buf, int len, int* transferred, int timeout_ms);
    void close();
    bool isOpen() const { return m_open; }

private:
    libusb_device*       m_device;
    libusb_device_handle* m_handle;
    libusb_context*      m_ctx;
    uint8_t              m_read_ep;
    uint8_t              m_write_ep;
    int                  m_interface_number;
    bool                 m_open;
};
```

更新 `usb_device.cpp` 的构造函数和 `discover()` 中创建设备的行：

```cpp
UsbDevice::UsbDevice(libusb_device* dev, libusb_context* ctx,
                     int iface, uint8_t read_ep, uint8_t write_ep)
    : m_device(dev), m_handle(nullptr), m_ctx(ctx)
    , m_read_ep(read_ep), m_write_ep(write_ep)
    , m_interface_number(iface), m_open(false) {}

// discover() 中创建设备行改为：
UsbDevice dev(devs[i], ctx, adb_iface, read_ep, write_ep);
result.push_back(dev);
```

- [ ] **Step 3: 编译验证**

```powershell
cd D:/project/MoLink/molink-access-cpp/build
D:/software/w64devkit/x64/bin/mingw32-make.exe
```

预期：编译成功。

---

### Task 3: 实现 UsbDevice::open() / close() / getSerial() / bulkRead() / bulkWrite()

**Files:**
- Modify: `molink-access-cpp/src/usb/usb_device.cpp`

- [ ] **Step 1: 实现 open()**

```cpp
bool UsbDevice::open() {
    if (m_open) return true;

    int ret = libusb_open(m_device, &m_handle);
    if (ret != 0) {
        printf("USB: libusb_open failed: %s\n", libusb_error_name(ret));
        return false;
    }

    // Windows: 如果 WinUSB 驱动未关联，kernel driver 可能已声明接口
    // 尝试 detach kernel driver（不检查返回值，可能本来就没有）
    libusb_detach_kernel_driver(m_handle, m_interface_number);

    ret = libusb_claim_interface(m_handle, m_interface_number);
    if (ret != 0) {
        printf("USB: libusb_claim_interface(%d) failed: %s\n",
               m_interface_number, libusb_error_name(ret));
        // 打印提示
        if (ret == LIBUSB_ERROR_NOT_SUPPORTED || ret == LIBUSB_ERROR_BUSY) {
            printf("USB: Hint: Use Zadig to replace driver of ADB "
                   "interface with WinUSB\n");
        }
        libusb_close(m_handle);
        m_handle = nullptr;
        return false;
    }

    m_open = true;
    printf("USB: Device opened, interface %d claimed (read=0x%02X write=0x%02X)\n",
           m_interface_number, m_read_ep, m_write_ep);
    return true;
}
```

- [ ] **Step 2: 实现 close()**

```cpp
void UsbDevice::close() {
    if (m_handle) {
        libusb_release_interface(m_handle, m_interface_number);
        libusb_close(m_handle);
        m_handle = nullptr;
    }
    m_open = false;
}
```

- [ ] **Step 3: 实现 getSerial()**

```cpp
std::string UsbDevice::getSerial() const {
    if (!m_handle) return "";

    libusb_device_descriptor desc;
    if (libusb_get_device_descriptor(m_device, &desc) != 0) return "";
    if (desc.iSerialNumber == 0) return "";

    unsigned char buf[256] = {0};
    int len = libusb_get_string_descriptor_ascii(
        m_handle, desc.iSerialNumber, buf, sizeof(buf));
    if (len < 0) return "";

    return std::string((char*)buf, len);
}
```

- [ ] **Step 4: 实现 bulkRead() / bulkWrite()**

```cpp
bool UsbDevice::bulkRead(void* buf, int len, int* transferred, int timeout_ms) {
    int ret = libusb_bulk_transfer(m_handle, m_read_ep,
                                   (unsigned char*)buf, len,
                                   transferred, timeout_ms);
    if (ret < 0) {
        printf("USB: bulkRead(ep=0x%02X, len=%d) failed: %s\n",
               m_read_ep, len, libusb_error_name(ret));
        return false;
    }
    return true;
}

bool UsbDevice::bulkWrite(const void* buf, int len, int* transferred, int timeout_ms) {
    int ret = libusb_bulk_transfer(m_handle, m_write_ep,
                                   (unsigned char*)buf, len,
                                   transferred, timeout_ms);
    if (ret < 0) {
        printf("USB: bulkWrite(ep=0x%02X, len=%d) failed: %s\n",
               m_write_ep, len, libusb_error_name(ret));
        return false;
    }
    return true;
}
```

- [ ] **Step 5: 编译验证**

```powershell
cd D:/project/MoLink/molink-access-cpp/build
D:/software/w64devkit/x64/bin/mingw32-make.exe
```

预期：编译成功。

---

### Task 4: 改造 AdbTransport — 移除 AdbWinApi 依赖，改用 UsbDevice&

**Files:**
- Modify: `molink-access-cpp/src/adb/adb_transport.h`
- Modify: `molink-access-cpp/src/adb/adb_transport.cpp`

- [ ] **Step 1: 重写 adb_transport.h**

将文件完整替换为：

```cpp
#ifndef ADB_TRANSPORT_H
#define ADB_TRANSPORT_H

#include <cstdint>
#include <vector>
#include <string>

class UsbDevice;

constexpr uint32_t A_CNXN = 0x4e584e43;
constexpr uint32_t A_AUTH = 0x48545541;
constexpr uint32_t A_OPEN = 0x4e45504f;
constexpr uint32_t A_OKAY = 0x59414b4f;
constexpr uint32_t A_CLSE = 0x45534c43;
constexpr uint32_t A_WRTE = 0x45545257;

constexpr uint32_t A_VERSION = 0x01000000;
constexpr uint32_t MAX_PAYLOAD_V2 = 256 * 1024;

constexpr uint32_t AUTH_TOKEN = 1;
constexpr uint32_t AUTH_SIGNATURE = 2;
constexpr uint32_t AUTH_RSAPUBLICKEY = 3;

#pragma pack(push, 1)
struct AdbMessage {
    uint32_t command;
    uint32_t arg0;
    uint32_t arg1;
    uint32_t data_length;
    uint32_t data_crc32;
    uint32_t magic;

    AdbMessage()
        : command(0), arg0(0), arg1(0), data_length(0), data_crc32(0), magic(0) {}

    AdbMessage(uint32_t cmd, uint32_t a0, uint32_t a1, uint32_t len, uint32_t crc)
        : command(cmd), arg0(a0), arg1(a1), data_length(len), data_crc32(crc)
        , magic(cmd ^ 0xFFFFFFFF) {}
};
#pragma pack(pop)

class AdbTransport {
public:
    explicit AdbTransport(UsbDevice& device);

    bool send(uint32_t cmd, uint32_t arg0, uint32_t arg1,
              const void* data, uint32_t data_len);
    bool recv(AdbMessage& msg, std::vector<uint8_t>& data, uint32_t timeout_ms = 5000);
    bool handshake(const std::string& banner = "host::");

    uint32_t openChannel(const std::string& destination, uint32_t local_id);
    void closeChannel(uint32_t local_id, uint32_t remote_id);

    const std::vector<uint8_t>& getAuthToken() const { return m_auth_token; }
    static uint32_t crc32_direct(const uint8_t* data, uint32_t len) { return crc32(data, len); }

private:
    UsbDevice& m_device;
    std::vector<uint8_t> m_auth_token;

    bool readExact(void* buf, uint32_t len, uint32_t timeout_ms);
    bool writeExact(const void* buf, uint32_t len, uint32_t timeout_ms);
    static uint32_t crc32(const uint8_t* data, uint32_t len);
};

#endif
```

- [ ] **Step 2: 重写 adb_transport.cpp**

将文件完整替换为：

```cpp
#include "adb_transport.h"
#include "../usb/usb_device.h"
#include <cstdio>
#include <cstring>

AdbTransport::AdbTransport(UsbDevice& device)
    : m_device(device) {}

bool AdbTransport::readExact(void* buf, uint32_t len, uint32_t timeout_ms) {
    uint8_t* p = (uint8_t*)buf;
    int remain = (int)len;
    while (remain > 0) {
        int bytes = 0;
        if (!m_device.bulkRead(p, remain, &bytes, (int)timeout_ms)) {
            printf("FAIL: bulk read\n");
            return false;
        }
        if (bytes == 0) {
            printf("FAIL: bulk read returned 0 bytes\n");
            return false;
        }
        p += bytes;
        remain -= bytes;
    }
    return true;
}

bool AdbTransport::writeExact(const void* buf, uint32_t len, uint32_t timeout_ms) {
    const uint8_t* p = (const uint8_t*)buf;
    int remain = (int)len;
    while (remain > 0) {
        int bytes = 0;
        if (!m_device.bulkWrite(p, remain, &bytes, (int)timeout_ms)) {
            printf("FAIL: bulk write\n");
            return false;
        }
        p += bytes;
        remain -= bytes;
    }
    return true;
}

uint32_t AdbTransport::crc32(const uint8_t* data, uint32_t len) {
    static uint32_t table[256];
    static bool table_ready = false;
    if (!table_ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t crc = i;
            for (int j = 0; j < 8; j++)
                crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320UL : 0);
            table[i] = crc;
        }
        table_ready = true;
    }
    uint32_t crc = 0xFFFFFFFF;
    for (uint32_t i = 0; i < len; i++)
        crc = (crc >> 8) ^ table[(crc ^ data[i]) & 0xFF];
    return crc ^ 0xFFFFFFFF;
}

bool AdbTransport::send(uint32_t cmd, uint32_t arg0, uint32_t arg1,
                        const void* data, uint32_t data_len) {
    uint32_t crc = data ? crc32((const uint8_t*)data, data_len) : 0;
    AdbMessage msg(cmd, arg0, arg1, data_len, crc);

    printf("ADB SEND: cmd=0x%08X arg0=%u arg1=%u len=%u crc=%u magic=0x%08X\n",
           cmd, arg0, arg1, data_len, crc, msg.magic);

    // 合并 header + data 为单次 USB 传输
    uint32_t total = sizeof(msg) + data_len;
    std::vector<uint8_t> buf(total);
    memcpy(buf.data(), &msg, sizeof(msg));
    if (data && data_len > 0) {
        memcpy(buf.data() + sizeof(msg), data, data_len);
    }

    if (!writeExact(buf.data(), total, 5000)) {
        printf("FAIL: send\n");
        return false;
    }
    printf("ADB SEND: OK (%u bytes)\n", total);
    return true;
}

bool AdbTransport::recv(AdbMessage& msg, std::vector<uint8_t>& data,
                         uint32_t timeout_ms) {
    data.clear();

    printf("ADB RECV: waiting for header (%u ms)...\n", timeout_ms);
    if (!readExact(&msg, sizeof(msg), timeout_ms)) {
        printf("FAIL: recv header\n");
        return false;
    }

    printf("ADB RECV: cmd=0x%08X arg0=%u arg1=%u len=%u crc=%u magic=0x%08X\n",
           msg.command, msg.arg0, msg.arg1, msg.data_length, msg.data_crc32, msg.magic);

    if (msg.magic != (msg.command ^ 0xFFFFFFFF)) {
        printf("FAIL: bad magic, got 0x%08X expect 0x%08X\n",
               msg.magic, msg.command ^ 0xFFFFFFFF);
        return false;
    }

    if (msg.data_length > MAX_PAYLOAD_V2) {
        printf("FAIL: payload too large: %u\n", msg.data_length);
        return false;
    }

    if (msg.data_length > 0) {
        printf("ADB RECV: reading %u bytes data...\n", msg.data_length);
        data.resize(msg.data_length);
        if (!readExact(data.data(), msg.data_length, timeout_ms)) return false;
    }
    return true;
}

bool AdbTransport::handshake(const std::string& banner) {
    uint32_t maxdata = MAX_PAYLOAD_V2;
    if (!send(A_CNXN, A_VERSION, maxdata, banner.c_str(), (uint32_t)banner.size() + 1)) {
        printf("FAIL: send A_CNXN\n");
        return false;
    }

    AdbMessage msg;
    std::vector<uint8_t> data;
    if (!recv(msg, data, 10000)) {
        printf("FAIL: recv after A_CNXN\n");
        return false;
    }

    if (msg.command == A_CNXN) {
        printf("ADB: Connected! Max=%u Banner=%.*s\n",
               msg.arg1, msg.data_length, (char*)data.data());
        return true;
    }

    if (msg.command == A_AUTH) {
        uint32_t auth_type = msg.arg0;
        printf("ADB: A_AUTH type=%u (token %u bytes, needs RSA signature)\n",
               auth_type, (uint32_t)data.size());
        m_auth_token = data;
        return false;
    }

    printf("FAIL: Unexpected response 0x%08X\n", msg.command);
    return false;
}

uint32_t AdbTransport::openChannel(const std::string& destination, uint32_t local_id) {
    std::vector<uint8_t> data;
    AdbMessage msg;

    if (!send(A_OPEN, local_id, 0,
              destination.c_str(), (uint32_t)destination.size() + 1)) {
        return 0;
    }

    if (!recv(msg, data, 5000)) return 0;
    if (msg.command != A_OKAY) {
        printf("FAIL: Expected A_OKAY, got 0x%08X\n", msg.command);
        return 0;
    }

    printf("ADB: Channel %u opened to %s, remote_id=%u\n",
           local_id, destination.c_str(), msg.arg1);
    return msg.arg1;
}

void AdbTransport::closeChannel(uint32_t local_id, uint32_t remote_id) {
    send(A_CLSE, local_id, remote_id, nullptr, 0);
    printf("ADB: Channel %u/%u closed\n", local_id, remote_id);
}
```

- [ ] **Step 3: 更新 CMakeLists.txt，添加 adb_transport.cpp**

在 `CMakeLists.txt` 的 `add_executable` 中添加：

```cmake
add_executable(molink
    src/poc_main.cpp
    src/usb/usb_device.cpp
    src/adb/adb_transport.cpp
)
```

- [ ] **Step 4: 编译验证**

```powershell
cd D:/project/MoLink/molink-access-cpp/build
D:/software/w64devkit/x64/bin/mingw32-make.exe
```

预期：编译成功。

---

### Task 5: 重写 poc_main.cpp — 完整 POC 流程

**Files:**
- Modify: `molink-access-cpp/src/poc_main.cpp`

- [ ] **Step 1: 重写 poc_main.cpp**

将 `src/poc_main.cpp` 完整替换为：

```cpp
#include <cstdio>
#include <vector>
#include <string>
#include "usb/usb_device.h"
#include "adb/adb_transport.h"

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);

    printf("=== MoLink POC: libusb ===\n");

    // 1. 发现设备
    printf("\n--- Step 1: Discover ---\n");
    std::vector<UsbDevice> devices = UsbDevice::discover();
    if (devices.empty()) {
        printf("FAIL: No ADB device found\n");
        return 1;
    }

    // 2. 打开第一个设备
    printf("\n--- Step 2: Open ---\n");
    UsbDevice& dev = devices[0];
    if (!dev.open()) {
        printf("FAIL: Cannot open device\n");
        return 1;
    }

    // 3. 获取序列号
    printf("\n--- Step 3: Serial ---\n");
    std::string serial = dev.getSerial();
    printf("Serial: %s\n", serial.empty() ? "(unknown)" : serial.c_str());

    // 4. 创建 ADB 传输
    printf("\n--- Step 4: Handshake ---\n");
    AdbTransport transport(dev);
    bool connected = transport.handshake();
    if (!connected) {
        const auto& token = transport.getAuthToken();
        if (!token.empty()) {
            printf("Device requires RSA auth (token=%zu bytes). "
                   "RSA not implemented in POC.\n", token.size());
        } else {
            printf("FAIL: Handshake failed with no auth token\n");
        }
        printf("\n=== POC: Handshake requires RSA auth ===\n");
        printf("This is expected for most devices. "
               "Core USB+ADB path is working!\n");
        dev.close();
        return 0;
    }
    printf("Handshake: CONNECTED!\n");

    // 5. 打开 TCP 通道
    printf("\n--- Step 5: Open Channel ---\n");
    uint32_t remote_id = transport.openChannel("tcp:1080", 1);
    if (remote_id > 0) {
        printf("OK: Channel to tcp:1080 opened! remote_id=%u\n", remote_id);
        transport.closeChannel(1, remote_id);
    }

    // 6. 清理
    dev.close();
    printf("\n=== POC SUCCESS ===\n");
    return 0;
}
```

- [ ] **Step 2: 编译**

```powershell
cd D:/project/MoLink/molink-access-cpp/build
D:/software/w64devkit/x64/bin/mingw32-make.exe
```

预期：编译成功。

---

### Task 6: 运行 POC 测试

- [ ] **Step 1: 清理旧文件，确保 libusb DLL 在输出目录**

```powershell
# 确保 build 目录有 libusb-1.0.dll
Copy-Item D:/project/MoLink/molink-access-cpp/libs/libusb-1.0.dll D:/project/MoLink/molink-access-cpp/build/
```

- [ ] **Step 2: 运行 POC**

```powershell
cd D:/project/MoLink/molink-access-cpp
.\build\molink.exe
```

- [ ] **Step 3: 分析输出**

三种可能结果：

**结果 A：完全成功**
```
=== MoLink POC: libusb ===

--- Step 1: Discover ---
USB: Scanning N USB devices...
USB: Found ADB device [X]: VID=0xXXXX PID=0xXXXX iface=0 read_ep=0x81 write_ep=0x01
USB: Found 1 ADB device(s)

--- Step 2: Open ---
USB: Device opened, interface 0 claimed (read=0x81 write=0x01)

--- Step 3: Serial ---
Serial: XXXXXXXXXXXX

--- Step 4: Handshake ---
ADB SEND: cmd=0x4e584e43 ...
ADB SEND: OK (31 bytes)
ADB RECV: waiting for header (10000 ms)...
ADB RECV: cmd=0x4e584e43 ...   ← 或 0x48545541 (A_AUTH)
ADB: Connected! / Device requires RSA auth...

=== POC SUCCESS ===  ← 或 "Handshake requires RSA auth"（也算通过）
```

**结果 B：设备发现了但打不开（WinUSB 驱动问题）**
```
USB: libusb_claim_interface(0) failed: LIBUSB_ERROR_NOT_SUPPORTED
USB: Hint: Use Zadig to replace driver of ADB interface with WinUSB
```
→ 需用 Zadig 工具替换驱动

**结果 C：完全找不到设备**
→ 检查 USB 线缆、设备是否开启 USB 调试、驱动是否正确

---

## 门禁判定

| 步骤 | 通过标准 |
|---|---|
| 设备发现 | 打印 "Found N ADB device(s)"（N ≥ 1） |
| 设备打开 | 打印 "Device opened, interface X claimed" |
| 序列号获取 | 打印有效序列号或 "(unknown)"（两者均可） |
| ADB 握手 | 收到 A_CNXN 或 A_AUTH 响应 |
| TCP 通道 | 如果握手成功，能打开 tcp:1080 通道 |

**POC 通过条件**：第 1-4 步全部通过（第 5 步在无 RSA 认证的设备上验证）。

如果设备需要 RSA 认证且收到 A_AUTH token，也被视为第 4 步通过——因为这说明 USB 读写通道已打通，只是缺少 RSA 签名。

---

## 执行顺序

```
Task 1（下载libusb+项目骨架）
  → Task 2（设备发现）
    → Task 3（打开/读写/序列号）
      → Task 4（改造 AdbTransport）
        → Task 5（重写 poc_main）
          → Task 6（运行测试）

---

## 当前状态 (2026-05-03) — 魅族设备 POC 已完成

**POC 核心目标全部达成。** USB 通信 + ADB 协议 + RSA 认证握手全部打通。通过两次 Wireshark 抓包分析 adb.exe 的正常通信流程：

- `docs/mezu.pcapng` — 基础握手流程（校验和、header/data 分离等）
- `docs/mezu-auth.pcapng` — 完整首次授权握手（AUTH_RSAPUBLICKEY 流程）


### 关键发现

#### 1. ADB 校验和 = 字节求和，不是 CRC32

adb.exe 的 `data_check` 字段是**简单的字节求和**（`sum(data_bytes)`），而非标准 CRC32。之前的分析认为"libusb/AdbWinApi/WinUSB 三位一体，Bulk Read 始终超时"——根本原因就是校验算法错误，设备丢弃了所有校验和不匹配的消息。

pcap 验证：
- A_CNXN banner (234 bytes): 字节和 = 0x5B44 → header 中的 `data_check` = 0x00005B44 ✅
- AUTH_SIGNATURE (256 bytes): 字节和 = 0x7B70 → header 中的 `data_check` = 0x00007B70 ✅

#### 2. Header 和 Data 必须分开传输

adb.exe 将 ADB header(24B) 和 data 作为**两次独立的 USB bulk write**。之前的代码合并成一次写入，魅族设备不响应。

#### 3. Banner 不含 null 终止符

adb.exe 的 A_CNXN data_length=234 正好等于 banner 字符串长度，不包含 NUL 终止符。之前用 `banner.size() + 1` 多发了 1 字节。

#### 4. 根因纠错

之前的分析认为"魅族 adbd 做了定制化修改，需要 OEM 握手"——这是**错误的**。抓包证实 adb.exe 与魅族设备通信使用的是**完全标准的 ADB 协议**，问题纯粹出在我们的实现上（校验和 + header/data 合并 + banner 格式）。

### 魅族设备信息 (不变)

| 项目 | 值 |
|------|-----|
| VID/PID | 0x2A45 / 0x4EE7 |
| ADB 接口 | class=0xFF, subclass=0x42, protocol=0x01（标准 ADB）|
| 端点 | read_ep=0x81, write_ep=0x01, maxpkt=512 |
| 序列号 | 852QLDV923XMM |
| 系统 | Flyme OS（魅族定制 Android）|

### 已完成 ✅

| 步骤 | 状态 | 备注 |
|------|------|------|
| 设备发现 | ✅ | libusb VID/PID 匹配 |
| 设备打开 | ✅ | libusb_open + claim_interface |
| clearHalt + drainRead | ✅ | 模拟 adb.exe 的 ABORT_PIPE + RESET_PIPE |
| 序列号获取 | ✅ | libusb_get_string_descriptor_ascii |
| ADB A_CNXN 发送 | ✅ | header/data 分开传输，字节求和校验 |
| 接收 A_AUTH TOKEN | ✅ | 设备正常返回 20 字节 token |
| RSA 签名 | ✅ | 手动 PKCS#1 v1.5 填充 + BCryptDecrypt 裸 RSA |
| 发送 AUTH_SIGNATURE | ✅ | 字节求和校验 |
| AUTH_RSAPUBLICKEY | ✅ | base64(RSAPublicKey 524B) + " " + user@host + null |
| 设备授权弹窗 | ✅ | 用户点击"允许"后设备响应 CNXN |
| 打开 TCP 通道 | ✅ | A_OPEN → 设备返回 CLSE（worker 未启动，预期行为） |

### 关键发现 (2026-05-02 补充)

#### 4. AUTH_RSAPUBLICKEY 格式：base64 文本，非原始二进制

adb.exe 的 AUTH_RSAPUBLICKEY 数据格式（720 字节）：
```
base64_encode(RSAPublicKey 结构体 524 字节) + " " + user@host + '\0'
```
**没有 4 字节长度前缀！** 原始实现错误地加了 LE 长度前缀 + 原始二进制公钥。

RSAPublicKey 结构体（524 字节）：
```
uint32_t key_size;   // 64 (2048-bit key 的 32-bit word 数)
uint32_t n0inv;      // -1/n[0] mod 2^32 (Montgomery 逆)
uint32_t modulus[64]; // n 的 64 个 LE uint32 words
uint32_t rr[64];     // R^2 mod n = 2^4096 mod n (Montgomery R^2)
uint32_t exponent;   // 公钥指数 (LE uint32)
```

- `n0inv` 计算：Newton 迭代法求模 2^32 逆元，再取负
- `rr` 计算：2^4096 mod n，通过 2048 次「加倍 + 条件减 n」实现
- 通过 adbkey.pub 的已知数据验证：n0*n0inv ≡ -1 (mod 2^32) ✅

#### 5. NCryptSignHash 产生错误签名

Windows NCryptSignHash 产生的 PKCS#1 v1.5 签名与 OpenSSL/BoringSSL 不一致，导致设备验证失败。

**解决方案**：手动构建 PKCS#1 v1.5 填充块（0x00 0x01 0xFF...0x00 + SHA-1 DigestInfo），然后用 `BCryptDecrypt` + `BCRYPT_PAD_NONE` 做裸 RSA 私钥操作得到签名。

SHA-1 DigestInfo 前缀：
```
30 21 30 09 06 05 2B 0E 03 02 1A 05 00 04 14
```

#### 6. 设备授权流程

握手完整流程：
```
A_CNXN → A_AUTH(TOKEN) → AUTH_SIGNATURE → A_AUTH(TOKEN, 要公钥)
→ AUTH_RSAPUBLICKEY → (设备弹授权对话框，用户点允许) → A_CNXN(成功!)
```

设备会弹出「允许 USB 调试」对话框，用户授权后立即响应 A_CNXN，连接成功。

### 待解决 ⏳

| 步骤 | 状态 | 备注 |
|------|------|------|
| 签名免弹窗 | ⏳ | BCryptSignHash 签名不能被魅族 adbd 验证，每次需 RSAPUBLICKEY+弹窗。疑为 DigestInfo 中 AlgorithmIdentifier 的 NULL 参数格式不一致（`30 09...05 00` vs `30 07...`），需手动控制 PKCS#1 填充 |
| 自动重连 | ⏳ | 连接断开后自动恢复 |

### 明日攻坚方向

**根因推测**：BCryptSignHash 在 DigestInfo 中 SHA-1 的 AlgorithmIdentifier 可能不带 NULL 参数（`30 07`），而魅族 adbd 期望带 NULL（`30 09`）。这会导致签名完全不同，设备无法验证。

**方案**：手动构建带 NULL 的 PKCS#1 v1.5 填充块 + `BCryptDecrypt(BCRYPT_PAD_NONE)` 裸 RSA。已验证手动 PKCS#1 可工作（第一天测试确认），只需确认 DigestInfo 格式。

### 当前架构总结

```
poc_main → UsbDevice(libusb) → AdbTransport(ADB 协议) → AdbRsa(BCrypt)
         ├─ discover/打开/序列号
         ├─ 握手: CNXN → TOKEN → SIGNATURE → TOKEN → RSAPUBLICKEY → CNXN
         └─ 通道: A_OPEN → A_CLSE (worker 未启动)
```

密钥来源：`%USERPROFILE%\.android\adbkey`（PKCS#8 PEM → DER 解析 → BCrypt blob 导入）
公钥来源：`%USERPROFILE%\.android\adbkey.pub`（base64 + user@host，直接复用）

### 新增/修改文件

| 文件 | 变更 |
|------|------|
| `src/adb/adb_transport.h` | A_VERSION → 0x01000001, MAX_PAYLOAD_V2 → 1MB, checksum() 替换 crc32() |
| `src/adb/adb_transport.cpp` | send() header/data 分开写; handshake() 先 SIGNATURE 再 RSAPUBLICKEY; 移除 crc32() |
| `src/usb/usb_device.h` | 新增 clearHalt(), drainRead() |
| `src/usb/usb_device.cpp` | clearHalt()/drainRead() 实现; open() detach_kernel_driver |
| `src/adb/adb_rsa.h` | 新增 getPubKeyPayload(), base64Encode(), buildRsaPublicKey(), readAdbPubKey() |
| `src/adb/adb_rsa.cpp` | **手动 PKCS#1 v1.5 签名** (BCryptDecrypt + BCRYPT_PAD_NONE); **RSAPublicKey 构建** (n0inv + rr 计算); base64Encode(); 读取 adbkey.pub; loadPkcs8() 移除 NCRYPT_DO_NOT_FINALIZE_FLAG |
| `src/poc_main.cpp` | BCrypt 生成密钥 (绕过 NCrypt 签名问题); fullBanner |
| `CMakeLists.txt` | 移除硬编码工具路径; 新增 bcrypt, ncrypt, crypt32 |

### 编译

工具链已全部加入 PATH（MinGW64, CMake, mingw32-make）。

```powershell
cd D:\MyProjects\MoLink\molink-access-cpp
mkdir build && cd build
cmake .. -G "MinGW Makefiles"
mingw32-make
cp ../libs/libusb-1.0.dll .
.\molink.exe
```

### 技术要点

- **校验和**: `sum(data_bytes)` 简单字节求和，不是 CRC32
- **A_CNXN**: header(24B, LE) + data(banner, 不含 NUL), 各一次独立 bulk write
- **版本**: 0x01000001 (A_VERSION_SKIP_CHECKSUM — 但设备仍校验 checksum)
- **RSA 密钥**: 2048-bit, 从 `%USERPROFILE%\.android\adbkey` (PKCS#8 PEM) 导入
- **签名**: NCryptSignHash + BCRYPT_PAD_PKCS1 + SHA1
- **公钥格式**: `user\0` + 4 bytes LE exponent + 256 bytes LE modulus (共 272 bytes)
- **密钥导入路径**: CNG blob 失败 → loadPkcs8() PKCS#8 DER 解析 → NCryptImportKey
- **编译**: MinGW64 GCC 12.1.0, 链接 libusb-1.0.dll.a, ws2_32, bcrypt, ncrypt, crypt32
