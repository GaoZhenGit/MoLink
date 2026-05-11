# Hotplug Detection & File Transfer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add USB hotplug detection (auto-reconnect on device removal) and file transfer commands (push/pull/ls) to molink-access-cpp.

**Architecture:** Hotplug detection hooks into AdbReader's existing read loop to detect USB errors, signals daemonMain via Windows event handle, and runs a reconnect loop. File transfer adds adb_sync protocol layer, three independent transfer/ modules (FilePush/FilePull/FileList), and extends the daemon pipe command set.

**Tech Stack:** C++17, MinGW-w64, libusb 1.0.28, WinSock2, Windows CNG

**Constraints:** 
- **No git commits** — all changes stay in working tree
- **Sequential development** — Phase 1 (hotplug) must be built and tested before Phase 2 (file transfer)

---

## Phase 1: Hotplug Detection

### Task 1: Add disconnect notification to AdbReader

**Files:**
- Modify: `molink-access-cpp/src/adb/adb_reader.h`
- Modify: `molink-access-cpp/src/adb/adb_reader.cpp`

**Goal:** When `readLoop()` detects a fatal USB error (`NO_DEVICE` or `IO`), call a user-registered callback so upper layers can react. Non-fatal errors (timeout, pipe) are still ignored as they are transient.

- [ ] **Step 1: Add callback type and setter to header**

Add to `adb_reader.h`, after the `#include` block and before the `AdbReader` class:

```cpp
// New: disconnect callback type
using DisconnectCallback = std::function<void()>;
```

Inside `class AdbReader`, add a setter and member:

```cpp
// In the public section, after stop():
void setDisconnectCallback(DisconnectCallback cb) { m_onDisconnect = cb; }

// In the private section:
DisconnectCallback m_onDisconnect;
```

- [ ] **Step 2: Add error detection to readLoop**

In `adb_reader.cpp`, modify `readLoop()`. Find the line:

```cpp
if (!m_transport->recv(msg, data, 100)) continue;
```

Replace with:

```cpp
if (!m_transport->recv(msg, data, 100)) {
    // Distinguish timeout from fatal errors
    // recv() returns false on both timeout and device removal.
    // Try a quick USB read to check if device is still alive.
    if (!m_running) break;
    // If we can't distinguish at this layer, we rely on the read error
    // bubbling up through AdbTransport which wraps UsbDevice.
    // For now, AdbTransport::recv() already prints the error.
    // We check m_running in case we were stopped intentionally.
    continue;
}
```

Actually, the cleanest approach: read `UsbDevice`'s state from `AdbTransport`. But `AdbTransport` doesn't expose it. Instead, check if `recv()` consistently fails for N consecutive attempts (e.g., 30 attempts = 3 seconds at 100ms each). Add to `readLoop()`:

```cpp
void AdbReader::readLoop() {
    int consecutiveErrors = 0;
    const int kMaxErrors = 30; // 3 seconds at 100ms poll

    while (m_running) {
        AdbMessage msg;
        std::vector<uint8_t> data;
        if (!m_transport->recv(msg, data, 100)) {
            consecutiveErrors++;
            if (consecutiveErrors >= kMaxErrors && m_onDisconnect) {
                m_onDisconnect();
                // Wait until stop() is called (transition to DISCONNECTED)
                while (m_running) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                break;
            }
            continue;
        }
        consecutiveErrors = 0; // reset on success

        // ... rest of existing dispatch logic unchanged ...
```

Add `#include <chrono>` to the top of `adb_reader.cpp`.

- [ ] **Step 3: Build and verify compilation**

```powershell
cd molink-access-cpp\build
mingw32-make
```

Expected: compile succeeds, `molink.exe` produced.

---

### Task 2: Add state management to AdbClient

**Files:**
- Modify: `molink-access-cpp/src/adb/adb_client.h`
- Modify: `molink-access-cpp/src/adb/adb_client.cpp`

**Goal:** Add a `DaemonState` enum and `setDisconnectEvent()` so the daemon event loop can be notified. No behavior change to existing API.

- [ ] **Step 1: Add state enum and disconnect event to header**

In `adb_client.h`, before `class AdbClient`, add:

```cpp
enum class DaemonState {
    CONNECTED,
    DISCONNECTED,
    STOPPING
};
```

In `class AdbClient` public section, add:

```cpp
void setDisconnectEvent(HANDLE h) { m_hDisconnectEvent = h; }
DaemonState getState() const { return m_state; }
void setState(DaemonState s) { m_state = s; }
```

In private section, add:

```cpp
HANDLE m_hDisconnectEvent = nullptr;
DaemonState m_state = DaemonState::CONNECTED;
```

- [ ] **Step 2: Wire disconnect callback in connect()**

In `adb_client.cpp`, in `connect()`, after the line `m_reader.start(...)`, add:

```cpp
// Wire disconnect notification
if (m_hDisconnectEvent) {
    m_reader.setDisconnectCallback([this]() {
        SetEvent(m_hDisconnectEvent);
    });
}
```

Also in `connect()`, at the top after successful open, reset state:

```cpp
m_state = DaemonState::CONNECTED;
```

In `disconnect()`, add at the end before the method returns:

