# apush/apull & start/forward 分离 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 1) 实现 apush/apull 智能文件传输（目录自动压缩/解压、base64、.gitignore）；2) 分离 molink start 和 molink forward

**Architecture:** CLI 层负责 zip/unzip/base64/gitignore 逻辑，通过现有 daemon pipe 命令（push/pull/ls）完成传输，daemon 传输层零改动。forward 独立为 pipe 命令，daemonMain 中 Forwarder 改为按需创建。

**Tech Stack:** C++17, MinGW-w64, CMake, miniz (single-file zip), Win32 API

---

## Phase 1: 基础库和工具

### Task 1: 下载并配置 miniz

**Files:**
- Create: `third_party/miniz/miniz.h`
- Create: `third_party/miniz/miniz.c`
- Create: `third_party/miniz/README.md`

- [ ] **Step 1: 创建目录并下载 miniz**

```powershell
mkdir -p D:\project\MoLink\molink-access-cpp\third_party\miniz
curl --proxy http://127.0.0.1:7890 -L -o D:\project\MoLink\molink-access-cpp\third_party\miniz\miniz.h https://raw.githubusercontent.com/richgel999/miniz/refs/heads/master/miniz.h
curl --proxy http://127.0.0.1:7890 -L -o D:\project\MoLink\molink-access-cpp\third_party\miniz\miniz.c https://raw.githubusercontent.com/richgel999/miniz/refs/heads/master/miniz.c
```

若无代理则去掉 `--proxy http://127.0.0.1:7890`。

- [ ] **Step 2: 验证文件存在**

```powershell
dir D:\project\MoLink\molink-access-cpp\third_party\miniz\
```

预期: 看到 `miniz.h` 和 `miniz.c`。

- [ ] **Step 3: 编写 README.md**

文件 `third_party/miniz/README.md`：

```markdown
# miniz

单文件公域授权 zip 库，提供压缩/解压 API。

- 来源: https://github.com/richgel999/miniz
- 版本: master (amalgamated)
- 许可: Public Domain / MIT
- 集成: miniz.c 直接编译进 molink.exe，无需预编译静态库
```

---

### Task 2: 实现 base64

**Files:**
- Create: `src/utils/base64.h`

- [ ] **Step 1: 创建 utils 目录**

```powershell
mkdir -p D:\project\MoLink\molink-access-cpp\src\utils
```

- [ ] **Step 2: 编写 base64.h**

文件 `src/utils/base64.h`：

```cpp
#ifndef BASE64_H
#define BASE64_H

#include <string>
#include <vector>
#include <cstdint>

inline std::string base64Encode(const std::string& input) {
    static const char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string output;
    output.reserve(((input.size() + 2) / 3) * 4);

    const uint8_t* data = (const uint8_t*)input.data();
    size_t i = 0;
    for (; i + 2 < input.size(); i += 3) {
        output.push_back(kAlphabet[(data[i] >> 2) & 0x3F]);
        output.push_back(kAlphabet[((data[i] & 0x03) << 4) | ((data[i + 1] >> 4) & 0x0F)]);
        output.push_back(kAlphabet[((data[i + 1] & 0x0F) << 2) | ((data[i + 2] >> 6) & 0x03)]);
        output.push_back(kAlphabet[data[i + 2] & 0x3F]);
    }

    size_t remaining = input.size() - i;
    if (remaining == 1) {
        output.push_back(kAlphabet[(data[i] >> 2) & 0x3F]);
        output.push_back(kAlphabet[(data[i] & 0x03) << 4]);
        output.push_back('=');
        output.push_back('=');
    } else if (remaining == 2) {
        output.push_back(kAlphabet[(data[i] >> 2) & 0x3F]);
        output.push_back(kAlphabet[((data[i] & 0x03) << 4) | ((data[i + 1] >> 4) & 0x0F)]);
        output.push_back(kAlphabet[(data[i + 1] & 0x0F) << 2]);
        output.push_back('=');
    }

    return output;
}

inline std::string base64Decode(const std::string& input) {
    static const uint8_t kDecode[128] = {
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,62,0,0,0,63,
        52,53,54,55,56,57,58,59,60,61,0,0,0,0,0,0,
        0,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,0,0,0,0,0,
        0,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,0,0,0,0,0
    };

    std::string output;
    output.reserve((input.size() / 4) * 3);

    int val = 0, valb = -8;
    for (unsigned char c : input) {
        if (c >= 128 || (c != '=' && kDecode[c] == 0 && c != 'A')) continue;
        val = (val << 6) + kDecode[c];
        valb += 6;
        if (valb >= 0) {
            output.push_back(char((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return output;
}

#endif
```

---

### Task 3: 实现 gitignore 匹配器

**Files:**
- Create: `src/utils/gitignore.h`
- Create: `src/utils/gitignore.cpp`

- [ ] **Step 1: 编写 gitignore.h**

文件 `src/utils/gitignore.h`：

