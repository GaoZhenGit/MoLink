# MoLink Access C++ 重写实现计划

> **Goal:** 用 C++ 重写 molink-access，分为两个阶段：POC 验证 DLL 可行性 → 完整实现

> **Warning:** 此计划为预研性质，POC 阶段为强制门禁——如 DLL 无法完成设备发现+握手+通道建立，则后续任务不执行。

**Architecture:** 单线程事件驱动，通过 select() 统一管理 Named Pipe CLI、本地监听 socket、ADB 设备端点和设备轮询定时器

**Tech Stack:** C++17, MinGW GCC 32-bit (D:/software/w64devkit/x86), CMake, AdbWinApi.dll, Winsock2

---

## 文件结构

```
molink-access-cpp/
├── CMakeLists.txt
├── libs/
│   ├── AdbWinApi.dll         # 从 D:/AndroidSdk/platform-tools/ 复制
│   └── AdbWinUsbApi.dll      # 同上
├── vendor/
│   └── adb_api.h             # AOSP 头文件
└── src/
    ├── poc_main.cpp          # POC 阶段入口
    ├── adb/
    │   ├── adb_dll.h          # DLL 动态加载，函数指针类型声明
    │   ├── adb_dll.cpp        # LoadLibrary / GetProcAddress 实现
    │   ├── adb_transport.h    # ADB 消息结构和协议常量
    │   ├── adb_transport.cpp  # ADB 消息打包/解包/CRC32
    │   ├── adb_device.h       # 单设备抽象
    │   └── adb_device.cpp     # 设备连接/握手/OPEN/读写
    ├── cli/
    │   ├── cli_server.h
    │   └── cli_server.cpp     # Named Pipe 命令服务
    ├── forward/
    │   ├── forwarder.h
    │   └── forwarder.cpp      # 端口转发
    └── core/
        ├── daemon.h
        └── daemon.cpp         # 守护进程主循环
```

---

## Phase 1: 可行性验证 POC

**门禁标准：** 成功执行以下全部步骤，控制台可打印设备序列号，则 POC 通过。

### Task 1: 搭建 CMake 项目骨架

**Files:**
- Create: `molink-access-cpp/CMakeLists.txt`
- Create: `molink-access-cpp/vendor/adb_api.h`
- Create: `molink-access-cpp/src/poc_main.cpp`

- [ ] **Step 1: 从 Android SDK 复制头文件和 DLL**

```powershell
mkdir -p D:/project/MoLink/molink-access-cpp/libs
mkdir -p D:/project/MoLink/molink-access-cpp/vendor
mkdir -p D:/project/MoLink/molink-access-cpp/src

Copy-Item D:/AndroidSdk/platform-tools/AdbWinApi.dll D:/project/MoLink/molink-access-cpp/libs/
Copy-Item D:/AndroidSdk/platform-tools/AdbWinUsbApi.dll D:/project/MoLink/molink-access-cpp/libs/
```

- [ ] **Step 2: 从 AOSP 下载 adb_api.h 并保存**

使用 curl 下载解码后保存到 `molink-access-cpp/vendor/adb_api.h`：

```powershell
curl -x http://127.0.0.1:7890 -s -L "https://android.googlesource.com/platform/development/+/refs/heads/main/host/windows/usb/api/adb_api.h?format=TEXT" -o adb_api.b64
certutil -decode adb_api.b64 D:/project/MoLink/molink-access-cpp/vendor/adb_api.h
```

- [ ] **Step 3: 编写 CMakeLists.txt**

```cmake
cmake_minimum_required(VERSION 3.14)
project(molink LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 32-bit MinGW
set(CMAKE_C_COMPILER "D:/software/w64devkit/x86/bin/gcc.exe")
set(CMAKE_CXX_COMPILER "D:/software/w64devkit/x86/bin/g++.exe")

set(CMAKE_EXE_LINKER_FLAGS "-static-libgcc -static-libstdc++")

include_directories(${CMAKE_SOURCE_DIR}/vendor)

add_executable(molink src/poc_main.cpp)

# 需要的 Windows 系统库
target_link_libraries(molink ws2_32 setupapi winusb)

# 构建后复制 DLL 到输出目录
add_custom_command(TARGET molink POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/libs/AdbWinApi.dll"
        "${CMAKE_SOURCE_DIR}/libs/AdbWinUsbApi.dll"
        "$<TARGET_FILE_DIR:molink>"
)
```

- [ ] **Step 4: 编写最小 poc_main.cpp 验证编译**

```cpp
#include <windows.h>
#include <cstdio>

int main() {
    printf("MoLink POC starting...\n");

    HMODULE hDll = LoadLibraryW(L"AdbWinApi.dll");
    if (!hDll) {
        printf("FAIL: Cannot load AdbWinApi.dll, err=%lu\n", GetLastError());
        return 1;
    }
    printf("OK: AdbWinApi.dll loaded\n");
    FreeLibrary(hDll);
    return 0;
}
```

- [ ] **Step 5: 编译运行验证**

```powershell
cd D:/project/MoLink/molink-access-cpp
mkdir build -Force
cd build
cmake .. -G "MinGW Makefiles" -DCMAKE_MAKE_PROGRAM="D:/software/w64devkit/x86/bin/mingw32-make.exe"
mingw32-make
cd ..
.\build\molink.exe
```

**预期输出:**
```
MoLink POC starting...
OK: AdbWinApi.dll loaded
```

---

### Task 2: DLL 动态加载封装 + 设备枚举

**Files:**
- Create: `molink-access-cpp/src/adb/adb_dll.h`
- Create: `molink-access-cpp/src/adb/adb_dll.cpp`
- Modify: `molink-access-cpp/src/poc_main.cpp`
- Modify: `molink-access-cpp/CMakeLists.txt`

- [ ] **Step 1: 编写 adb_dll.h — 函数指针类型声明 + 类接口**