```cpp
m_state = DaemonState::DISCONNECTED;
```

- [ ] **Step 3: Build**

```powershell
mingw32-make
```

Expected: compile succeeds.

---

### Task 3: Add pause/resume to Forwarder

**Files:**
- Modify: `molink-access-cpp/src/forward/forwarder.h`
- Modify: `molink-access-cpp/src/forward/forwarder.cpp`

**Goal:** `pause()` stops accepting new connections but keeps the listen socket alive. `resume()` restarts accepting. `m_running` stays true throughout — only `m_paused` changes.

- [ ] **Step 1: Add members to header**

In `forwarder.h`, add to public section:

```cpp
void pause();
void resume();
bool isPaused() const { return m_paused; }
```

In private section, add:

```cpp
std::atomic<bool> m_paused{false};
```

- [ ] **Step 2: Implement pause/resume in cpp**

In `forwarder.cpp`, add:

```cpp
void Forwarder::pause() {
    m_paused = true;
    printf("FWD: Paused (device disconnected), waiting for %d active relays...\n", m_activeCount);
    // Wait for existing relays to drain
    for (int i = 0; i < 50 && m_activeCount > 0; i++) {
        Sleep(100);
    }
    if (m_activeCount > 0) {
        printf("FWD: Warning — %d relays still active after 5s\n", m_activeCount);
    }
}

void Forwarder::resume() {
    m_paused = false;
    printf("FWD: Resumed\n");
}
```

Modify `acceptLoop()` to respect `m_paused`:

```cpp
void Forwarder::acceptLoop() {
    while (m_running) {
        if (m_paused) {
            Sleep(200);
            continue;
        }
        // ... rest unchanged
```

- [ ] **Step 3: Build**

```powershell
mingw32-make
```

Expected: compile succeeds.

---

### Task 4: Update daemonMain with reconnect loop

**Files:**
- Modify: `molink-access-cpp/src/main.cpp` (daemonMain function only)

**Goal:** Replace the simple 2-event wait with a 3-event wait that handles disconnect. Implement the state machine from the spec.

- [ ] **Step 1: Create hDisconnectEvent and wire into AdbClient**

In `daemonMain()`, after `HANDLE hStopEvent = CreateEventW(...)`, add:

```cpp
HANDLE hDisconnectEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
client.setDisconnectEvent(hDisconnectEvent);
```

- [ ] **Step 2: Replace the event loop**

Replace the existing `while (running)` loop and everything from line 292 to line 305 with:

```cpp
    auto transitionToDisconnected = [&]() {
        ResetEvent(hDisconnectEvent);
        client.setState(DaemonState::DISCONNECTED);
        forwarder.pause();
        client.disconnect();
        printf("Daemon: Device disconnected, waiting for reconnect...\n");
    };

    auto transitionToStopping = [&]() {
        client.setState(DaemonState::STOPPING);
        running = false;
        printf("Daemon stopping...\n");
        pipe.stop();
        forwarder.stop();
        client.disconnect();
        CloseHandle(hStopEvent);
        CloseHandle(hDisconnectEvent);
    };

    auto tryReconnect = [&]() -> bool {
        printf("Daemon: Attempting reconnect with serial=%s...\n",
               client.getSerial().empty() ? "(auto)" : client.getSerial().c_str());
        std::string savedSerial = client.getSerial();
        if (client.connect(savedSerial)) {
            forwarder.resume();
            client.setState(DaemonState::CONNECTED);
            printf("Daemon: Reconnected\n");
            return true;
        }
        return false;
    };

    bool running = true;
    while (running) {
        DaemonState state = client.getState();
        DWORD timeout = (state == DaemonState::DISCONNECTED) ? 2000 : INFINITE;

        HANDLE handles[3] = { hStopEvent, hPipeEvent, hDisconnectEvent };
        DWORD ret = WaitForMultipleObjects(3, handles, FALSE, timeout);

        if (ret == WAIT_OBJECT_0) {
            // hStopEvent
            transitionToStopping();
        }
        else if (ret == WAIT_OBJECT_0 + 1) {
            // hPipeEvent
            pipe.processConnection();
        }
        else if (ret == WAIT_OBJECT_0 + 2) {
            // hDisconnectEvent — device removed
            transitionToDisconnected();
        }
        else if (ret == WAIT_TIMEOUT) {
            // DISCONNECTED state: try reconnect every 2s
            if (state == DaemonState::DISCONNECTED) {
                tryReconnect();
            }
        }
    }

    // Final cleanup (STOPPING path)
    pipe.stop();
    forwarder.stop();
    client.disconnect();
    CloseHandle(hStopEvent);
    CloseHandle(hDisconnectEvent);
    removePidFile();
    releaseDaemonLock();
    return 0;
```

**Remove** the lines after the new loop that duplicate cleanup (the old `pipe.stop(); forwarder.stop(); CloseHandle(hStopEvent); removePidFile(); releaseDaemonLock(); return 0;` at lines 300-306). The cleanup is now in `transitionToStopping()` and the final block.

- [ ] **Step 3: Update status command handler**

In the pipe handler lambda (around line 264), update the "status" response to include the state:

```cpp
if (cmd == "status") {
    char buf[256];
    const char* stateStr = (client.getState() == DaemonState::CONNECTED) 
        ? "connected" : "disconnected";
    snprintf(buf, sizeof(buf),
             "%s  serial=%s  port=%u  connections=%d",
             stateStr,
             client.getSerial().c_str(), localPort,
             forwarder.getConnectionCount());
    return buf;
}
```

- [ ] **Step 4: Build**

```powershell
mingw32-make
```

Expected: compile succeeds.

---

### Task 5: Update foregroundMode with same logic

**Files:**
- Modify: `molink-access-cpp/src/main.cpp` (foregroundMode function only)

**Goal:** foreground mode also handles disconnect/reconnect. Uses a polling pattern since there's no daemon event loop.

- [ ] **Step 1: Replace the Sleep loop in foregroundMode**

Replace lines 399-401:

```cpp
    while (forwarder.isRunning()) {
        Sleep(500);
    }
```

With:

```cpp
    HANDLE hDisconnectEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    client.setDisconnectEvent(hDisconnectEvent);

    while (forwarder.isRunning()) {
        DWORD ret = WaitForSingleObject(hDisconnectEvent, 500);
        if (ret == WAIT_OBJECT_0) {
            // Device disconnected
            ResetEvent(hDisconnectEvent);
            printf("\n[Device disconnected, reconnecting...]\n");
            forwarder.pause();
            client.disconnect();

            bool reconnected = false;
            while (!reconnected && forwarder.isRunning()) {
                Sleep(2000);
                printf("[Reconnect attempt...]\n");
                if (client.connect(serial)) {
                    forwarder.resume();
                    reconnected = true;
                    printf("[Reconnected]\n");
                }
            }
            if (!reconnected) break; // Ctrl+C during reconnect
        }
    }
    CloseHandle(hDisconnectEvent);
```

- [ ] **Step 2: Build**

```powershell
mingw32-make
```

Expected: compile succeeds.

---

### Task 6: Build and manual test (hotplug)

**Goal:** Verify everything compiles and the hotplug behavior works.

- [ ] **Step 1: Full build**

```powershell
cd molink-access-cpp\build
mingw32-make
```

Expected: compile succeeds, no warnings.

- [ ] **Step 2: Test — normal start**

```powershell
.\molink.exe run -p 1080
```

Expected: `=== MoLink Access ===`, `ADB: Connected`, `FWD: Listening on 127.0.0.1:1080`

- [ ] **Step 3: Test — status command (new terminal)**

```powershell
.\molink.exe status
# Expect: "connected  serial=...  port=1080  connections=0"
```

- [ ] **Step 4: Test — disconnect device**

Physically unplug the USB cable.

Expected in `molink run` output:
```
USB: bulkRead error: LIBUSB_ERROR_NO_DEVICE (repeated)
FWD: Paused (device disconnected)...
Daemon: Device disconnected, waiting for reconnect...
```

Expected from `status` in another terminal:
```
disconnected  serial=...  port=1080  connections=0
```

- [ ] **Step 5: Test — reconnect device**

Plug the USB cable back in.

Expected in `molink run` output:
```
Daemon: Attempting reconnect...
ADB: Handshake attempt 1...
ADB: Connected
FWD: Resumed
Daemon: Reconnected
```

Expected from `status`: `connected serial=... port=1080 connections=0`

- [ ] **Step 6: Test — stop while disconnected**

Unplug device → wait for disconnected → `molink stop`

Expected: daemon exits cleanly, no hang.

- [ ] **Step 7: Test — rapid plug/unplug (5 times)**

Expected: no crash, no deadlock, status toggles correctly each time.

**Phase 1 gate: All 7 tests must pass before proceeding to Phase 2.**

---

## Phase 2: File Transfer

### Task 7: Create project scaffolding

**Files:**
- Create: `molink-access-cpp/src/adb/adb_sync.h`
- Create: `molink-access-cpp/src/adb/adb_sync.cpp`
- Create: `molink-access-cpp/src/adb/adb_shell.h`
- Create: `molink-access-cpp/src/adb/adb_shell.cpp`
- Create: `molink-access-cpp/src/transfer/file_push.h`
- Create: `molink-access-cpp/src/transfer/file_push.cpp`
- Create: `molink-access-cpp/src/transfer/file_pull.h`
- Create: `molink-access-cpp/src/transfer/file_pull.cpp`
- Create: `molink-access-cpp/src/transfer/file_list.h`
- Create: `molink-access-cpp/src/transfer/file_list.cpp`
- Modify: `molink-access-cpp/CMakeLists.txt`

- [ ] **Step 1: Create directories**

```powershell
mkdir molink-access-cpp\src\transfer
```

- [ ] **Step 2: Create adb_sync.h**