```cpp
#ifndef GITIGNORE_H
#define GITIGNORE_H

#include <string>
#include <vector>
#include <functional>

// gitwildmatch 子集匹配器
class GitignoreMatcher {
public:
    GitignoreMatcher() {}

    // 从 .gitignore 文件路径加载规则
    bool load(const std::string& gitignorePath);

    // 检查路径是否被忽略
    // filePath: 相对于仓库根的路径（使用正斜杠）
    // isDir: 该路径是否为目录
    bool isIgnored(const std::string& filePath, bool isDir) const;

    // 是否有有效规则
    bool hasRules() const { return !m_rules.empty(); }

private:
    struct Rule {
        std::string pattern;    // 原始 pattern
        bool negate;            // 是否取反（! 开头）
        bool dirOnly;           // 是否仅匹配目录（/ 结尾）
        bool anchored;          // 是否锚定到根（含 /）
        std::string regex;      // 转换后的正则表达式片段
    };

    std::vector<Rule> m_rules;

    // 将 gitignore pattern 转换为内部匹配用字符串
    static std::string patternToMatch(const std::string& pattern);
    static bool matchPattern(const std::string& path, const std::string& pattern);
};

// 便捷函数：从路径检测并加载 .gitignore
// 先查当前目录，再查 folderPath 自身
std::string findGitignore(const std::string& folderPath);

#endif
```

- [ ] **Step 2: 编写 gitignore.cpp**

文件 `src/utils/gitignore.cpp`：

```cpp
#include "gitignore.h"
#include <fstream>
#include <algorithm>
#include <cstdio>

std::string findGitignore(const std::string& folderPath) {
    // 先查当前工作目录
    std::string cwdGi = ".gitignore";
    std::ifstream testCwd(cwdGi);
    if (testCwd.good()) return cwdGi;

    // 再查 folderPath 自身
    std::string folderGi;
    // 规范化路径
    for (char c : folderPath) {
        folderGi += (c == '\\') ? '/' : c;
    }
    while (!folderGi.empty() && folderGi.back() == '/')
        folderGi.pop_back();
    folderGi += "/.gitignore";

    std::ifstream testFolder(folderGi);
    if (testFolder.good()) return folderGi;

    return "";
}

bool GitignoreMatcher::load(const std::string& gitignorePath) {
    m_rules.clear();

    std::ifstream f(gitignorePath);
    if (!f.good()) return false;

    std::string line;
    while (std::getline(f, line)) {
        // 去掉 \r
        if (!line.empty() && line.back() == '\r') line.pop_back();
        // 跳过空行和注释
        if (line.empty() || line[0] == '#') continue;
        // 跳过以 \ 开头的行
        if (line[0] == '\\' && line.size() > 1) line = line.substr(1);

        Rule rule;
        rule.negate = (line[0] == '!');
        if (rule.negate) line = line.substr(1);

        // 去掉首尾空格
        while (!line.empty() && line.back() == ' ') line.pop_back();

        if (line.empty()) continue;

        rule.dirOnly = (!line.empty() && line.back() == '/');
        if (rule.dirOnly) line.pop_back();

        // 去掉尾部空格（/ 之后）
        while (!line.empty() && line.back() == ' ') line.pop_back();

        rule.anchored = (line.find('/') != std::string::npos && !(
            line.size() >= 3 && line[0] == '*' && line[1] == '*' && line[2] == '/'));

        // 去掉开头的 / 锚定符
        if (!line.empty() && line[0] == '/') {
            line = line.substr(1);
            rule.anchored = true;
        }

        rule.pattern = line;
        rule.regex = patternToMatch(line);
        m_rules.push_back(rule);
    }
    return true;
}

std::string GitignoreMatcher::patternToMatch(const std::string& pattern) {
    std::string result;
    for (size_t i = 0; i < pattern.size(); i++) {
        if (pattern[i] == '*' && i + 1 < pattern.size() && pattern[i + 1] == '*') {
            // **: 匹配任意深度
            if (i + 2 < pattern.size() && pattern[i + 2] == '/') {
                result += "(.*/)?";
                i += 2;
            } else if (i + 2 == pattern.size()) {
                result += ".*";
                i += 1;
            } else {
                result += "[^/]*";
            }
        } else if (pattern[i] == '*') {
            result += "[^/]*";
        } else if (pattern[i] == '?') {
            result += "[^/]";
        } else if (pattern[i] == '.') {
            result += "\\.";
        } else {
            result += pattern[i];
        }
    }
    return result;
}

bool GitignoreMatcher::matchPattern(const std::string& path, const std::string& pattern) {
    // 简化实现：递归匹配
    if (pattern.empty()) return path.empty();
    if (pattern[0] == '*') {
        // 处理 **/ 前缀
        if (pattern.size() >= 3 && pattern[0] == '*' && pattern[1] == '*' && pattern[2] == '/') {
            std::string rest = pattern.substr(3);
            if (matchPattern(path, rest)) return true;
            for (size_t i = 0; i < path.size(); i++) {
                if (path[i] == '/' && matchPattern(path.substr(i + 1), rest)) return true;
            }
            return false;
        }
    }
    // 简化：直接字符串匹配 + 通配符
    size_t pi = 0, si = 0;
    size_t starIdx = std::string::npos;
    size_t matchIdx = 0;

    while (si < path.size()) {
        if (pi < pattern.size() && pattern[pi] == '*') {
            starIdx = pi;
            matchIdx = si;
            pi++;
        } else if (pi < pattern.size() && (pattern[pi] == '?' ||
                   tolower(pattern[pi]) == tolower(path[si]))) {
            pi++;
            si++;
        } else if (starIdx != std::string::npos) {
            pi = starIdx + 1;
            matchIdx++;
            si = matchIdx;
        } else {
            return false;
        }
    }
    while (pi < pattern.size() && pattern[pi] == '*') pi++;
    return pi == pattern.size();
}

bool GitignoreMatcher::isIgnored(const std::string& filePath, bool isDir) const {
    bool ignored = false;

    for (const auto& rule : m_rules) {
        // 目录规则仅匹配目录
        if (rule.dirOnly && !isDir) continue;

        bool matches = false;

        if (rule.anchored) {
            matches = matchPattern(filePath, rule.pattern);
        } else {
            // 非锚定：匹配文件名的任意层级
            std::string basename = filePath;
            size_t lastSlash = basename.find_last_of('/');
            if (lastSlash != std::string::npos)
                basename = basename.substr(lastSlash + 1);

            matches = matchPattern(basename, rule.pattern);
            // 也尝试匹配全路径
            if (!matches)
                matches = matchPattern(filePath, rule.pattern);
        }

        if (matches) {
            ignored = !rule.negate;
        }
    }

    return ignored;
}
```