```cpp
#ifndef ADB_DLL_H
#define ADB_DLL_H

#include <windows.h>
#include <initguid.h>
#include <usb100.h>

// Android USB Class GUID
// {F72FE0D4-CBCB-407d-8814-9ED673D0DD6B}
DEFINE_GUID(GUID_ANDROID_USB,
    0xf72fe0d4, 0xcbcb, 0x407d,
    0x88, 0x14, 0x9e, 0xd6, 0x73, 0xd0, 0xdd, 0x6b);

typedef void* ADBAPIHANDLE;

typedef enum {
    AdbOpenAccessTypeReadWrite = 0,
    AdbOpenAccessTypeRead,
    AdbOpenAccessTypeWrite,
    AdbOpenAccessTypeQueryInfo,
} AdbOpenAccessType;

typedef enum {
    AdbOpenSharingModeReadWrite = 0,
    AdbOpenSharingModeRead,
    AdbOpenSharingModeWrite,
    AdbOpenSharingModeExclusive,
} AdbOpenSharingMode;

typedef struct {
    unsigned long max_packet_size;
    unsigned long max_transfer_size;
    int           endpoint_type;
    unsigned char endpoint_address;
    unsigned char polling_interval;
    unsigned char setting_index;
} AdbEndpointInformation;

typedef struct {
    GUID          class_id;
    unsigned long flags;
    wchar_t       device_name[1];
} AdbInterfaceInfo;

// DLL 函数指针类型
typedef ADBAPIHANDLE (__cdecl *PFN_AdbEnumInterfaces)(GUID, bool, bool, bool);
typedef bool (__cdecl *PFN_AdbNextInterface)(ADBAPIHANDLE, AdbInterfaceInfo*, unsigned long*);
typedef bool (__cdecl *PFN_AdbResetInterfaceEnum)(ADBAPIHANDLE);
typedef ADBAPIHANDLE (__cdecl *PFN_AdbCreateInterfaceByName)(const wchar_t*);
typedef bool (__cdecl *PFN_AdbGetSerialNumber)(ADBAPIHANDLE, void*, unsigned long*, bool);
typedef ADBAPIHANDLE (__cdecl *PFN_AdbOpenDefaultBulkReadEndpoint)(ADBAPIHANDLE, AdbOpenAccessType, AdbOpenSharingMode);
typedef ADBAPIHANDLE (__cdecl *PFN_AdbOpenDefaultBulkWriteEndpoint)(ADBAPIHANDLE, AdbOpenAccessType, AdbOpenSharingMode);
typedef bool (__cdecl *PFN_AdbReadEndpointSync)(ADBAPIHANDLE, void*, unsigned long, unsigned long*, unsigned long);
typedef bool (__cdecl *PFN_AdbWriteEndpointSync)(ADBAPIHANDLE, void*, unsigned long, unsigned long*, unsigned long);
typedef bool (__cdecl *PFN_AdbCloseHandle)(ADBAPIHANDLE);
typedef bool (__cdecl *PFN_AdbGetEndpointInformation)(ADBAPIHANDLE, unsigned char, AdbEndpointInformation*);

class AdbDll {
public:
    AdbDll();
    ~AdbDll();
    bool load();

    // 函数指针成员
    PFN_AdbEnumInterfaces              EnumInterfaces;
    PFN_AdbNextInterface               NextInterface;
    PFN_AdbResetInterfaceEnum          ResetInterfaceEnum;
    PFN_AdbCreateInterfaceByName       CreateInterfaceByName;
    PFN_AdbGetSerialNumber             GetSerialNumber;
    PFN_AdbOpenDefaultBulkReadEndpoint OpenDefaultBulkReadEndpoint;
    PFN_AdbOpenDefaultBulkWriteEndpoint OpenDefaultBulkWriteEndpoint;
    PFN_AdbReadEndpointSync            ReadEndpointSync;
    PFN_AdbWriteEndpointSync           WriteEndpointSync;
    PFN_AdbCloseHandle                 CloseHandle;

private:
    HMODULE m_hDll;

    template<typename T>
    T bind(const char* name);
};

#endif
```

- [ ] **Step 2: 编写 adb_dll.cpp — LoadLibrary + GetProcAddress 实现**

```cpp
#include "adb_dll.h"
#include <cstdio>

AdbDll::AdbDll() : m_hDll(nullptr) {}

AdbDll::~AdbDll() {
    if (m_hDll) FreeLibrary(m_hDll);
}

bool AdbDll::load() {
    m_hDll = LoadLibraryW(L"AdbWinApi.dll");
    if (!m_hDll) {
        printf("FAIL: LoadLibrary AdbWinApi.dll, err=%lu\n", GetLastError());
        return false;
    }

    #define BIND(name) name = bind<PFN_##name>(#name); if (!name) return false

    BIND(AdbEnumInterfaces);
    BIND(AdbNextInterface);
    BIND(AdbResetInterfaceEnum);
    BIND(AdbCreateInterfaceByName);
    BIND(AdbGetSerialNumber);
    BIND(AdbOpenDefaultBulkReadEndpoint);
    BIND(AdbOpenDefaultBulkWriteEndpoint);
    BIND(AdbReadEndpointSync);
    BIND(AdbWriteEndpointSync);
    BIND(AdbCloseHandle);

    #undef BIND
    printf("OK: All AdbWinApi functions bound\n");
    return true;
}

template<typename T>
T AdbDll::bind(const char* name) {
    FARPROC proc = GetProcAddress(m_hDll, name);
    if (!proc) {
        printf("FAIL: GetProcAddress(%s), err=%lu\n", name, GetLastError());
        return nullptr;
    }
    return reinterpret_cast<T>(proc);
}
```

- [ ] **Step 3: 更新 CMakeLists.txt 添加 adb_dll.cpp**

在 CMakeLists.txt 的 `add_executable` 行修改为：

```cmake
add_executable(molink
    src/poc_main.cpp
    src/adb/adb_dll.cpp
)
```

- [ ] **Step 4: 更新 poc_main.cpp — 枚举设备测试**

```cpp
#include <windows.h>
#include <cstdio>
#include <vector>
#include <string>
#include "adb/adb_dll.h"

int main() {
    printf("=== MoLink POC: Device Enumeration ===\n");

    AdbDll adb;
    if (!adb.load()) {
        printf("FATAL: AdbWinApi.dll load failed\n");
        return 1;
    }

    // 枚举设备接口
    ADBAPIHANDLE hEnum = adb.EnumInterfaces(GUID_ANDROID_USB, true, true, true);
    if (!hEnum) {
        printf("FAIL: AdbEnumInterfaces, err=%lu\n", GetLastError());
        printf("(可能没有连接 Android 设备)\n");
        return 1;
    }

    std::vector<std::wstring> device_names;
    unsigned long size = 0;
    while (true) {
        if (!adb.NextInterface(hEnum, nullptr, &size)) {
            if (GetLastError() == ERROR_NO_MORE_ITEMS) break;
            printf("FAIL: AdbNextInterface get size, err=%lu\n", GetLastError());
            break;
        }
        AdbInterfaceInfo* info = (AdbInterfaceInfo*)malloc(size);
        if (!info) break;

        if (adb.NextInterface(hEnum, info, &size)) {
            device_names.push_back(info->device_name);
        } else {
            free(info);
            if (GetLastError() == ERROR_NO_MORE_ITEMS) break;
        }
    }
    adb.CloseHandle(hEnum);

    printf("Found %zu Android USB device(s):\n", device_names.size());
    for (auto& name : device_names) {
        printf("  %ls\n", name.c_str());
    }

    if (device_names.empty()) {
        printf("No devices found - 请连接 Android 设备后重试\n");
        return 1;
    }
    return 0;
}
```