```cpp
#ifndef ADB_SYNC_H
#define ADB_SYNC_H

#include <cstdint>
#include <string>
#include <vector>

// ADB Sync protocol message IDs
constexpr uint32_t SYNC_SEND = 0x444e4553;  // "SEND"
constexpr uint32_t SYNC_RECV = 0x56434552;  // "RECV"
constexpr uint32_t SYNC_DATA = 0x41544144;  // "DATA"
constexpr uint32_t SYNC_DONE = 0x454e4f44;  // "DONE"
constexpr uint32_t SYNC_OKAY = 0x59414b4f;  // "OKAY"
constexpr uint32_t SYNC_FAIL = 0x4c494146;  // "FAIL"
constexpr uint32_t SYNC_QUIT = 0x54495551;  // "QUIT"

#pragma pack(push, 1)
struct SyncMsg {
    uint32_t id;
    uint32_t data_length;
};
#pragma pack(pop)

class AdbClient;
using ChannelPtr = std::shared_ptr<struct Channel>;

// Low-level sync protocol helpers (used by FilePush / FilePull)
bool syncSend(AdbClient& client, ChannelPtr ch,
              const std::string& remotePath, uint32_t mode);
bool syncRecv(AdbClient& client, ChannelPtr ch,
              const std::string& remotePath);
bool syncQuit(AdbClient& client, ChannelPtr ch);

// Read a sync response from the channel's dataQueue.
// Returns true and fills msg + payload on success.
// timeoutMs: max wait per read cycle (total timeout handled by caller).
bool syncReadResponse(ChannelPtr ch, SyncMsg& msg,
                      std::vector<uint8_t>& payload, int timeoutMs);

#endif
```

- [ ] **Step 3: Create adb_sync.cpp (stub)**

```cpp
#include "adb_sync.h"
#include "adb_reader.h"
#include "adb_client.h"
#include <cstdio>
#include <cstring>

bool syncSend(AdbClient& client, ChannelPtr ch,
              const std::string& remotePath, uint32_t mode) {
    // TODO: implement in Task 8
    (void)client; (void)ch; (void)remotePath; (void)mode;
    return false;
}

bool syncRecv(AdbClient& client, ChannelPtr ch,
              const std::string& remotePath) {
    // TODO: implement in Task 8
    (void)client; (void)ch; (void)remotePath;
    return false;
}

bool syncQuit(AdbClient& client, ChannelPtr ch) {
    // TODO: implement in Task 8
    (void)client; (void)ch;
    return false;
}

bool syncReadResponse(ChannelPtr ch, SyncMsg& msg,
                      std::vector<uint8_t>& payload, int timeoutMs) {
    // TODO: implement in Task 8
    (void)ch; (void)msg; (void)payload; (void)timeoutMs;
    return false;
}
```

- [ ] **Step 4: Create adb_shell.h**

```cpp
#ifndef ADB_SHELL_H
#define ADB_SHELL_H

#include <string>
#include <memory>

class AdbClient;
struct Channel;
using ChannelPtr = std::shared_ptr<Channel>;

// Execute a shell command on the device via "shell:<cmd>" channel.
// Returns the stdout output as a string, or empty on failure.
std::string shellCommand(AdbClient& client, const std::string& command);

#endif
```

- [ ] **Step 5: Create adb_shell.cpp (stub)**

```cpp
#include "adb_shell.h"
#include "adb_client.h"
#include "adb_reader.h"
#include <cstdio>

std::string shellCommand(AdbClient& client, const std::string& command) {
    // TODO: implement in Task 9
    (void)client; (void)command;
    return "";
}
```

- [ ] **Step 6: Create transfer/file_push.h**

```cpp
#ifndef FILE_PUSH_H
#define FILE_PUSH_H

#include <string>

class AdbClient;

// Push a local file to the device.
// Returns "ok" on success, or "fail: <reason>" on error.
std::string pushFile(AdbClient& client,
                     const std::string& localPath,
                     const std::string& remotePath);

#endif
```

- [ ] **Step 7: Create transfer/file_push.cpp (stub)**

```cpp
#include "file_push.h"
#include "../adb/adb_client.h"
#include <cstdio>

std::string pushFile(AdbClient& client,
                     const std::string& localPath,
                     const std::string& remotePath) {
    // TODO: implement in Task 10
    (void)client; (void)localPath; (void)remotePath;
    return "fail: not implemented";
}
```

- [ ] **Step 8: Create transfer/file_pull.h**

```cpp
#ifndef FILE_PULL_H
#define FILE_PULL_H

#include <string>

class AdbClient;

// Pull a file from the device to the local filesystem.
// Returns "ok" on success, or "fail: <reason>" on error.
std::string pullFile(AdbClient& client,
                     const std::string& remotePath,
                     const std::string& localPath);

#endif
```

- [ ] **Step 9: Create transfer/file_pull.cpp (stub)**

```cpp
#include "file_pull.h"
#include "../adb/adb_client.h"
#include <cstdio>

std::string pullFile(AdbClient& client,
                     const std::string& remotePath,
                     const std::string& localPath) {
    // TODO: implement in Task 11
    (void)client; (void)remotePath; (void)localPath;
    return "fail: not implemented";
}
```

- [ ] **Step 10: Create transfer/file_list.h**

```cpp
#ifndef FILE_LIST_H
#define FILE_LIST_H

#include <string>

class AdbClient;

// List a directory on the device.
// Returns directory listing as string, or "fail: <reason>".
std::string listFiles(AdbClient& client, const std::string& remotePath);

#endif
```

- [ ] **Step 11: Create transfer/file_list.cpp (stub)**

