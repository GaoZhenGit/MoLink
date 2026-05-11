#include <winsock2.h>
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <shlobj.h>
#include <io.h>
#include "adb/adb_client.h"
#include "forward/forwarder.h"
#include "cli/named_pipe.h"
#include "transfer/file_push.h"
#include "transfer/file_pull.h"
#include "transfer/file_list.h"

// ---- 全局（Ctrl+C 用） ----
static Forwarder* g_forwarder = nullptr;
static BOOL WINAPI ctrlHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
        printf("\nShutting down...\n");
        if (g_forwarder) g_forwarder->stop();
        return TRUE;
    }
    return FALSE;
}

// ---- PID 文件 ----
static std::string getPidPath() {
    char dir[MAX_PATH];
    GetModuleFileNameA(nullptr, dir, sizeof(dir));
    char* lastSlash = strrchr(dir, '\\');
    if (!lastSlash) lastSlash = strrchr(dir, '/');
    if (lastSlash) *(lastSlash + 1) = '\0';
    return std::string(dir) + "molinkd.pid";
}

static void writePidFile() {
    auto path = getPidPath();
    FILE* f = fopen(path.c_str(), "w");
    if (f) {
        fprintf(f, "%lu", GetCurrentProcessId());
        fclose(f);
    }
}

static DWORD readPidFile() {
    auto path = getPidPath();
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return 0;
    DWORD pid = 0;
    fscanf(f, "%lu", &pid);
    fclose(f);
    return pid;
}

static void removePidFile() {
    auto path = getPidPath();
    remove(path.c_str());
}

// ---- 单实例 ----
static HANDLE g_daemonMutex = nullptr;

static bool tryAcquireDaemonLock() {
    g_daemonMutex = CreateMutexW(nullptr, TRUE, L"Global\\MoLinkDaemon");
    return (GetLastError() != ERROR_ALREADY_EXISTS);
}

static void releaseDaemonLock() {
    if (g_daemonMutex) {
        ReleaseMutex(g_daemonMutex);
        CloseHandle(g_daemonMutex);
        g_daemonMutex = nullptr;
    }
}

// ---- Named Pipe 客户端 ----
static std::string sendPipeCmd(const std::string& cmd) {
    HANDLE pipe = CreateFileA("\\\\.\\pipe\\molink",
        GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) return {};

    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);

    std::string msg = cmd + "\n";
    DWORD written = 0;
    WriteFile(pipe, msg.c_str(), (DWORD)msg.size(), &written, nullptr);

    char buf[4096] = {};
    DWORD read = 0;
    ReadFile(pipe, buf, sizeof(buf) - 1, &read, nullptr);
    CloseHandle(pipe);

    std::string result(buf, read);
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
        result.pop_back();
    return result;
}

// ---- 命令实现 ----

static int cmdDevices() {
    // 1. 密钥文件
    char appdata[MAX_PATH] = {};
    if (SHGetFolderPathA(nullptr, CSIDL_PROFILE, nullptr, 0, appdata) == S_OK) {
        printf("Keys: %s\\.android\n", appdata);
        auto exists = [](const std::string& p) {
            return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES;
        };
        std::string base = std::string(appdata) + "\\.android\\";
        printf("  molink_key.bin : %s\n", exists(base + "molink_key.bin") ? "found" : "missing");
        printf("  adbkey         : %s\n", exists(base + "adbkey") ? "found" : "missing");
        printf("  adbkey.pub     : %s\n", exists(base + "adbkey.pub") ? "found" : "missing");
    }

    // 2. 静默探测（抑制 USB/ADB/RSA 内部 debug 输出）
    fflush(stdout);
    int saved = _dup(1);
    freopen("NUL", "w", stdout);

    auto devices = UsbDevice::discover();

    AdbRsa rsa;
    bool keyReady = false;
    if (appdata[0]) {
        std::string kp = std::string(appdata) + "\\.android\\molink_key.bin";
        if (!rsa.loadKey(kp)) {
            std::string ak = std::string(appdata) + "\\.android\\adbkey";
            if (rsa.loadPkcs8(ak)) keyReady = true;
        } else keyReady = true;
    }

    struct DevResult { std::string serial; bool authorized; };
    std::vector<DevResult> results;

    for (auto& dev : devices) {
        DevResult r;
        if (!dev.open()) { results.push_back(r); continue; }
        dev.clearHalt(dev.getReadEndpoint());
        dev.clearHalt(dev.getWriteEndpoint());
        dev.drainRead();
        r.serial = dev.getSerial();

        // 快速握手（5s 超时 → 未授权）
        r.authorized = false;
        if (keyReady) {
            AdbTransport tr(dev);
            uint32_t maxdata = MAX_PAYLOAD_V2;
            tr.send(A_CNXN, A_VERSION, maxdata, "host::", 5);
            AdbMessage msg;
            std::vector<uint8_t> data;
            // 单轮：等 5s，期望直接 A_CNXN 或签名后 A_CNXN
            if (tr.recv(msg, data, 5000)) {
                if (msg.command == A_CNXN) {
                    r.authorized = true;
                } else if (msg.command == A_AUTH && msg.arg0 == AUTH_TOKEN) {
                    auto sig = rsa.signToken(data.data(), data.size());
                    if (!sig.empty()) {
                        tr.send(A_AUTH, AUTH_SIGNATURE, 0, sig.data(), (uint32_t)sig.size());
                        if (tr.recv(msg, data, 2000)) {
                            if (msg.command == A_CNXN) r.authorized = true;
                        }
                    }
                }
            }
        }

        dev.close();
        results.push_back(r);
    }

    fflush(stdout);
    _dup2(saved, 1);
    _close(saved);

    // 3. 输出
    if (devices.empty()) { printf("No ADB devices found.\n"); return 1; }
    printf("\n%-4s %-22s %s\n", "#", "SERIAL", "AUTH");
    printf("%-4s %-22s %s\n", "---", "----------------------", "----");
    for (size_t i = 0; i < results.size(); i++) {
        printf("%-4zu %-22s %s\n", i,
               results[i].serial.empty() ? "-" : results[i].serial.c_str(),
               results[i].serial.empty() ? "-" : (results[i].authorized ? "yes" : "no"));
    }
    return 0;
}