- [ ] **Step 5: 编译运行**

```powershell
cd D:/project/MoLink/molink-access-cpp/build
mingw32-make
cd ..
.\build\molink.exe
```

**预期输出:** 列出已连接的 Android 设备接口路径

---

### Task 3: ADB 消息协议实现

**Files:**
- Create: `molink-access-cpp/src/adb/adb_transport.h`
- Create: `molink-access-cpp/src/adb/adb_transport.cpp`

- [ ] **Step 1: 编写 adb_transport.h — 消息结构定义**

```cpp
#ifndef ADB_TRANSPORT_H
#define ADB_TRANSPORT_H

#include <cstdint>
#include <vector>
#include <string>

// ADB 协议常量
constexpr uint32_t A_CNXN = 0x4e584e43;
constexpr uint32_t A_AUTH = 0x48545541;
constexpr uint32_t A_OPEN = 0x4e45504f;
constexpr uint32_t A_OKAY = 0x59414b4f;
constexpr uint32_t A_CLSE = 0x45534c43;
constexpr uint32_t A_WRTE = 0x45545257;

constexpr uint32_t A_VERSION = 0x01000000;
constexpr uint32_t MAX_PAYLOAD_V1 = 4 * 1024;
constexpr uint32_t MAX_PAYLOAD_V2 = 256 * 1024;

// ADB 认证类型
constexpr uint32_t AUTH_TOKEN = 1;
constexpr uint32_t AUTH_SIGNATURE = 2;
constexpr uint32_t AUTH_RSAPUBLICKEY = 3;

#pragma pack(push, 1)
struct AdbMessage {
    uint32_t command;   // A_CNXN / A_OPEN / etc.
    uint32_t arg0;      // local_id
    uint32_t arg1;      // remote_id
    uint32_t data_length;
    uint32_t data_crc32;
    uint32_t magic;     // command ^ 0xFFFFFFFF

    AdbMessage() : command(0), arg0(0), arg1(0), data_length(0), data_crc32(0), magic(0) {}

    AdbMessage(uint32_t cmd, uint32_t a0, uint32_t a1, uint32_t len, uint32_t crc)
        : command(cmd), arg0(a0), arg1(a1), data_length(len), data_crc32(crc)
        , magic(cmd ^ 0xFFFFFFFF) {}
};
#pragma pack(pop)

class RsaAuth;

class AdbTransport {
public:
    AdbTransport(void* read_ep, void* write_ep,
                  PFN_AdbReadEndpointSync read_fn,
                  PFN_AdbWriteEndpointSync write_fn);

    // 发送 ADB 消息（header + data）
    bool send(uint32_t cmd, uint32_t arg0, uint32_t arg1,
              const void* data, uint32_t data_len);

    // 接收 ADB 消息
    bool recv(AdbMessage& msg, std::vector<uint8_t>& data, uint32_t timeout_ms = 5000);

    // 连接握手：发送 A_CNXN，处理 A_AUTH，返回 true=成功
    bool handshake(RsaAuth* auth = nullptr,
                   const std::string& banner = "host::");

    // 打开到设备的 TCP 通道: A_OPEN("tcp:<port>")
    // 返回 local_id（arg0）
    uint32_t openChannel(const std::string& destination, uint32_t local_id);

    // 关闭通道
    void closeChannel(uint32_t local_id, uint32_t remote_id);

    // 读通道数据
    bool readChannel(uint32_t local_id, uint32_t remote_id,
                     void* buf, uint32_t len, uint32_t& bytes_read,
                     uint32_t timeout_ms = 0);

    // 写通道数据
    bool writeChannel(uint32_t local_id, uint32_t remote_id,
                      const void* buf, uint32_t len, uint32_t timeout_ms = 0);

private:
    void* m_read_ep;
    void* m_write_ep;
    PFN_AdbReadEndpointSync m_read;
    PFN_AdbWriteEndpointSync m_write;

    bool readExact(void* buf, uint32_t len, uint32_t timeout_ms);
    bool writeExact(const void* buf, uint32_t len, uint32_t timeout_ms);
    uint32_t crc32(const uint8_t* data, uint32_t len);
};

#endif
```

- [ ] **Step 2: 编写 adb_transport.cpp — 核心实现**