```cpp
#include "file_list.h"
#include "../adb/adb_client.h"
#include <cstdio>

std::string listFiles(AdbClient& client, const std::string& remotePath) {
    // TODO: implement in Task 12
    (void)client; (void)remotePath;
    return "fail: not implemented";
}
```

- [ ] **Step 12: Update CMakeLists.txt**

Add the new source files to the `add_executable` block:

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
)
```

- [ ] **Step 13: Build to verify scaffolding compiles**

```powershell
cd molink-access-cpp\build
cmake .. -G "MinGW Makefiles"
mingw32-make
```

Expected: compile succeeds (with stub implementations).

---

### Task 8: Implement adb_sync protocol

**Files:**
- Modify: `molink-access-cpp/src/adb/adb_sync.cpp`

**Goal:** Full implementation of sync protocol helpers. These build sync messages, send them via writeChannel, and read responses from dataQueue.

- [ ] **Step 1: Replace adb_sync.cpp with full implementation**

```cpp
#include "adb_sync.h"
#include "adb_reader.h"
#include "adb_client.h"
#include <cstdio>
#include <cstring>
#include <chrono>

static bool sendSyncMsg(AdbClient& client, ChannelPtr ch,
                         uint32_t id, const void* data, uint32_t len) {
    // Build sync header + payload
    SyncMsg hdr;
    hdr.id = id;
    hdr.data_length = len;

    // Combine header + data into one buffer
    std::vector<uint8_t> buf(sizeof(hdr) + len);
    memcpy(buf.data(), &hdr, sizeof(hdr));
    if (data && len > 0) {
        memcpy(buf.data() + sizeof(hdr), data, len);
    }
    return client.writeChannel(ch, buf.data(), (uint32_t)buf.size());
}

bool syncReadResponse(ChannelPtr ch, SyncMsg& msg,
                      std::vector<uint8_t>& payload, int timeoutMs) {
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeoutMs);

    while (std::chrono::steady_clock::now() < deadline) {
        std::unique_lock<std::mutex> lock(ch->mtx);
        if (!ch->dataQueue.empty()) {
            auto raw = std::move(ch->dataQueue.front());
            ch->dataQueue.pop();
            lock.unlock();

            if (raw.size() < sizeof(SyncMsg)) {
                printf("SYNC: Short message (%zu bytes)\n", raw.size());
                return false;
            }
            memcpy(&msg, raw.data(), sizeof(SyncMsg));
            payload.assign(raw.begin() + sizeof(SyncMsg), raw.end());
            return true;
        }
        if (ch->closed) {
            printf("SYNC: Channel closed by device\n");
            return false;
        }
        ch->cv.wait_for(lock, std::chrono::milliseconds(100));
    }
    printf("SYNC: Read timeout (%d ms)\n", timeoutMs);
    return false;
}

bool syncSend(AdbClient& client, ChannelPtr ch,
              const std::string& remotePath, uint32_t mode) {
    // SEND payload: "<path>,<mode>"
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s,%o", remotePath.c_str(), mode);
    uint32_t len = (uint32_t)strlen(buf);

    if (!sendSyncMsg(client, ch, SYNC_SEND, buf, len)) {
        printf("SYNC: Failed to send SEND\n");
        return false;
    }

    // Wait for OKAY
    SyncMsg resp;
    std::vector<uint8_t> payload;
    if (!syncReadResponse(ch, resp, payload, 60000)) return false;
    if (resp.id == SYNC_FAIL) {
        std::string err(payload.begin(), payload.end());
        printf("SYNC: SEND failed: %s\n", err.c_str());
        return false;
    }
    if (resp.id != SYNC_OKAY) {
        printf("SYNC: Expected OKAY, got 0x%08X\n", resp.id);
        return false;
    }
    return true;
}

bool syncRecv(AdbClient& client, ChannelPtr ch,
              const std::string& remotePath) {
    uint32_t len = (uint32_t)remotePath.size();
    if (!sendSyncMsg(client, ch, SYNC_RECV, remotePath.data(), len)) {
        printf("SYNC: Failed to send RECV\n");
        return false;
    }
    // Response will be DATA or FAIL — handled by caller
    return true;
}

bool syncQuit(AdbClient& client, ChannelPtr ch) {
    return sendSyncMsg(client, ch, SYNC_QUIT, nullptr, 0);
}
```

- [ ] **Step 2: Build**

```powershell
mingw32-make
```

Expected: compile succeeds.

---

### Task 9: Implement adb_shell helper

**Files:**
- Modify: `molink-access-cpp/src/adb/adb_shell.cpp`

**Goal:** Open a `shell:<cmd>` channel, read all output until channel closes, return as string.

- [ ] **Step 1: Replace adb_shell.cpp**

```cpp
#include "adb_shell.h"
#include "adb_client.h"
#include "adb_reader.h"
#include <cstdio>