- [ ] **Step 3: 编译验证**

```powershell
cd D:\project\MoLink\molink-access-cpp\build
cmake --build . --config Release
```

预期: 编译通过（先更新 CMakeLists.txt — 见 Task 4）。

---

### Task 4: 更新 CMakeLists.txt

**Files:**
- Modify: `CMakeLists.txt`

- [ ] **Step 1: 添加 include 路径和新源文件**

在 `include_directories` 行之后添加 miniz 和 utils 路径：

```cmake
include_directories(${CMAKE_SOURCE_DIR}/third_party/libusb/include)
include_directories(${CMAKE_SOURCE_DIR}/third_party/miniz)
include_directories(${CMAKE_SOURCE_DIR}/src/utils)
```

在 `add_executable` 块末尾添加新源文件：

```cmake
add_executable(molink
    src/main.cpp
    src/usb/usb_device.cpp
    src/adb/adb_transport.cpp
    src/adb/adb_rsa.cpp
    src/adb/adb_reader.cpp
    src/adb/adb_client.cpp
    src/adb/adb_sync.cpp
    src/adb/adb_shell.cpp
    src/cli/named_pipe.cpp
    src/forward/forwarder.cpp
    src/transfer/file_push.cpp
    src/transfer/file_pull.cpp
    src/transfer/file_list.cpp
    third_party/miniz/miniz.c
    src/utils/gitignore.cpp
)
```

注意：`miniz.c` 是 C 文件。CMake 会根据扩展名自动用 C 编译器编译，如果报错，需确认 MinGW 包含 C 编译器。

若需显式启用 C 编译器，在 `project(molink LANGUAGES CXX)` 改为 `project(molink LANGUAGES C CXX)`。

- [ ] **Step 2: 编译验证**

```powershell
cd D:\project\MoLink\molink-access-cpp\build
cmake .. -G "MinGW Makefiles"
cmake --build . --config Release
```

预期: 编译通过。若 `miniz.c` 编译报错（C99 特性），检查 CMake 中 C 标准设置，必要时添加 `set(CMAKE_C_STANDARD 99)`。

---

## Phase 2: start/forward 分离

### Task 5: daemonMain 中 Forwarder 改为按需创建

**Files:**
- Modify: `src/forward/forwarder.h` — 添加 `getRemotePort()`
- Modify: `src/main.cpp` — daemonMain 函数

- [ ] **Step 1: Forwarder 添加 getRemotePort()**

在 `forwarder.h` 的 public 区域，`getLocalPort()` 之后添加：

```cpp
uint16_t getRemotePort() const { return m_remotePort; }
```

- [ ] **Step 2: daemonMain 中 Forwarder 改为 unique_ptr**

在 `src/main.cpp` 的 `daemonMain` 函数中（约 line 261），将：

```cpp
Forwarder forwarder(client, localPort, remotePort);
if (!forwarder.start()) {
    printf("FAIL: Cannot start forwarder\n");
    CloseHandle(hDisconnectEvent);
    releaseDaemonLock();
    return 1;
}
```

改为：

```cpp
// Forwarder 按需创建，不在启动时自动启动转发
std::unique_ptr<Forwarder> forwarder;
```

**同时**：删除 `daemonMain` 函数签名中的 `remotePort` 参数的后续使用（daemonMain 仍接收 `remotePort`，但不在启动时用它）。

注意：局部对象 `localPort` 和 `remotePort` 仍需要被 lambda 捕获引用，供后续 forward 命令使用。

- [ ] **Step 3: 更新 status pipe 处理**

在 pipe handler 中（约 line 277-287），将 status 处理改为：