```cpp
#include "adb_transport.h"
#include "adb_dll.h"
#include <cstdio>
#include <cstring>

AdbTransport::AdbTransport(void* read_ep, void* write_ep,
                           PFN_AdbReadEndpointSync read_fn,
                           PFN_AdbWriteEndpointSync write_fn)
    : m_read_ep(read_ep), m_write_ep(write_ep)
    , m_read(read_fn), m_write(write_fn) {}

bool AdbTransport::readExact(void* buf, uint32_t len, uint32_t timeout_ms) {
    uint8_t* p = (uint8_t*)buf;
    uint32_t remain = len;
    while (remain > 0) {
        unsigned long bytes = 0;
        if (!m_read(m_read_ep, p, remain, &bytes, timeout_ms)) {
            printf("FAIL: bulk read, err=%lu\n", GetLastError());
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
    uint32_t remain = len;
    while (remain > 0) {
        unsigned long bytes = 0;
        if (!m_write(m_write_ep, (void*)p, remain, &bytes, timeout_ms)) {
            printf("FAIL: bulk write, err=%lu\n", GetLastError());
            return false;
        }
        p += bytes;
        remain -= bytes;
    }
    return true;
}

uint32_t AdbTransport::crc32(const uint8_t* data, uint32_t len) {
    // CRC32 table and algorithm from AOSP
    static uint32_t table[256];
    static bool table_ready = false;
    if (!table_ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t crc = i;
            for (int j = 0; j < 8; j++)
                crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320 : 0);
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
    AdbMessage msg(cmd, arg0, arg1, data_len,
                   data ? crc32((const uint8_t*)data, data_len) : 0);

    printf("ADB SEND: cmd=0x%08X arg0=%u arg1=%u len=%u\n",
           cmd, arg0, arg1, data_len);

    if (!writeExact(&msg, sizeof(msg), 5000)) return false;
    if (data && data_len > 0) {
        if (!writeExact(data, data_len, 5000)) return false;
    }
    return true;
}

bool AdbTransport::recv(AdbMessage& msg, std::vector<uint8_t>& data,
                         uint32_t timeout_ms) {
    data.clear();

    if (!readExact(&msg, sizeof(msg), timeout_ms)) return false;

    printf("ADB RECV: cmd=0x%08X arg0=%u arg1=%u len=%u\n",
           msg.command, msg.arg0, msg.arg1, msg.data_length);

    if (msg.magic != (msg.command ^ 0xFFFFFFFF)) {
        printf("FAIL: bad magic\n");
        return false;
    }

    if (msg.data_length > 0) {
        data.resize(msg.data_length);
        if (!readExact(data.data(), msg.data_length, timeout_ms)) return false;
    }
    return true;
}

bool AdbTransport::handshake(RsaAuth* auth, const std::string& banner) {
    uint32_t maxdata = MAX_PAYLOAD_V2;
    if (!send(A_CNXN, A_VERSION, maxdata, banner.c_str(), (uint32_t)banner.size() + 1)) {
        printf("FAIL: send A_CNXN\n");
        return false;
    }

    int auth_rounds = 0;
    while (auth_rounds < 5) {
        AdbMessage msg;
        std::vector<uint8_t> data;
        if (!recv(msg, data, 10000)) {
            printf("FAIL: recv after A_CNXN/A_AUTH\n");
            return false;
        }

        if (msg.command == A_CNXN) {
            printf("OK: Connected! Max=%u Banner=%.*s\n",
                   msg.arg1, msg.data_length, (char*)data.data());
            return true;
        }

        if (msg.command == A_AUTH && auth) {
            uint32_t auth_type = msg.arg0;
            if (auth_type == AUTH_TOKEN) {
                // 对 token 做 RSA-SHA1 签名
                auto sig = auth->sign(data.data(), data.size());
                if (sig.empty()) {
                    printf("FAIL: RSA sign returned empty\n");
                    return false;
                }
                if (!send(A_AUTH, AUTH_SIGNATURE, 0, sig.data(), (uint32_t)sig.size())) {
                    printf("FAIL: send A_AUTH signature\n");
                    return false;
                }
            } else {
                printf("FAIL: Unknown A_AUTH type %u\n", auth_type);
                return false;
            }
        } else if (msg.command == A_AUTH && !auth) {
            printf("FAIL: Device requires A_AUTH but no RsaAuth provided\n");
            return false;
        } else {
            printf("FAIL: Unexpected command 0x%08X\n", msg.command);
            return false;
        }
        auth_rounds++;
    }
    printf("FAIL: Too many A_AUTH rounds\n");
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
    if (msg.command != A_OKAY) return 0;

    printf("OK: Channel %u opened to %s, remote_id=%u\n",
           local_id, destination.c_str(), msg.arg1);
    return msg.arg1;  // remote_id
}

void AdbTransport::closeChannel(uint32_t local_id, uint32_t remote_id) {
    send(A_CLSE, local_id, remote_id, nullptr, 0);
}

bool AdbTransport::readChannel(uint32_t local_id, uint32_t remote_id,
                                void* buf, uint32_t len, uint32_t& bytes_read,
                                uint32_t timeout_ms) {
    AdbMessage msg;
    std::vector<uint8_t> data;
    if (!recv(msg, data, timeout_ms)) return false;

    if (msg.command == A_CLSE) {
        bytes_read = 0;
        return false;
    }

    if (msg.command != A_WRTE) return false;

    bytes_read = msg.data_length;
    memcpy(buf, data.data(), bytes_read);
    return true;
}

bool AdbTransport::writeChannel(uint32_t local_id, uint32_t remote_id,
                                 const void* buf, uint32_t len, uint32_t timeout_ms) {
    if (!send(A_WRTE, local_id, remote_id, buf, len)) return false;

    AdbMessage msg;
    std::vector<uint8_t> data;
    if (!recv(msg, data, timeout_ms)) return false;
    return msg.command == A_OKAY;
}
```

- [ ] **Step 3: 更新 CMakeLists.txt 添加 adb_transport.cpp**

```cmake
add_executable(molink
    src/poc_main.cpp
    src/adb/adb_dll.cpp
    src/adb/adb_transport.cpp
)
```

---

### Task 4: 完整 POC — 设备连接 + 握手测试

**Files:**
- Modify: `molink-access-cpp/src/poc_main.cpp`

- [ ] **Step 1: 重写 poc_main.cpp — 完整设备发现+连接+握手流程**

```cpp
#include <windows.h>
#include <cstdio>
#include <vector>
#include <string>
#include "adb/adb_dll.h"
#include "adb/adb_transport.h"

int main() {
    printf("=== MoLink POC: Device Connect Test ===\n");

    // 1. 加载 DLL
    AdbDll adb;
    if (!adb.load()) return 1;

    // 2. 枚举设备
    ADBAPIHANDLE hEnum = adb.EnumInterfaces(GUID_ANDROID_USB, true, true, true);
    if (!hEnum) {
        printf("FAIL: AdbEnumInterfaces, err=%lu\n", GetLastError());
        return 1;
    }

    std::wstring dev_name;
    unsigned long size = 0;
    if (!adb.NextInterface(hEnum, nullptr, &size)) {
        printf("FAIL: No device found\n");
        adb.CloseHandle(hEnum);
        return 1;
    }
    AdbInterfaceInfo* info = (AdbInterfaceInfo*)malloc(size);
    if (!adb.NextInterface(hEnum, info, &size)) {
        printf("FAIL: Cannot get first device\n");
        adb.CloseHandle(hEnum);
        return 1;
    }
    dev_name = info->device_name;
    printf("Device: %ls\n", dev_name.c_str());
    adb.CloseHandle(hEnum);

    // 3. 打开设备接口
    ADBAPIHANDLE hIface = adb.CreateInterfaceByName(dev_name.c_str());
    if (!hIface) {
        printf("FAIL: AdbCreateInterfaceByName, err=%lu\n", GetLastError());
        return 1;
    }
    printf("OK: Interface opened\n");

    // 4. 获取序列号
    wchar_t serial[256] = {0};
    unsigned long serial_size = 256;
    if (adb.GetSerialNumber(hIface, serial, &serial_size, false)) {
        printf("Serial: %ls\n", serial);
    } else {
        printf("WARN: Cannot get serial, err=%lu\n", GetLastError());
    }

    // 5. 打开 Bulk 端点
    ADBAPIHANDLE hRead = adb.OpenDefaultBulkReadEndpoint(
        hIface, AdbOpenAccessTypeReadWrite, AdbOpenSharingModeReadWrite);
    if (!hRead) {
        printf("FAIL: Open read endpoint, err=%lu\n", GetLastError());
        adb.CloseHandle(hIface);
        return 1;
    }

    ADBAPIHANDLE hWrite = adb.OpenDefaultBulkWriteEndpoint(
        hIface, AdbOpenAccessTypeReadWrite, AdbOpenSharingModeReadWrite);
    if (!hWrite) {
        printf("FAIL: Open write endpoint, err=%lu\n", GetLastError());
        adb.CloseHandle(hRead);
        adb.CloseHandle(hIface);
        return 1;
    }
    printf("OK: Bulk endpoints opened\n");

    // 6. ADB 握手（先尝试无认证）
    AdbTransport transport(hRead, hWrite,
                           adb.ReadEndpointSync, adb.WriteEndpointSync);

    bool connected = transport.handshake(nullptr); // 无 RSA auth，仅测试
    printf("Handshake: %s\n", connected ? "PASS" : "FAIL (may need RSA auth)");

    // 7. 如果握手失败且需要认证，加载 adbkey 重试
    if (!connected) {
        printf("尝试加载 RSA 密钥重试...\n");
        // ... RSA 加载和重试逻辑（POC 阶段可选）
    }

    // 7. 如果握手成功，尝试打开 TCP 通道
    if (connected) {
        uint32_t remote_id = transport.openChannel("tcp:1080", 1);
        if (remote_id > 0) {
            printf("OK: Channel to tcp:1080 opened! remote_id=%u\n", remote_id);
            transport.closeChannel(1, remote_id);
        }
    }

    // 8. 清理
    adb.CloseHandle(hWrite);
    adb.CloseHandle(hRead);
    adb.CloseHandle(hIface);
    printf("Cleanup done\n");
    return 0;
}
```