std::string shellCommand(AdbClient& client, const std::string& command) {
    std::string dest = "shell:" + command;
    auto ch = client.openChannel(dest);
    if (!ch) {
        printf("SHELL: Failed to open channel: %s\n", dest.c_str());
        return "fail: Could not open shell channel";
    }

    std::string output;
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::seconds(30);

    while (std::chrono::steady_clock::now() < deadline) {
        std::unique_lock<std::mutex> lock(ch->mtx);
        if (!ch->dataQueue.empty()) {
            auto data = std::move(ch->dataQueue.front());
            ch->dataQueue.pop();
            lock.unlock();
            output.append((const char*)data.data(), data.size());
            continue;
        }
        if (ch->closed) {
            break;
        }
        ch->cv.wait_for(lock, std::chrono::milliseconds(100));
    }

    client.closeChannel(ch);
    return output;
}
```

Add to the top of adb_shell.cpp:

```cpp
#include <chrono>
```

- [ ] **Step 2: Build**

```powershell
mingw32-make
```

Expected: compile succeeds.

---

### Task 10: Implement FilePush

**Files:**
- Modify: `molink-access-cpp/src/transfer/file_push.cpp`

**Goal:** Read local file, push to device via sync protocol.

- [ ] **Step 1: Replace file_push.cpp**

```cpp
#include "file_push.h"
#include "../adb/adb_client.h"
#include "../adb/adb_reader.h"
#include "../adb/adb_sync.h"
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

std::string pushFile(AdbClient& client,
                     const std::string& localPath,
                     const std::string& remotePath) {
    // Open local file
    FILE* f = fopen(localPath.c_str(), "rb");
    if (!f) {
        char buf[512];
        snprintf(buf, sizeof(buf), "fail: Local file not found: %s",
                 localPath.c_str());
        return buf;
    }

    // Get file size and mtime
    struct stat st;
    if (stat(localPath.c_str(), &st) != 0) {
        fclose(f);
        return "fail: Cannot stat local file";
    }

    // Open sync channel
    auto ch = client.openChannel("sync:");
    if (!ch) {
        fclose(f);
        return "fail: Cannot open sync channel";
    }

    // Send SEND with path
    if (!syncSend(client, ch, remotePath, 0644)) {
        fclose(f);
        client.closeChannel(ch);
        return "fail: Device rejected file open";
    }

    // Send file data in 64KB chunks
    constexpr size_t kChunkSize = 65536;
    std::vector<uint8_t> chunk(kChunkSize + sizeof(SyncMsg));
    int64_t totalSent = 0;

    while (!feof(f)) {
        size_t n = fread(chunk.data() + sizeof(SyncMsg), 1, kChunkSize, f);
        if (n == 0 && ferror(f)) {
            fclose(f);
            client.closeChannel(ch);
            return "fail: Error reading local file";
        }
        if (n == 0) break;

        // Build DATA message
        SyncMsg* hdr = (SyncMsg*)chunk.data();
        hdr->id = SYNC_DATA;
        hdr->data_length = (uint32_t)n;

        if (!client.writeChannel(ch, chunk.data(),
                                  (uint32_t)(sizeof(SyncMsg) + n))) {
            fclose(f);
            client.closeChannel(ch);
            return "fail: USB write error during push";
        }
        totalSent += n;
        printf("PUSH: Sent %lld bytes...\r", totalSent);
    }
    printf("\nPUSH: Total %lld bytes sent\n", totalSent);
    fclose(f);

    // Send DONE with mtime
    uint32_t mtime = (uint32_t)st.st_mtime;
    SyncMsg doneHdr;
    doneHdr.id = SYNC_DONE;
    doneHdr.data_length = 4;
    std::vector<uint8_t> doneBuf(sizeof(SyncMsg) + 4);
    memcpy(doneBuf.data(), &doneHdr, sizeof(SyncMsg));
    memcpy(doneBuf.data() + sizeof(SyncMsg), &mtime, 4);

    if (!client.writeChannel(ch, doneBuf.data(), (uint32_t)doneBuf.size())) {
        client.closeChannel(ch);
        return "fail: USB write error sending DONE";
    }

    // Wait for final OKAY or FAIL
    SyncMsg resp;
    std::vector<uint8_t> payload;
    if (!syncReadResponse(ch, resp, payload, 60000)) {
        client.closeChannel(ch);
        return "fail: Sync timeout waiting for DONE response";
    }

    syncQuit(client, ch);
    client.closeChannel(ch);

    if (resp.id == SYNC_FAIL) {
        std::string err(payload.begin(), payload.end());
        char buf[512];
        snprintf(buf, sizeof(buf), "fail: %s", err.c_str());
        return buf;
    }
    if (resp.id == SYNC_OKAY) {
        return "ok";
    }
    return "fail: Unexpected sync response";
}
```

- [ ] **Step 2: Build**

```powershell
mingw32-make
```

Expected: compile succeeds.

---

### Task 11: Implement FilePull

**Files:**
- Modify: `molink-access-cpp/src/transfer/file_pull.cpp`

- [ ] **Step 1: Replace file_pull.cpp**

```cpp
#include "file_pull.h"
#include "../adb/adb_client.h"
#include "../adb/adb_reader.h"
#include "../adb/adb_sync.h"
#include <cstdio>
#include <cstring>