```cpp
if (cmd == "status") {
    char buf[256];
    const char* stateStr = (client.getState() == DaemonState::CONNECTED)
        ? "connected" : "disconnected";
    if (forwarder) {
        snprintf(buf, sizeof(buf),
                 "%s  serial=%s  forwarding=%u→%u  connections=%d",
                 stateStr,
                 client.getSerial().c_str(),
                 forwarder->getLocalPort(),
                 forwarder->getRemotePort(),
                 forwarder->getConnectionCount());
    } else {
        snprintf(buf, sizeof(buf),
                 "%s  serial=%s  forwarding=off",
                 stateStr,
                 client.getSerial().c_str());
    }
    return buf;
}
```

- [ ] **Step 4: 更新 transition lambda 中的 forwarder 引用**

`transitionToDisconnected` lambda 中 `forwarder.pause()` 改为：

```cpp
if (forwarder) forwarder->pause();
```

`tryReconnect` lambda 中 `forwarder.resume()` 改为：

```cpp
if (forwarder) forwarder->resume();
```

- [ ] **Step 5: 更新清理逻辑**

daemonMain 末尾清理代码（约 line 389-391）：

```cpp
printf("Daemon stopping...\n");
pipe.stop();
if (forwarder) forwarder->stop();
client.disconnect();
```

- [ ] **Step 6: 编译验证**

```powershell
cd D:\project\MoLink\molink-access-cpp\build
cmake --build . --config Release
```

预期: 编译通过。

---

### Task 6: 添加 forward pipe 命令

**Files:**
- Modify: `src/main.cpp` — daemonMain pipe handler

- [ ] **Step 1: 在 pipe handler 中添加 forward 命令**

在 `ls` 命令处理之后、`return "unknown";` 之前：

```cpp
if (cmd.rfind("forward ", 0) == 0) {
    std::string args = cmd.substr(8);
    size_t space = args.find(' ');
    if (space == std::string::npos)
        return "fail: Usage: forward <localPort> <remotePort>";
    uint16_t lp = (uint16_t)atoi(args.substr(0, space).c_str());
    uint16_t rp = (uint16_t)atoi(args.substr(space + 1).c_str());

    if (forwarder) {
        forwarder->stop();
        forwarder.reset();
    }

    forwarder = std::make_unique<Forwarder>(client, lp, rp);
    if (!forwarder->start()) {
        forwarder.reset();
        return "fail: Cannot start forwarding";
    }
    char buf[128];
    snprintf(buf, sizeof(buf), "ok: forwarding 127.0.0.1:%u → device tcp:%u", lp, rp);
    return buf;
}
```

- [ ] **Step 2: 编译验证**

```powershell
cmake --build . --config Release
```

---

### Task 7: CLI forward 命令 + 更新帮助

**Files:**
- Modify: `src/main.cpp` — cmdStart, cmdForward, main dispatch, printUsage

- [ ] **Step 1: 简化 cmdStart**

`cmdStart` 函数中，移除等待 status 时的 "connected" 条件检查。将第 443-444 行：

```cpp
if (!resp.empty() && resp.find("connected") != std::string::npos) {
    printf("%s\n", resp.c_str());
```

改为：

```cpp
if (!resp.empty()) {
    printf("%s\n", resp.c_str());
```

这样 `molink start` 启动后 daemon 存活且响应 pipe 即可，不要求 connect 和 forwarding。

- [ ] **Step 2: 新增 cmdForward 函数**

在 `cmdStop()` 函数之后，`daemonMain` 之前添加：

```cpp
static int cmdForward(int argc, char* argv[]) {
    uint16_t lp = 1080;
    uint16_t rp = 1081;

    for (int i = 2; i < argc; i++) {
        if ((strcmp(argv[i], "--port") == 0 || strcmp(argv[i], "-p") == 0) && i + 1 < argc)
            lp = (uint16_t)atoi(argv[++i]);
        else if ((strcmp(argv[i], "--rport") == 0 || strcmp(argv[i], "-r") == 0) && i + 1 < argc)
            rp = (uint16_t)atoi(argv[++i]);
    }

    std::string cmd = "forward " + std::to_string(lp) + " " + std::to_string(rp);
    auto resp = sendPipeCmd(cmd);
    if (resp.empty()) {
        printf("Daemon is not running. Use 'molink start' first.\n");
        return 1;
    }
    printf("%s\n", resp.c_str());
    return (resp.rfind("ok", 0) == 0) ? 0 : 1;
}
```

- [ ] **Step 3: 在 main() 中添加 forward 分发**

在 `strcmp(argv[1], "stop")` 之后、`strcmp(argv[1], "push")` 之前添加：

```cpp
if (strcmp(argv[1], "forward") == 0) return cmdForward(argc, argv);
```

- [ ] **Step 4: 更新 printUsage**

```cpp
static void printUsage() {
    printf("MoLink Access - ADB USB Proxy\n\n"
           "Usage:\n"
           "  molink run      [options]         Run in foreground\n"
           "  molink start    [options]         Start daemon in background\n"
           "  molink stop                       Stop running daemon\n"
           "  molink forward  [options]         Start port forwarding (requires daemon)\n"
           "  molink devices                    List ADB devices\n"
           "  molink status                     Show daemon status\n"
           "  molink push     <local> <remote>  Upload file to device\n"
           "  molink pull     <remote> <local>  Download file from device\n"
           "  molink ls       [remote_path]     List device directory\n"
           "  molink apush    <path> [options]  Auto push (dir→zip, .gitignore)\n"
           "  molink apull    [options]         Interactive auto pull (unzip)\n"
           "  molink --help                     Show this help\n\n"
           "Options:\n"
           "  --port, -p <port>                 Local TCP port (default: 1080)\n"
           "  --rport, -r <port>                Remote device port (default: 1081)\n"
           "  --serial, -s <sn>                 Device serial number\n");
}
```