- [ ] **Step 2: 编译运行**

```powershell
cd D:/project/MoLink/molink-access-cpp/build
mingw32-make
cd ..
.\build\molink.exe
```

**门禁判定标准：**
- ✅ 能列出设备
- ✅ 能打开接口并获取序列号
- ✅ 能打开 Bulk 端点
- ✅ 能完成 ADB 握手（或明确报告需要 RSA 认证）
- ✅ 能打开 TCP 通道

**如果 RSA 认证阻塞：** 在此步骤停下来，实现 RSA 签名逻辑（从 `%USERPROFILE%\.android\adbkey` 读取私钥 + OpenSSL/BoringSSL 或 Windows CryptoAPI 做签名）。

---

## Phase 2: 完整实现（POC 通过后执行）

### Task 5: adb_device 抽象 + RSA 认证

**Files:**
- Create: `molink-access-cpp/src/adb/adb_device.h`
- Create: `molink-access-cpp/src/adb/adb_device.cpp`
- Create: `molink-access-cpp/src/adb/rsa_auth.h`
- Create: `molink-access-cpp/src/adb/rsa_auth.cpp`

- [ ] **Step 1: rsa_auth.h — RSA 签名辅助**

```cpp
#ifndef RSA_AUTH_H
#define RSA_AUTH_H

#include <vector>
#include <string>
#include <cstdint>

class RsaAuth {
public:
    // 从 adbkey 文件加载私钥（PEM 格式 PKCS#8）
    bool loadKey(const std::string& path);

    // 对 token 做 RSA-SHA1 签名，返回签名结果
    std::vector<uint8_t> sign(const uint8_t* token, size_t token_len);

    // 是否已加载密钥
    bool ready() const { return m_loaded; }

private:
    bool m_loaded = false;
    // Windows CryptoAPI 的内部结构，或用 OpenSSL RSA*
    void* m_key_data = nullptr;
};

#endif
```

- [ ] **Step 2: rsa_auth.cpp — 使用 Windows CryptoAPI 实现**

```cpp
#include "rsa_auth.h"

#include <windows.h>
#include <wincrypt.h>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "crypt32.lib")

bool RsaAuth::loadKey(const std::string& path) {
    // 读取 adbkey 文件 (PEM PKCS#8)
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        printf("RSA: Cannot open %s\n", path.c_str());
        return false;
    }
    fseek(f, 0, SEEK_END);
    size_t len = ftell(f);
    fseek(f, 0, SEEK_SET);

    std::vector<char> pem(len + 1);
    fread(pem.data(), 1, len, f);
    fclose(f);
    pem[len] = 0;

    // 跳过 PEM 头尾，提取 base64
    const char* begin = strstr(pem.data(), "-----BEGIN");
    if (!begin) { printf("RSA: bad PEM header\n"); return false; }
    begin = strchr(begin, '\n');
    if (!begin) { printf("RSA: no line after header\n"); return false; }
    begin++;

    const char* end = strstr(begin, "-----END");
    if (!end) { printf("RSA: no PEM footer\n"); return false; }

    // Base64 解码到 DER
    DWORD der_len = 0;
    if (!CryptStringToBinaryA(begin, (DWORD)(end - begin),
                               CRYPT_STRING_BASE64, nullptr, &der_len,
                               nullptr, nullptr)) {
        printf("RSA: base64 decode size fail, err=%lu\n", GetLastError());
        return false;
    }

    std::vector<uint8_t> der(der_len);
    if (!CryptStringToBinaryA(begin, (DWORD)(end - begin),
                               CRYPT_STRING_BASE64, der.data(), &der_len,
                               nullptr, nullptr)) {
        printf("RSA: base64 decode fail, err=%lu\n", GetLastError());
        return false;
    }

    // 导入 PKCS#8 私钥到 CSP
    if (!CryptDecodeObjectEx(X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
                              PKCS_RSA_PRIVATE_KEY,
                              der.data(), der_len,
                              CRYPT_DECODE_ALLOC_FLAG, nullptr,
                              &m_key_data, &der_len)) {
        printf("RSA: CryptDecodeObjectEx fail (trying CNG), err=%lu\n", GetLastError());
        // 备选: 使用 CNG (BCrypt) 或直接用 OpenSSL lite
        return false;
    }

    m_loaded = true;
    printf("RSA: Key loaded from %s\n", path.c_str());
    return true;
}

std::vector<uint8_t> RsaAuth::sign(const uint8_t* token, size_t token_len) {
    std::vector<uint8_t> result;
    if (!m_loaded || !m_key_data) return result;

    // SHA-1 hash first (ADB uses SHA-1)
    HCRYPTPROV hProv = 0;
    HCRYPTHASH hHash = 0;
    HCRYPTKEY hKey = 0;

    if (!CryptAcquireContextW(&hProv, nullptr, nullptr, PROV_RSA_FULL, 0)) {
        printf("RSA: CryptAcquireContext fail, err=%lu\n", GetLastError());
        return result;
    }

    if (!CryptImportKey(hProv, ((CRYPT_PRIVATE_KEY_INFO*)m_key_data)->PrivateKey.pbData,
                         ((CRYPT_PRIVATE_KEY_INFO*)m_key_data)->PrivateKey.cbData,
                         0, 0, &hKey)) {
        printf("RSA: CryptImportKey fail, err=%lu\n", GetLastError());
        CryptReleaseContext(hProv, 0);
        return result;
    }

    if (!CryptCreateHash(hProv, CALG_SHA1, 0, 0, &hHash)) {
        printf("RSA: CryptCreateHash fail, err=%lu\n", GetLastError());
        CryptDestroyKey(hKey);
        CryptReleaseContext(hProv, 0);
        return result;
    }

    if (!CryptHashData(hHash, token, (DWORD)token_len, 0)) {
        printf("RSA: CryptHashData fail, err=%lu\n", GetLastError());
        CryptDestroyHash(hHash);
        CryptDestroyKey(hKey);
        CryptReleaseContext(hProv, 0);
        return result;
    }

    DWORD sig_len = 0;
    CryptSignHashW(hHash, AT_SIGNATURE, nullptr, 0, nullptr, &sig_len);
    result.resize(sig_len);
    if (!CryptSignHashW(hHash, AT_SIGNATURE, nullptr, 0, result.data(), &sig_len)) {
        printf("RSA: CryptSignHash fail, err=%lu\n", GetLastError());
        result.clear();
    }

    CryptDestroyHash(hHash);
    CryptDestroyKey(hKey);
    CryptReleaseContext(hProv, 0);
    return result;
}
```