std::string pullFile(AdbClient& client,
                     const std::string& remotePath,
                     const std::string& localPath) {
    auto ch = client.openChannel("sync:");
    if (!ch) {
        return "fail: Cannot open sync channel";
    }

    // Send RECV
    if (!syncRecv(client, ch, remotePath)) {
        client.closeChannel(ch);
        return "fail: Failed to send RECV";
    }

    // Wait for first response (DATA or FAIL)
    SyncMsg msg;
    std::vector<uint8_t> payload;
    if (!syncReadResponse(ch, msg, payload, 60000)) {
        client.closeChannel(ch);
        return "fail: Sync timeout waiting for file data";
    }

    if (msg.id == SYNC_FAIL) {
        std::string err(payload.begin(), payload.end());
        client.closeChannel(ch);
        char buf[512];
        snprintf(buf, sizeof(buf), "fail: %s", err.c_str());
        return buf;
    }

    // Open local file for writing
    FILE* f = fopen(localPath.c_str(), "wb");
    if (!f) {
        client.closeChannel(ch);
        return "fail: Cannot create local file";
    }

    int64_t totalReceived = 0;
    bool done = false;
    std::string error;

    while (!done) {
        if (msg.id == SYNC_DATA) {
            fwrite(payload.data(), 1, payload.size(), f);
            totalReceived += payload.size();
            printf("PULL: Received %lld bytes...\r", totalReceived);
        } else if (msg.id == SYNC_DONE) {
            done = true;
            printf("\nPULL: Total %lld bytes received\n", totalReceived);
            break;
        } else if (msg.id == SYNC_FAIL) {
            std::string err(payload.begin(), payload.end());
            char buf[512];
            snprintf(buf, sizeof(buf), "fail: %s", err.c_str());
            error = buf;
            break;
        }
        // else: unexpected, try to read next

        if (!done) {
            if (!syncReadResponse(ch, msg, payload, 60000)) {
                error = "fail: Sync timeout during pull";
                break;
            }
        }
    }

    fclose(f);

    if (!error.empty()) {
        client.closeChannel(ch);
        return error;
    }

    // Send final OKAY (acknowledge DONE)
    SyncMsg okayHdr;
    okayHdr.id = SYNC_OKAY;
    okayHdr.data_length = 0;
    client.writeChannel(ch, &okayHdr, sizeof(SyncMsg));

    syncQuit(client, ch);
    client.closeChannel(ch);
    return "ok";
}
```

- [ ] **Step 2: Build**

```powershell
mingw32-make
```

Expected: compile succeeds.

---

### Task 12: Implement FileList

**Files:**
- Modify: `molink-access-cpp/src/transfer/file_list.cpp`

- [ ] **Step 1: Replace file_list.cpp**

```cpp
#include "file_list.h"
#include "../adb/adb_client.h"
#include "../adb/adb_shell.h"
#include <cstdio>

std::string listFiles(AdbClient& client, const std::string& remotePath) {
    std::string path = remotePath.empty() ? "/sdcard/" : remotePath;
    std::string cmd = "ls -la " + path;
    std::string output = shellCommand(client, cmd);

    if (output.empty()) {
        return "fail: No output from shell ls command";
    }
    return output;
}
```

- [ ] **Step 2: Build**

```powershell
mingw32-make
```

Expected: compile succeeds.

---

### Task 13: Add pipe commands to daemon

**Files:**
- Modify: `molink-access-cpp/src/main.cpp` (daemonMain pipe handler only)

**Goal:** Extend the daemon's pipe command handler to support `push`, `pull`, and `ls` commands.

- [ ] **Step 1: Add includes at top of main.cpp**

```cpp
#include "transfer/file_push.h"
#include "transfer/file_pull.h"
#include "transfer/file_list.h"
```

- [ ] **Step 2: Extend pipe handler lambda**

In `daemonMain()`, find the pipe handler lambda (around line 264). Replace the handler block with:

```cpp
pipe.setHandler([&](const std::string& cmd) -> std::string {
    if (cmd == "stop") {
        SetEvent(hStopEvent);
        return "ok";
    }
    if (cmd == "status") {
        char buf[256];
        const char* stateStr = (client.getState() == DaemonState::CONNECTED) 
            ? "connected" : "disconnected";
        snprintf(buf, sizeof(buf),
                 "%s  serial=%s  port=%u  connections=%d",
                 stateStr,
                 client.getSerial().c_str(), localPort,
                 forwarder.getConnectionCount());
        return buf;
    }
    // File transfer commands
    if (cmd.rfind("push ", 0) == 0) {
        // Format: "push <local> <remote>"
        std::string args = cmd.substr(5);
        size_t space = args.find(' ');
        if (space == std::string::npos) return "fail: Usage: push <local> <remote>";
        std::string local = args.substr(0, space);
        std::string remote = args.substr(space + 1);
        return pushFile(client, local, remote);
    }
    if (cmd.rfind("pull ", 0) == 0) {
        // Format: "pull <remote> <local>"
        std::string args = cmd.substr(5);
        size_t space = args.find(' ');
        if (space == std::string::npos) return "fail: Usage: pull <remote> <local>";
        std::string remote = args.substr(0, space);
        std::string local = args.substr(space + 1);
        return pullFile(client, remote, local);
    }
    if (cmd.rfind("ls", 0) == 0) {
        // Format: "ls <path>" or just "ls"
        std::string path;
        if (cmd.size() > 2) {
            path = cmd.substr(3);
        }
        return listFiles(client, path);
    }
    return "unknown";
});
```

- [ ] **Step 3: Build**

```powershell
mingw32-make
```

Expected: compile succeeds.

---

### Task 14: Add CLI commands

**Files:**
- Modify: `molink-access-cpp/src/main.cpp` (main function + new command functions)

**Goal:** Add `push`, `pull`, `ls` as top-level CLI commands that route through the daemon pipe.

- [ ] **Step 1: Add command dispatch in main()**

In `main()`, after the existing command dispatch lines (`devices`/`status`/`stop`), add:

```cpp
if (strcmp(argv[1], "push") == 0) {
    if (argc < 4) {
        printf("Usage: molink push <local_file> <remote_path>\n");
        return 1;
    }
    std::string cmd = std::string("push ") + argv[2] + " " + argv[3];
    auto resp = sendPipeCmd(cmd);
    if (resp.empty()) {
        printf("Daemon is not running. Use 'molink start' first.\n");
        return 1;
    }
    printf("%s\n", resp.c_str());
    return (resp == "ok") ? 0 : 1;
}