static int cmdStatus() {
    auto resp = sendPipeCmd("status");
    if (resp.empty()) {
        printf("Daemon is not running.\n");
        return 1;
    }
    printf("%s\n", resp.c_str());
    return 0;
}

static int cmdStop() {
    auto resp = sendPipeCmd("stop");
    if (resp.empty()) {
        printf("Daemon is not running.\n");
        return 1;
    }
    if (resp == "ok") {
        printf("Daemon stopped.\n");
        return 0;
    }

    // 超时强杀（但 sendPipeCmd 已经等了 ReadFile，这里补充 TerminateProcess）
    printf("Daemon unresponsive, force killing...\n");
    DWORD pid = readPidFile();
    if (pid) {
        HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
        if (hProc) {
            TerminateProcess(hProc, 1);
            CloseHandle(hProc);
            printf("Daemon killed (pid=%lu).\n", pid);
            removePidFile();
            return 0;
        }
    }
    printf("Cannot kill daemon.\n");
    return 1;
}

// ---- 实际 daemon 入口（由 cmdStart 通过 CreateProcess 调起） ----
static int daemonMain(uint16_t localPort, uint16_t remotePort,
                      const std::string& serial) {
    if (!tryAcquireDaemonLock()) {
        printf("Daemon is already running.\n");
        return 1;
    }

    // 日志重定向
    char exeDir[MAX_PATH];
    GetModuleFileNameA(nullptr, exeDir, sizeof(exeDir));
    char* lastSlash = strrchr(exeDir, '\\');
    if (!lastSlash) lastSlash = strrchr(exeDir, '/');
    if (lastSlash) *(lastSlash + 1) = '\0';
    std::string logPath = std::string(exeDir) + "molinkd.log";

    freopen(logPath.c_str(), "w", stdout);
    freopen(logPath.c_str(), "a", stderr);
    setvbuf(stdout, nullptr, _IONBF, 0);

    printf("MoLink daemon starting (pid=%lu)...\n", GetCurrentProcessId());

    AdbClient client;
    HANDLE hDisconnectEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    client.setDisconnectEvent(hDisconnectEvent);

    if (!client.connect(serial)) {
        printf("FAIL: Cannot connect to device\n");
        CloseHandle(hDisconnectEvent);
        releaseDaemonLock();
        return 1;
    }
    printf("Device: %s\n", client.getSerial().c_str());

    Forwarder forwarder(client, localPort, remotePort);
    if (!forwarder.start()) {
        printf("FAIL: Cannot start forwarder\n");
        CloseHandle(hDisconnectEvent);
        releaseDaemonLock();
        return 1;
    }

    HANDLE hStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    NamedPipeServer pipe("\\\\.\\pipe\\molink");
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
            std::string args = cmd.substr(5);
            size_t space = args.find(' ');
            if (space == std::string::npos) return "fail: Usage: push <local> <remote>";
            std::string local = args.substr(0, space);
            std::string remote = args.substr(space + 1);
            return pushFile(client, local, remote);
        }
        if (cmd.rfind("pull ", 0) == 0) {
            std::string args = cmd.substr(5);
            size_t space = args.find(' ');
            if (space == std::string::npos) return "fail: Usage: pull <remote> <local>";
            std::string remote = args.substr(0, space);
            std::string local = args.substr(space + 1);
            return pullFile(client, remote, local);
        }
        if (cmd.rfind("ls", 0) == 0) {
            std::string path;
            if (cmd.size() > 2) {
                path = cmd.substr(3);
            }
            return listFiles(client, path);
        }
        return "unknown";
    });

    HANDLE hPipeEvent = pipe.start();
    if (!hPipeEvent) {
        printf("FAIL: Cannot create named pipe\n");
        forwarder.stop();
        CloseHandle(hStopEvent);
        CloseHandle(hDisconnectEvent);
        releaseDaemonLock();
        return 1;
    }

    writePidFile();
    printf("Daemon ready. Use 'molink stop' to stop.\n");

    // 保存串号用于重连（disconnect 会清空 m_serial）
    std::string lastSerial = serial;

    auto transitionToDisconnected = [&]() {
        ResetEvent(hDisconnectEvent);
        client.setState(DaemonState::DISCONNECTED);
        forwarder.pause();
        lastSerial = client.getSerial(); // 在 disconnect 前保存
        client.disconnect();
        printf("Daemon: Device disconnected (serial=%s), waiting for reconnect...\n",
               lastSerial.c_str());
    };

    auto tryReconnect = [&]() -> bool {
        printf("Daemon: Attempting reconnect with serial=%s...\n",
               lastSerial.empty() ? "(auto)" : lastSerial.c_str());
        if (client.connect(lastSerial)) {
            forwarder.resume();
            client.setState(DaemonState::CONNECTED);
            printf("Daemon: Reconnected\n");
            return true;
        }
        return false;
    };

    bool running = true;
    DWORD lastReconnectAttempt = 0; // tick count of last attempt

    while (running) {
        DaemonState state = client.getState();
        // Use short timeout so pipe events don't starve reconnect checks
        DWORD timeout = 500; // check every 500ms

        HANDLE handles[3] = { hStopEvent, hPipeEvent, hDisconnectEvent };
        DWORD ret = WaitForMultipleObjects(3, handles, FALSE, timeout);

        if (ret == WAIT_OBJECT_0) {
            // hStopEvent
            client.setState(DaemonState::STOPPING);
            running = false;
        }
        else if (ret == WAIT_OBJECT_0 + 1) {
            // hPipeEvent
            pipe.processConnection();
        }
        else if (ret == WAIT_OBJECT_0 + 2) {
            // hDisconnectEvent — device removed
            transitionToDisconnected();
            lastReconnectAttempt = GetTickCount(); // reset timer on fresh disconnect
        }

        // Try reconnect if disconnected and enough time passed
        if (state == DaemonState::DISCONNECTED) {
            DWORD now = GetTickCount();
            if (now - lastReconnectAttempt >= 1000) {
                lastReconnectAttempt = now;
                tryReconnect();
            }
        }
    }

    printf("Daemon stopping...\n");
    pipe.stop();
    forwarder.stop();
    client.disconnect();
    CloseHandle(hStopEvent);
    CloseHandle(hDisconnectEvent);
    removePidFile();
    releaseDaemonLock();
    return 0;
}