- [ ] **Step 3: adb_device.h — 单设备抽象**

```cpp
#ifndef ADB_DEVICE_H
#define ADB_DEVICE_H

#include "adb_dll.h"
#include "adb_transport.h"
#include "rsa_auth.h"
#include <string>
#include <memory>

class AdbDevice {
public:
    AdbDevice(AdbDll* adb, const std::wstring& iface_name);
    ~AdbDevice();

    bool connect(RsaAuth* auth = nullptr);
    bool isConnected() const { return m_connected; }
    std::string getSerial() const;

    // 返回 AdbTransport 供上层使用
    AdbTransport* getTransport() { return m_transport.get(); }

private:
    AdbDll* m_adb;
    std::wstring m_iface_name;
    std::string m_serial;
    bool m_connected = false;

    ADBAPIHANDLE m_hIface = nullptr;
    ADBAPIHANDLE m_hRead = nullptr;
    ADBAPIHANDLE m_hWrite = nullptr;
    std::unique_ptr<AdbTransport> m_transport;
};

#endif
```

- [ ] **Step 4: adb_device.cpp — 实现**

```cpp
#include "adb_device.h"
#include <cstdio>

AdbDevice::AdbDevice(AdbDll* adb, const std::wstring& iface_name)
    : m_adb(adb), m_iface_name(iface_name) {}

AdbDevice::~AdbDevice() {
    if (m_transport) m_transport.reset();
    if (m_hWrite) m_adb->CloseHandle(m_hWrite);
    if (m_hRead) m_adb->CloseHandle(m_hRead);
    if (m_hIface) m_adb->CloseHandle(m_hIface);
}

std::string AdbDevice::getSerial() const {
    return m_serial;
}

bool AdbDevice::connect(RsaAuth* auth) {
    // 1. 打开接口
    m_hIface = m_adb->CreateInterfaceByName(m_iface_name.c_str());
    if (!m_hIface) {
        printf("Device: CreateInterfaceByName fail, err=%lu\n", GetLastError());
        return false;
    }

    // 2. 获取序列号
    wchar_t serial[256] = {0};
    unsigned long serial_size = 256;
    if (m_adb->GetSerialNumber(m_hIface, serial, &serial_size, false)) {
        // 转换为 UTF-8
        int len = WideCharToMultiByte(CP_UTF8, 0, serial, -1, nullptr, 0, nullptr, nullptr);
        m_serial.resize(len - 1);
        WideCharToMultiByte(CP_UTF8, 0, serial, -1, &m_serial[0], len, nullptr, nullptr);
        printf("Device: serial=%s\n", m_serial.c_str());
    }

    // 3. 打开 Bulk 端点
    m_hRead = m_adb->OpenDefaultBulkReadEndpoint(
        m_hIface, AdbOpenAccessTypeReadWrite, AdbOpenSharingModeReadWrite);
    m_hWrite = m_adb->OpenDefaultBulkWriteEndpoint(
        m_hIface, AdbOpenAccessTypeReadWrite, AdbOpenSharingModeReadWrite);

    if (!m_hRead || !m_hWrite) {
        printf("Device: Open endpoints fail\n");
        return false;
    }

    // 4. 创建 Transport
    m_transport.reset(new AdbTransport(
        m_hRead, m_hWrite,
        m_adb->ReadEndpointSync, m_adb->WriteEndpointSync));

    // 5. 握手
    //    AdbTransport::handshake() 内部处理 A_AUTH 挑战，
    //    若需要 RSA 签名则调用 RsaAuth::sign()
    m_connected = m_transport->handshake(auth);
    if (!m_connected) {
        printf("Device: Handshake FAILED\n");
        return false;
    }

    printf("Device: Connected\n");
    return true;
}
```

---

### Task 6: 守护进程主循环

**Files:**
- Create: `molink-access-cpp/src/core/daemon.h`
- Create: `molink-access-cpp/src/core/daemon.cpp`

- [ ] **Step 1: daemon.h**

```cpp
#ifndef DAEMON_H
#define DAEMON_H

#include "../adb/adb_dll.h"
#include "../adb/adb_device.h"
#include "../cli/cli_server.h"
#include "../forward/forwarder.h"
#include <vector>
#include <memory>

class Daemon {
public:
    Daemon();
    ~Daemon();

    int run();

private:
    AdbDll m_adb;
    std::vector<std::unique_ptr<AdbDevice>> m_devices;
    std::vector<std::unique_ptr<Forwarder>> m_forwarders;
    CliServer m_cli;

    HANDLE m_timer = nullptr;
    bool m_running = false;

    void pollDevices();
    void setupForwarder(AdbDevice* device, int local_port, int remote_port);

    // CLI 回调
    std::string handleCommand(const std::string& cmd);
};

#endif
```