- [ ] **Step 5: 编译验证**

```powershell
cmake --build . --config Release
```

---

### Task 8: 构建并测试 start/forward 分离

- [ ] **Step 1: 测试 start 不再自动转发**

```powershell
cd D:\project\MoLink\molink-access-cpp\build
.\molink.exe stop
.\molink.exe start
.\molink.exe status
```

预期: `connected  serial=...  forwarding=off`

- [ ] **Step 2: 测试 push/pull/ls 在 start 后可用（无需 forward）**

```powershell
echo test > test_apush.txt
.\molink.exe push test_apush.txt /sdcard/test_apush.txt
.\molink.exe pull /sdcard/test_apush.txt test_pull.txt
.\molink.exe ls /sdcard/
```

预期: push 返回 ok，pull 返回 ok，ls 显示目录内容。

- [ ] **Step 3: 测试 forward 命令（默认端口）**

```powershell
.\molink.exe forward
.\molink.exe status
```

预期: `forwarding=1080→1081  connections=0`

- [ ] **Step 4: 测试 forward 自定义端口**

```powershell
.\molink.exe forward -p 2080 -r 1081
.\molink.exe status
```

预期: `forwarding=2080→1081`

- [ ] **Step 5: 测试 forward 切换端口**

```powershell
.\molink.exe forward -p 3080 -r 1081
.\molink.exe status
```

预期: 旧转发被替换，显示 `forwarding=3080→1081`

- [ ] **Step 6: 测试 forward 无 daemon 时报错**

```powershell
.\molink.exe stop
.\molink.exe forward
```

预期: `Daemon is not running. Use 'molink start' first.`

- [ ] **Step 7: 清理**

```powershell
.\molink.exe start
del test_apush.txt test_pull.txt 2>nul
```

---

## Phase 3: apush / apull

### Task 9: 实现 apush CLI 逻辑

**Files:**
- Modify: `src/main.cpp` — 添加 cmdApush 函数 + 所需辅助

- [ ] **Step 1: 在 main.cpp 顶部添加 include**

```cpp
#include <filesystem>
#include "utils/base64.h"
#include "utils/gitignore.h"
#include "third_party/miniz/miniz.h"
```

- [ ] **Step 2: 添加辅助函数 compressFolder**

在 `cmdDevices()` 之前添加：

```cpp
namespace fs = std::filesystem;

static std::string compressFolder(const std::string& folderPath,
                                   const std::string& zipPath,
                                   GitignoreMatcher* gitSpec) {
    mz_zip_archive zip = {};
    if (!mz_zip_writer_init_file(&zip, zipPath.c_str(), 0)) {
        return "fail: Cannot create zip file";
    }

    std::error_code ec;
    std::string base = fs::absolute(folderPath, ec).string();
    if (ec) {
        mz_zip_writer_end(&zip);
        remove(zipPath.c_str());
        return "fail: Cannot resolve folder path";
    }
    std::replace(base.begin(), base.end(), '\\', '/');
    if (!base.empty() && base.back() != '/') base += '/';

    int fileCount = 0;
    for (auto& entry : fs::recursive_directory_iterator(folderPath, ec)) {
        if (ec) break;

        std::string absPath = entry.path().string();
        std::replace(absPath.begin(), absPath.end(), '\\', '/');

        // 计算相对路径
        std::string relPath;
        if (absPath.size() >= base.size())
            relPath = absPath.substr(base.size());

        bool isDir = entry.is_directory(ec);

        // 跳过 .git
        if (relPath.find(".git/") == 0 || relPath == ".git") continue;

        // 跳过 .gitignore 自身
        if (relPath == ".gitignore") continue;

        // 应用 gitignore 规则
        if (gitSpec && gitSpec->hasRules() && gitSpec->isIgnored(relPath, isDir))
            continue;

        if (isDir) {
            // 目录条目（以 / 结尾）
            std::string dirEntry = relPath + "/";
            if (!mz_zip_writer_add_mem(&zip, dirEntry.c_str(), nullptr, 0, MZ_DEFAULT_COMPRESSION)) {
                // 忽略目录添加失败
            }
        } else {
            // 读取文件内容
            std::ifstream f(absPath, std::ios::binary);
            if (!f.good()) continue;
            std::string content((std::istreambuf_iterator<char>(f)),
                                 std::istreambuf_iterator<char>());

            if (!mz_zip_writer_add_mem(&zip, relPath.c_str(),
                                       content.data(), content.size(),
                                       MZ_BEST_COMPRESSION)) {
                mz_zip_writer_end(&zip);
                remove(zipPath.c_str());
                return "fail: Cannot add file to zip: " + relPath;
            }
            fileCount++;
        }
    }

    if (!mz_zip_writer_finalize_archive(&zip)) {
        mz_zip_writer_end(&zip);
        remove(zipPath.c_str());
        return "fail: Cannot finalize zip";
    }
    mz_zip_writer_end(&zip);

    if (fileCount == 0) {
        remove(zipPath.c_str());
        return "fail: No files to compress";
    }
    return "ok";
}
```