// ---- molink start 命令 ----
static int cmdStart(uint16_t localPort, uint16_t remotePort,
                    const std::string& serial) {
    // 如果已有 daemon，先停旧再启新
    if (!sendPipeCmd("status").empty()) {
        printf("Daemon is running, restarting...\n");
        auto resp = sendPipeCmd("stop");
        if (resp != "ok") {
            printf("FAIL: Cannot stop existing daemon\n");
            return 1;
        }
        // 等旧进程完全退出
        Sleep(1500);
    }

    // 拼命令行: molink.exe --daemon <args>
    char exePath[MAX_PATH];
    GetModuleFileNameA(nullptr, exePath, sizeof(exePath));

    std::string cmdLine = "\"" + std::string(exePath) + "\" --daemon";
    cmdLine += " -p " + std::to_string(localPort);
    cmdLine += " -r " + std::to_string(remotePort);
    if (!serial.empty()) cmdLine += " -s " + serial;

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};

    if (!CreateProcessA(exePath, &cmdLine[0],
                        nullptr, nullptr, FALSE,
                        DETACHED_PROCESS | CREATE_NO_WINDOW,
                        nullptr, nullptr, &si, &pi)) {
        printf("FAIL: Cannot spawn daemon process: %lu\n", GetLastError());
        return 1;
    }

    CloseHandle(pi.hThread);

    // 等待 daemon 启动完成（最多 8 秒）
    printf("Starting daemon (pid=%lu)...\n", pi.dwProcessId);
    bool ok = false;
    for (int i = 0; i < 16; i++) {
        Sleep(500);
        auto resp = sendPipeCmd("status");
        if (!resp.empty() && resp.find("connected") != std::string::npos) {
            printf("%s\n", resp.c_str());
            ok = true;
            break;
        }
        // 进程是否已退出（启动失败）
        DWORD exitCode = STILL_ACTIVE;
        if (GetExitCodeProcess(pi.hProcess, &exitCode) && exitCode != STILL_ACTIVE) {
            printf("FAIL: Daemon exited with code %lu\n", exitCode);
            break;
        }
    }
    if (!ok) {
        printf("FAIL: Daemon did not start. Check molinkd.log\n");
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess);
        return 1;
    }

    CloseHandle(pi.hProcess);
    return 0;
}