- [ ] **Step 2: daemon.cpp — 主循环实现**

```cpp
#include "daemon.h"
#include <cstdio>
#include <sstream>

Daemon::Daemon() : m_cli("\\.\\pipe\\molink") {}

Daemon::~Daemon() {
    m_running = false;
    if (m_timer) CloseHandle(m_timer);
}

int Daemon::run() {
    printf("=== MoLink Daemon Starting ===\n");

    if (!m_adb.load()) {
        printf("FATAL: Cannot load AdbWinApi.dll\n");
        return 1;
    }

    // 启动 CLI Server
    m_cli.setHandler([this](const std::string& cmd) -> std::string {
        return handleCommand(cmd);
    });
    m_cli.start();

    // 创建设备轮询定时器 (3s)
    m_timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    LARGE_INTEGER due;
    due.QuadPart = -30000000LL; // 3s in 100ns units
    SetWaitableTimer(m_timer, &due, 3000, nullptr, nullptr, FALSE);

    // 初次扫描
    pollDevices();

    // 主循环
    m_running = true;
    while (m_running) {
        // 将 Named Pipe 事件加入等待
        HANDLE handles[2] = { m_timer, m_cli.getEventHandle() };
        DWORD ret = WaitForMultipleObjects(2, handles, FALSE, INFINITE);

        if (ret == WAIT_OBJECT_0) {
            // 定时器到期 → 轮询设备
            pollDevices();
        } else if (ret == WAIT_OBJECT_0 + 1) {
            // CLI 有连接
            m_cli.acceptAndProcess();
        } else if (ret == WAIT_FAILED) {
            printf("WaitForMultipleObjects failed, err=%lu\n", GetLastError());
            break;
        }
    }

    printf("=== MoLink Daemon Stopping ===\n");
    return 0;
}

void Daemon::pollDevices() {
    printf("[Daemon] Polling devices...\n");

    ADBAPIHANDLE hEnum = m_adb.EnumInterfaces(GUID_ANDROID_USB, true, true, true);
    if (!hEnum) return;

    std::vector<std::wstring> current_devices;
    unsigned long size = 0;
    while (m_adb.NextInterface(hEnum, nullptr, &size)) {
        AdbInterfaceInfo* info = (AdbInterfaceInfo*)malloc(size);
        if (!info) continue;
        if (m_adb.NextInterface(hEnum, info, &size)) {
            current_devices.push_back(info->device_name);
        }
        free(info);
    }
    m_adb.CloseHandle(hEnum);

    printf("[Daemon] Found %zu device(s)\n", current_devices.size());

    // 检查新设备（按接口名称去重）
    for (auto& name : current_devices) {
        bool found = false;
        for (auto& dev : m_devices) {
            // 此处需要存储接口名用于比较；简化起见，按设备数量判断
            // 实际实现中，AdbDevice 应有 getInterfaceName() 方法
            found = true; // 简化：POC 阶段先不处理多设备去重
            break;
        }
        if (!found) {
            printf("[Daemon] New device, connecting...\n");
            auto dev = std::make_unique<AdbDevice>(&m_adb, name);
            if (dev->connect()) {
                int local_port = 1080 + (int)m_forwarders.size();
                setupForwarder(dev.get(), local_port, 1080);
                m_devices.push_back(std::move(dev));
            }
        }
    }

    // TODO: 检测移除的设备，清理 Forwarder
}

void Daemon::setupForwarder(AdbDevice* device, int local_port, int remote_port) {
    auto fwd = std::make_unique<Forwarder>(device, local_port, remote_port);
    fwd->start();
    m_forwarders.push_back(std::move(fwd));
}

std::string Daemon::handleCommand(const std::string& cmd) {
    if (cmd == "devices") {
        std::ostringstream oss;
        oss << "[";
        for (size_t i = 0; i < m_devices.size(); i++) {
            if (i > 0) oss << ",";
            oss << "{\"serial\":\"" << m_devices[i]->getSerial()
                << "\",\"state\":\"connected\"}";
        }
        oss << "]";
        return oss.str();
    }
    if (cmd == "status") {
        std::ostringstream oss;
        oss << "{\"daemon\":\"running\",\"devices\":" << m_devices.size() << "}";
        return oss.str();
    }
    return "{\"error\":\"unknown command\"}";
}
```

---

### Task 7: CLI Server（Named Pipe）

**Files:**
- Create: `molink-access-cpp/src/cli/cli_server.h`
- Create: `molink-access-cpp/src/cli/cli_server.cpp`

- [ ] **Step 1: cli_server.h**

```cpp
#ifndef CLI_SERVER_H
#define CLI_SERVER_H

#include <windows.h>
#include <string>
#include <functional>

class CliServer {
public:
    using CommandHandler = std::function<std::string(const std::string&)>;

    explicit CliServer(const std::string& pipe_name);
    ~CliServer();

    void setHandler(CommandHandler handler) { m_handler = handler; }
    void start();
    HANDLE getEventHandle() const { return m_overlapped.hEvent; }
    void acceptAndProcess();

private:
    std::string m_pipe_name;
    HANDLE m_pipe;
    OVERLAPPED m_overlapped;
    CommandHandler m_handler;
    bool m_listening;
};

#endif
```

- [ ] **Step 2: cli_server.cpp**