注：miniz API 调用若编译报错，需要检查 miniz.h 中的实际函数签名。miniz 3.x 的 API 与 2.x 可能有差异。若 `mz_zip_writer_init_file` 不存在，则改用 `mz_zip_writer_init` + `mz_zip_writer_add_mem` + `mz_zip_writer_finalize_heap` 并在最后写入文件。需要根据实际下载的 miniz.h 调整。

- [ ] **Step 3: 添加 cmdApush 函数**

在 `main()` 函数之前添加：

```cpp
static const char* kRemoteDir = "/sdcard/tmp";
static const char* kEncodePrefix = "b64_";

static int cmdApush(int argc, char* argv[]) {
    if (argc < 3) {
        printf("Usage: molink apush <path> [--git] [--no-git] [--rdir <remote_dir>]\n");
        return 1;
    }

    std::string path = argv[2];
    std::string rdir = kRemoteDir;

    // 解析选项
    enum { AUTO, ENABLED, DISABLED } gitMode = AUTO;
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--git") == 0) gitMode = ENABLED;
        else if (strcmp(argv[i], "--no-git") == 0) gitMode = DISABLED;
        else if (strcmp(argv[i], "--rdir") == 0 && i + 1 < argc) rdir = argv[++i];
    }

    if (!fs::exists(path)) {
        printf("文件/目录不存在: %s\n", path.c_str());
        return 1;
    }

    // daemon 存活检查
    if (sendPipeCmd("status").empty()) {
        printf("Daemon is not running. Use 'molink start' first.\n");
        return 1;
    }

    std::string originalName = fs::path(path).filename().string();
    GitignoreMatcher gitSpec;
    int ignoredCount = 0;

    if (fs::is_directory(path)) {
        // 检测 .gitignore
        bool useGitignore = false;
        std::string gitignorePath;

        if (gitMode == AUTO || gitMode == ENABLED) {
            gitignorePath = findGitignore(path);
            useGitignore = (gitignorePath.empty() ? false : true);
            if (gitMode == ENABLED && !useGitignore) {
                printf("警告: 未找到 .gitignore 文件\n");
            }
        }

        if (useGitignore) {
            gitSpec.load(gitignorePath);
            // 统计将忽略的文件数
            std::error_code ec;
            std::string base = fs::absolute(path, ec).string();
            std::replace(base.begin(), base.end(), '\\', '/');
            if (!base.empty() && base.back() != '/') base += '/';
            for (auto& entry : fs::recursive_directory_iterator(path, ec)) {
                if (ec) break;
                std::string absPath = entry.path().string();
                std::replace(absPath.begin(), absPath.end(), '\\', '/');
                std::string rel;
                if (absPath.size() >= base.size()) rel = absPath.substr(base.size());
                if (rel.find(".git/") == 0 || rel == ".git" || rel == ".gitignore") continue;
                bool isDir = entry.is_directory(ec);
                if (gitSpec.hasRules() && gitSpec.isIgnored(rel, isDir)) ignoredCount++;
            }
            printf("检测到 .gitignore，将忽略 %d 个路径\n", ignoredCount);
        }

        // 压缩
        printf("检测到文件夹，正在压缩...\n");
        char tempDir[MAX_PATH];
        GetTempPathA(sizeof(tempDir), tempDir);
        std::string zipPath = std::string(tempDir) + originalName + ".molink.zip";

        std::string result = compressFolder(path, zipPath,
                                            (useGitignore ? &gitSpec : nullptr));
        if (result != "ok") {
            printf("%s\n", result.c_str());
            return 1;
        }

        // push
        std::string remoteName = kEncodePrefix + base64Encode(originalName + ".molink.zip");
        std::string pipeCmd = "push " + zipPath + " " + rdir + "/" + remoteName;
        auto resp = sendPipeCmd(pipeCmd);
        printf("%s\n", resp.c_str());

        // 清理
        remove(zipPath.c_str());

        if (resp == "ok") {
            printf("已上传文件夹: %s -> %s/%s\n", originalName.c_str(), rdir.c_str(), remoteName.c_str());
            if (ignoredCount > 0) printf("（已忽略 %d 个路径）\n", ignoredCount);
        }
        return (resp == "ok") ? 0 : 1;

    } else {
        // 单文件
        std::string remoteName = kEncodePrefix + base64Encode(originalName);
        std::string pipeCmd = "push " + path + " " + rdir + "/" + remoteName;
        auto resp = sendPipeCmd(pipeCmd);
        printf("%s\n", resp.c_str());
        if (resp == "ok") {
            printf("已上传: %s -> %s/%s\n", originalName.c_str(), rdir.c_str(), remoteName.c_str());
        }
        return (resp == "ok") ? 0 : 1;
    }
}
```

- [ ] **Step 4: 在 main() 中添加 apush 分发**

在 `strcmp(argv[1], "ls")` 处理之后，添加：