// ---- 前台模式 ----
static int foregroundMode(uint16_t localPort, uint16_t remotePort,
                           const std::string& serial) {
    printf("=== MoLink Access ===\n");
    printf("Local: 127.0.0.1:%u -> Device tcp:%u\n", localPort, remotePort);

    AdbClient client;
    HANDLE hDisconnectEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    client.setDisconnectEvent(hDisconnectEvent);

    if (!client.connect(serial)) {
        printf("FAIL: Cannot connect to device\n");
        CloseHandle(hDisconnectEvent);
        return 1;
    }
    printf("Device: %s\n", client.getSerial().c_str());

    Forwarder forwarder(client, localPort, remotePort);
    if (!forwarder.start()) {
        printf("FAIL: Cannot start forwarder\n");
        CloseHandle(hDisconnectEvent);
        return 1;
    }

    g_forwarder = &forwarder;
    SetConsoleCtrlHandler(ctrlHandler, TRUE);
    printf("Forwarding active. Press Ctrl+C to stop.\n");

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
                Sleep(1000);
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

    printf("MoLink stopped.\n");
    return 0;
}

// ---- 帮助 ----
static void printUsage() {
    printf("MoLink Access - ADB USB Proxy\n\n"
           "Usage:\n"
           "  molink run  [options]         Run in foreground\n"
           "  molink start [options]        Start daemon in background\n"
           "  molink stop                   Stop running daemon\n"
           "  molink devices                List ADB devices\n"
           "  molink status                 Show daemon status\n"
           "  molink push     <local> <remote>  Upload file to device\n"
           "  molink pull     <remote> <local>  Download file from device\n"
           "  molink ls       [remote_path]     List device directory\n"
           "  molink --help                 Show this help\n\n"
           "Options:\n"
           "  --port, -p <port>             Local TCP port (default: 1080)\n"
           "  --rport, -r <port>            Remote device port (default: 1081)\n"
           "  --serial, -s <sn>             Device serial number\n");
}

// ---- entry ----
int main(int argc, char* argv[]) {
    setvbuf(stdout, nullptr, _IONBF, 0);

    // 无参数 → 帮助
    if (argc < 2) {
        printUsage();
        return 0;
    }

    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        printUsage();
        return 0;
    }

    // 命令模式
    if (strcmp(argv[1], "devices") == 0) return cmdDevices();
    if (strcmp(argv[1], "status") == 0)  return cmdStatus();
    if (strcmp(argv[1], "stop") == 0)    return cmdStop();

    // push/pull/ls CLI dispatch
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
        printf("%s", resp.c_str());
        return 0;
    }

    // 解析 options
    uint16_t localPort = 1080;
    uint16_t remotePort = 1081;
    std::string serial;
    bool isRun = (strcmp(argv[1], "run") == 0);
    bool isStart = (strcmp(argv[1], "start") == 0);
    bool isDaemon = (strcmp(argv[1], "--daemon") == 0);
    int startIdx = (isRun || isStart || isDaemon) ? 2 : 1;

    for (int i = startIdx; i < argc; i++) {
        if ((strcmp(argv[i], "--port") == 0 || strcmp(argv[i], "-p") == 0) && i + 1 < argc)
            localPort = (uint16_t)atoi(argv[++i]);
        else if ((strcmp(argv[i], "--rport") == 0 || strcmp(argv[i], "-r") == 0) && i + 1 < argc)
            remotePort = (uint16_t)atoi(argv[++i]);
        else if ((strcmp(argv[i], "--serial") == 0 || strcmp(argv[i], "-s") == 0) && i + 1 < argc)
            serial = argv[++i];
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printUsage(); return 0;
        }
    }

    if (isStart)  return cmdStart(localPort, remotePort, serial);
    if (isDaemon) return daemonMain(localPort, remotePort, serial);
    if (isRun)    return foregroundMode(localPort, remotePort, serial);

    printf("Unknown command: %s\n", argv[1]);
    printUsage();
    return 1;
}