if (strcmp(argv[1], "pull") == 0) {
    if (argc < 4) {
        printf("Usage: molink pull <remote_path> <local_file>\n");
        return 1;
    }
    std::string cmd = std::string("pull ") + argv[2] + " " + argv[3];
    auto resp = sendPipeCmd(cmd);
    if (resp.empty()) {
        printf("Daemon is not running. Use 'molink start' first.\n");
        return 1;
    }
    printf("%s\n", resp.c_str());
    return (resp == "ok") ? 0 : 1;
}

if (strcmp(argv[1], "ls") == 0) {
    std::string path = (argc >= 3) ? argv[2] : "/sdcard/";
    std::string cmd = "ls " + path;
    auto resp = sendPipeCmd(cmd);
    if (resp.empty()) {
        printf("Daemon is not running. Use 'molink start' first.\n");
        return 1;
    }
    printf("%s", resp.c_str()); // ls output already has newlines
    return 0;
}
```

Insert these after the `stop` command line and **before** the option-parsing block (before the `isRun`/`isStart`/`isDaemon` section).

- [ ] **Step 2: Update help text**

In `printUsage()`, add the new commands:

```cpp
static void printUsage() {
    printf("MoLink Access - ADB USB Proxy\n\n"
           "Usage:\n"
           "  molink run      [options]         Run in foreground\n"
           "  molink start    [options]         Start daemon in background\n"
           "  molink stop                       Stop running daemon\n"
           "  molink devices                    List ADB devices\n"
           "  molink status                     Show daemon status\n"
           "  molink push     <local> <remote>  Upload file to device\n"
           "  molink pull     <remote> <local>  Download file from device\n"
           "  molink ls       [remote_path]     List device directory\n"
           "  molink --help                     Show this help\n\n"
           "Options:\n"
           "  --port, -p <port>                 Local TCP port (default: 1080)\n"
           "  --rport, -r <port>                Remote device port (default: 1081)\n"
           "  --serial, -s <sn>                 Device serial number\n");
}
```

- [ ] **Step 3: Build**

```powershell
mingw32-make
```

Expected: compile succeeds.

---

### Task 15: Build and manual test (file transfer)

- [ ] **Step 1: Full build**

```powershell
cd molink-access-cpp\build
mingw32-make
```

Expected: compile succeeds.

- [ ] **Step 2: Start daemon**

```powershell
.\molink.exe start -p 1080
```

Expected: "Daemon ready" or "connected serial=..."

- [ ] **Step 3: Test ls**

```powershell
.\molink.exe ls /sdcard/
```

Expected: directory listing similar to `adb shell ls -la /sdcard/`

- [ ] **Step 4: Test push (small file)**

Create a test file:
```powershell
echo "hello molink" > test_push.txt
.\molink.exe push test_push.txt /sdcard/test_push.txt
```

Expected: `ok`

- [ ] **Step 5: Verify push on device**

```powershell
.\molink.exe ls /sdcard/ | findstr test_push
```

Expected: test_push.txt appears in listing.

- [ ] **Step 6: Test pull**

```powershell
.\molink.exe pull /sdcard/test_push.txt test_pull.txt
type test_pull.txt
```

Expected: `ok` + file contains "hello molink".

- [ ] **Step 7: Test push (larger file, 5MB)**

```powershell
powershell -Command "$f=New-Object System.Random; $b=New-Object byte[] 5242880; $f.NextBytes($b); [IO.File]::WriteAllBytes('test_5mb.bin', $b)"
.\molink.exe push test_5mb.bin /sdcard/test_5mb.bin
```

Expected: `ok` within a few seconds.

- [ ] **Step 8: Test error — push nonexistent file**

```powershell
.\molink.exe push nonexistent.txt /sdcard/nonexistent.txt
```

Expected: `fail: Local file not found: nonexistent.txt`

- [ ] **Step 9: Test error — pull nonexistent file**

```powershell
.\molink.exe pull /sdcard/nonexistent_file_xyz.txt test.txt
```

Expected: `fail: ...` (device error message)

- [ ] **Step 10: Cleanup test files**

```powershell
del test_push.txt test_pull.txt test_5mb.bin 2>$null
```