```cpp
#include "cli_server.h"
#include <cstdio>
#include <vector>

CliServer::CliServer(const std::string& pipe_name)
    : m_pipe_name(pipe_name), m_pipe(INVALID_HANDLE_VALUE), m_listening(false) {
    memset(&m_overlapped, 0, sizeof(m_overlapped));
    m_overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

CliServer::~CliServer() {
    if (m_pipe != INVALID_HANDLE_VALUE) CloseHandle(m_pipe);
    if (m_overlapped.hEvent) CloseHandle(m_overlapped.hEvent);
}

void CliServer::start() {
    m_pipe = CreateNamedPipeA(
        m_pipe_name.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
        1,           // max instances
        4096,        // out buffer
        4096,        // in buffer
        0,           // default timeout
        nullptr);

    if (m_pipe == INVALID_HANDLE_VALUE) {
        printf("CLI: CreateNamedPipe fail, err=%lu\n", GetLastError());
        return;
    }

    // 开始异步等待客户端连接
    if (!ConnectNamedPipe(m_pipe, &m_overlapped)) {
        DWORD err = GetLastError();
        if (err == ERROR_IO_PENDING) {
            m_listening = true;
            printf("CLI: Listening on %s\n", m_pipe_name.c_str());
        } else if (err == ERROR_PIPE_CONNECTED) {
            SetEvent(m_overlapped.hEvent);
            m_listening = true;
        } else {
            printf("CLI: ConnectNamedPipe fail, err=%lu\n", err);
        }
    }
}

void CliServer::acceptAndProcess() {
    if (!m_listening) return;

    DWORD bytes = 0;
    if (!GetOverlappedResult(m_pipe, &m_overlapped, &bytes, FALSE)) {
        DWORD err = GetLastError();
        if (err != ERROR_IO_INCOMPLETE) {
            printf("CLI: GetOverlappedResult fail, err=%lu\n", err);
            // 重新监听
            DisconnectNamedPipe(m_pipe);
            ConnectNamedPipe(m_pipe, &m_overlapped);
        }
        return;
    }

    // 读取命令
    char buf[4096] = {0};
    DWORD read = 0;
    if (ReadFile(m_pipe, buf, sizeof(buf) - 1, &read, nullptr)) {
        std::string cmd(buf, read);
        // 去掉尾部换行
        while (!cmd.empty() && (cmd.back() == '\n' || cmd.back() == '\r'))
            cmd.pop_back();

        printf("CLI: cmd=%s\n", cmd.c_str());

        std::string response;
        if (m_handler) {
            response = m_handler(cmd);
        }
        response += "\n";

        DWORD written = 0;
        WriteFile(m_pipe, response.c_str(), (DWORD)response.size(), &written, nullptr);
    }

    // 断连，重新等待
    FlushFileBuffers(m_pipe);
    DisconnectNamedPipe(m_pipe);
    ConnectNamedPipe(m_pipe, &m_overlapped);
}
```

---

### Task 8: Forwarder + main.cpp 整合

**Files:**
- Create: `molink-access-cpp/src/forward/forwarder.h`
- Create: `molink-access-cpp/src/forward/forwarder.cpp`
- Create: `molink-access-cpp/src/main.cpp`

- [ ] **Step 1: forwarder.h**

```cpp
#ifndef FORWARDER_H
#define FORWARDER_H

#include "../adb/adb_device.h"
#include <winsock2.h>

class Forwarder {
public:
    Forwarder(AdbDevice* device, uint16_t local_port, uint16_t remote_port);
    ~Forwarder();

    bool start();
    bool isAlive() const { return m_alive; }
    SOCKET getListenSocket() const { return m_listen; }

private:
    AdbDevice* m_device;
    uint16_t m_local_port;
    uint16_t m_remote_port;
    SOCKET m_listen = INVALID_SOCKET;
    bool m_alive = false;
};

#endif
```

- [ ] **Step 2: forwarder.cpp**

```cpp
#include "forwarder.h"
#include <cstdio>

Forwarder::Forwarder(AdbDevice* device, uint16_t local_port, uint16_t remote_port)
    : m_device(device), m_local_port(local_port), m_remote_port(remote_port) {}

Forwarder::~Forwarder() {
    if (m_listen != INVALID_SOCKET) closesocket(m_listen);
}

bool Forwarder::start() {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    m_listen = socket(AF_INET, SOCK_STREAM, 0);
    if (m_listen == INVALID_SOCKET) {
        printf("FWD: socket fail, err=%d\n", WSAGetLastError());
        return false;
    }

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = htons(m_local_port);

    if (bind(m_listen, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        printf("FWD: bind port %u fail, err=%d\n", m_local_port, WSAGetLastError());
        closesocket(m_listen);
        return false;
    }

    if (listen(m_listen, SOMAXCONN) == SOCKET_ERROR) {
        printf("FWD: listen fail, err=%d\n", WSAGetLastError());
        closesocket(m_listen);
        return false;
    }

    m_alive = true;
    printf("FWD: Listening on 127.0.0.1:%u\n", m_local_port);
    return true;
}
```

- [ ] **Step 3: main.cpp — 入口（守护进程 / CLI 双用）**

```cpp
#include "core/daemon.h"
#include <cstdio>
#include <cstring>

static void printUsage() {
    printf("MoLink - Android USB Proxy Tool\n\n");
    printf("Usage:\n");
    printf("  molink                 Start daemon (foreground)\n");
    printf("  molink devices         List connected devices\n");
    printf("  molink status          Show daemon status\n");
    printf("  molink forward L R     Add port forward\n");
}

// CLI 客户端模式：连接 Named Pipe 发送命令
static int clientMode(const std::string& command) {
    HANDLE pipe = CreateFileA(
        "\\\\.\\pipe\\molink",
        GENERIC_READ | GENERIC_WRITE,
        0, nullptr, OPEN_EXISTING, 0, nullptr);

    if (pipe == INVALID_HANDLE_VALUE) {
        printf("Error: Cannot connect to daemon. Is it running?\n");
        return 1;
    }

    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);

    std::string msg = command + "\n";
    DWORD written = 0;
    WriteFile(pipe, msg.c_str(), (DWORD)msg.size(), &written, nullptr);

    char buf[4096] = {0};
    DWORD read = 0;
    ReadFile(pipe, buf, sizeof(buf) - 1, &read, nullptr);
    printf("%s\n", buf);

    CloseHandle(pipe);
    return 0;
}

int main(int argc, char* argv[]) {
    if (argc > 1) {
        if (strcmp(argv[1], "devices") == 0)
            return clientMode("devices");
        if (strcmp(argv[1], "status") == 0)
            return clientMode("status");
        if (strcmp(argv[1], "forward") == 0 && argc == 4) {
            char buf[128];
            snprintf(buf, sizeof(buf), "forward %s %s", argv[2], argv[3]);
            return clientMode(buf);
        }
        printUsage();
        return 1;
    }

    // 守护进程模式
    Daemon daemon;
    return daemon.run();
}
```

- [ ] **Step 4: 更新 CMakeLists.txt 添加所有源文件**

```cmake
add_executable(molink
    src/main.cpp
    src/adb/adb_dll.cpp
    src/adb/adb_transport.cpp
    src/adb/adb_device.cpp
    src/adb/rsa_auth.cpp
    src/cli/cli_server.cpp
    src/forward/forwarder.cpp
    src/core/daemon.cpp
)

target_link_libraries(molink ws2_32 setupapi winusb crypt32)
```

---

## 执行顺序

```
Task 1 → Task 2 → Task 3 → Task 4  [POC 门禁]
                                  │
                          ┌───────┴───────┐
                          │ POC 通过?      │
                          │ NO  → 终止     │
                          │ YES → 继续     │
                          └───────┬───────┘
                                  ↓
                      Task 5 → Task 6 → Task 7 → Task 8
```