```cpp
if (strcmp(argv[1], "apush") == 0) return cmdApush(argc, argv);
```

- [ ] **Step 5: 编译验证**

```powershell
cd D:\project\MoLink\molink-access-cpp\build
cmake --build . --config Release
```

预期: 编译通过。若 miniz API 报错，根据实际 miniz.h 调整函数名。

---

### Task 10: 实现 apull CLI 逻辑

**Files:**
- Modify: `src/main.cpp` — 添加 cmdApull 函数 + extractZip

- [ ] **Step 1: 添加辅助函数 extractZip**

在 `cmdApush` 之前添加：

```cpp
static std::pair<std::string, int> extractZip(const std::string& zipPath,
                                                const std::string& destDir) {
    mz_zip_archive zip = {};
    if (!mz_zip_reader_init_file(&zip, zipPath.c_str(), 0)) {
        return {"fail: Cannot open zip file", 0};
    }

    int fileCount = (int)mz_zip_reader_get_num_files(&zip);

    for (int i = 0; i < fileCount; i++) {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&zip, i, &stat)) continue;

        std::string entryName(stat.m_filename);
        // 跳过目录条目
        if (entryName.empty() || entryName.back() == '/') continue;

        // 解压文件
        size_t size = 0;
        void* data = mz_zip_reader_extract_to_heap(&zip, i, &size, 0);
        if (!data) continue;

        std::string outPath = destDir + "\\" + entryName;
        std::replace(outPath.begin(), outPath.end(), '/', '\\');

        // 确保父目录存在
        size_t lastSep = outPath.find_last_of('\\');
        if (lastSep != std::string::npos) {
            std::string parent = outPath.substr(0, lastSep);
            // 递归创建
            std::string partial;
            for (char c : parent) {
                if (c == '\\') {
                    CreateDirectoryA(partial.c_str(), nullptr);
                }
                partial += c;
            }
            CreateDirectoryA(partial.c_str(), nullptr);
        }

        FILE* f = fopen(outPath.c_str(), "wb");
        if (f) {
            fwrite(data, 1, size, f);
            fclose(f);
        }
        mz_free(data);
    }

    mz_zip_reader_end(&zip);
    return {"ok", fileCount};
}
```

注：`mz_zip_reader_extract_to_heap` 在 miniz 3.x 中可能不存在或签名不同。若编译报错，改用 `mz_zip_reader_extract_to_mem` 或先 `mz_zip_reader_file_stat` 获取 `m_uncomp_size`，再手动分配内存 + `mz_zip_reader_extract_to_mem`。根据实际下载的 miniz.h 调整。

- [ ] **Step 2: 添加 cmdApull 函数**

```cpp
static int cmdApull(int argc, char* argv[]) {
    std::string rdir = kRemoteDir;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--rdir") == 0 && i + 1 < argc) rdir = argv[++i];
    }

    // daemon 存活检查
    if (sendPipeCmd("status").empty()) {
        printf("Daemon is not running. Use 'molink start' first.\n");
        return 1;
    }

    // 获取远程文件列表
    std::string lsCmd = "ls " + rdir;
    std::string lsOutput = sendPipeCmd(lsCmd);
    if (lsOutput.empty() || lsOutput.find("fail:") == 0) {
        printf("目录不存在或为空: %s\n", rdir.c_str());
        return 1;
    }

    // 解析 ls -la 输出，提取 b64_ 开头的文件
    struct RemoteFile {
        std::string rawName;      // 远程文件名 (b64_xxx)
        std::string displayName;  // 解码后的显示名
        bool isZip;
    };
    std::vector<RemoteFile> files;

    std::istringstream ss(lsOutput);
    std::string line;
    while (std::getline(ss, line)) {
        // 跳过 total 行和空行
        if (line.empty() || line.find("total ") == 0) continue;
        // 跳过以 d/c/l 开头的行（目录、字符设备、链接）
        if (!line.empty() && (line[0] == 'd' || line[0] == 'c' || line[0] == 'l' || line[0] == 't'))
            continue;

        // 按空白切分取最后一段
        std::istringstream ls(line);
        std::string token, lastToken;
        while (ls >> token) lastToken = token;

        if (lastToken.empty()) continue;

        // 仅处理 b64_ 前缀的文件
        if (lastToken.find(kEncodePrefix) != 0) continue;

        RemoteFile rf;
        rf.rawName = lastToken;
        std::string encoded = lastToken.substr(strlen(kEncodePrefix));
        rf.displayName = base64Decode(encoded);
        rf.isZip = (rf.displayName.size() >= 10 &&
                    rf.displayName.substr(rf.displayName.size() - 10) == ".molink.zip");

        files.push_back(rf);
    }

    if (files.empty()) {
        printf("没有可下载的文件（b64_ 前缀）\n");
        return 1;
    }

    // 交互菜单
    printf("\n=== 选择要下载的文件 ===\n");
    for (size_t i = 0; i < files.size(); i++) {
        const char* tag = files[i].isZip ? " [ZIP]" : "";
        printf("  [%zu] %s%s\n", i, files[i].displayName.c_str(), tag);
    }

    printf("\n输入序号: ");
    int choice = -1;
    if (scanf("%d", &choice) != 1 || choice < 0 || choice >= (int)files.size()) {
        printf("无效序号\n");
        return 1;
    }

    // 拉取文件
    RemoteFile& selected = files[choice];
    char tempDir[MAX_PATH];
    GetTempPathA(sizeof(tempDir), tempDir);
    std::string tempPath = std::string(tempDir) + selected.rawName;

    std::string pullCmd = "pull " + rdir + "/" + selected.rawName + " " + tempPath;
    auto resp = sendPipeCmd(pullCmd);
    if (resp != "ok") {
        printf("%s\n", resp.c_str());
        return 1;
    }

    if (selected.isZip) {
        // 解压
        printf("检测到 .molink.zip 文件，正在解压...\n");
        std::string destDir = ".";
        auto [result, fileCount] = extractZip(tempPath, destDir);
        remove(tempPath.c_str());

        if (result != "ok") {
            printf("解压失败: %s\n", result.c_str());
            printf("原始 ZIP 已保留在: %s\n", tempPath.c_str());
            return 1;
        }
        std::string folderName = selected.displayName.substr(0, selected.displayName.size() - 10);
        printf("已下载并解压到: %s\\%s（共 %d 个文件）\n", destDir.c_str(), folderName.c_str(), fileCount);
    } else {
        // 移动到当前目录
        std::string localPath = ".\\" + selected.displayName;
        MoveFileA(tempPath.c_str(), localPath.c_str());
        printf("已下载到: %s\n", localPath.c_str());
    }

    return 0;
}
```

需要添加的 include：`<sstream>`。

- [ ] **Step 3: 在 main() 中添加 apull 分发**

在 `apush` 分发之后添加：

```cpp
if (strcmp(argv[1], "apull") == 0) return cmdApull(argc, argv);
```

- [ ] **Step 4: 编译验证**

```powershell
cmake --build . --config Release
```

---

### Task 11: 构建并端到端测试

- [ ] **Step 1: 准备测试目录**

```powershell
cd D:\project\MoLink\molink-access-cpp\build
mkdir testdir\sub1 2>nul
mkdir testdir\sub2 2>nul
echo hello world > testdir\file1.txt
echo from sub1 > testdir\sub1\file2.txt
echo from sub2 > testdir\sub2\file3.txt
```

- [ ] **Step 2: 测试 apush 目录（无 .gitignore）**

```powershell
.\molink.exe apush testdir
```

预期: 检测到文件夹 → 压缩 → push 成功 → 显示 `已上传文件夹: testdir -> /sdcard/tmp/b64_xxx`

- [ ] **Step 3: 测试 apull 交互式选择**

```powershell
.\molink.exe apull
```

预期: 显示 b64_ 文件列表 → 输入序号 → pull + 解压 → 本地出现 testdir\file1.txt 等

- [ ] **Step 4: 测试 apush 单文件**

```powershell
echo single test > single.txt
.\molink.exe apush single.txt
```

预期: 直接 push，显示 `已上传: single.txt -> /sdcard/tmp/b64_xxx`

- [ ] **Step 5: 测试 apush .gitignore**

```powershell
echo sub1\ > testdir\.gitignore
.\molink.exe apush testdir --git
```

预期: sub1 被忽略，仅上传 file1.txt 和 sub2\file3.txt

- [ ] **Step 6: 测试 apush 路径不存在**

```powershell
.\molink.exe apush /nonexistent/path
```

预期: `文件/目录不存在: /nonexistent/path`

- [ ] **Step 7: 测试 apull 无 daemon**

```powershell
.\molink.exe stop
.\molink.exe apull
```

预期: `Daemon is not running. Use 'molink start' first.`

- [ ] **Step 8: 测试 forward 在热插拔后恢复**

```powershell
.\molink.exe start
.\molink.exe forward -p 1080 -r 1081
# 拔出设备 → 等待 disconnected
.\molink.exe status
# 预期: disconnected ... forwarding=1080→1081
# 插入设备 → 等待 reconnected
.\molink.exe status
# 预期: connected ... forwarding=1080→1081  connections=0
```

- [ ] **Step 9: 清理**

```powershell
.\molink.exe stop
rmdir /s /q testdir 2>nul
del single.txt 2>nul
```

---

## 验证清单

完成所有任务后：

| # | 验证项 | 命令 | 预期 |
|---|--------|------|------|
| 1 | start 不自动转发 | `molink start && molink status` | `forwarding=off` |
| 2 | forward 默认端口 | `molink forward && molink status` | `forwarding=1080→1081` |
| 3 | forward 自定义端口 | `molink forward -p 2080 -r 1081` | 端口切换成功 |
| 4 | apush 目录 | `molink apush testdir` | 压缩上传成功 |
| 5 | apush 单文件 | `molink apush file.txt` | 直接上传成功 |
| 6 | apush .gitignore | `molink apush testdir --git` | 忽略规则生效 |
| 7 | apull 交互 | `molink apull` | 选择文件下载/解压 |
| 8 | 热插拔后 forward 存活 | 拔插设备 | forward 恢复 |
| 9 | daemon 未运行报错 | 不启动 daemon 执行各命令 | 提示启动 daemon |
