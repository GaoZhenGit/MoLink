#include "daemon_app.h"
#include "../transfer/file_push.h"
#include "../transfer/file_pull.h"
#include "../transfer/file_list.h"
#include "../adb/adb_shell.h"
#include "../usb/usb_device.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

// ============================================================
// PID 文件
// ============================================================

std::string getPidPath() {
    char dir[MAX_PATH];
    GetModuleFileNameA(nullptr, dir, sizeof(dir));
    char* lastSlash = strrchr(dir, '\\');
    if (!lastSlash) lastSlash = strrchr(dir, '/');
    if (lastSlash) *(lastSlash + 1) = '\0';
    return std::string(dir) + "molinkd.pid";
}

void writePidFile() {
    auto path = getPidPath();
    FILE* f = fopen(path.c_str(), "w");
    if (f) {
        fprintf(f, "%lu", GetCurrentProcessId());
        fclose(f);
    }
}

DWORD readPidFile() {
    auto path = getPidPath();
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return 0;
    DWORD pid = 0;
    fscanf(f, "%lu", &pid);
    fclose(f);
    return pid;
}

void removePidFile() {
    auto path = getPidPath();
    remove(path.c_str());
}

// ============================================================
// 单实例锁
// ============================================================

static HANDLE g_daemonMutex = nullptr;

bool tryAcquireDaemonLock() {
    g_daemonMutex = CreateMutexW(nullptr, TRUE, L"Global\\MoLinkDaemon");
    return (GetLastError() != ERROR_ALREADY_EXISTS);
}

void releaseDaemonLock() {
    if (g_daemonMutex) {
        ReleaseMutex(g_daemonMutex);
        CloseHandle(g_daemonMutex);
        g_daemonMutex = nullptr;
    }
}

// ============================================================
// DaemonApp
// ============================================================

DaemonApp::DaemonApp(uint16_t localPort, uint16_t remotePort,
                     const std::string& serial)
    : m_localPort(localPort)
    , m_remotePort(remotePort)
    , m_serial(serial)
    , m_pipe("\\\\.\\pipe\\molink")
{
}

DaemonApp::~DaemonApp() {
    if (m_forwarder) m_forwarder->stop();
    m_client.disconnect();
    if (m_hStopEvent) CloseHandle(m_hStopEvent);
    if (m_hDisconnectEvent) CloseHandle(m_hDisconnectEvent);
    removePidFile();
    releaseDaemonLock();
}

std::string DaemonApp::onPipeCommand(const std::string& cmd) {
    if (cmd == "stop") {
        SetEvent(m_hStopEvent);
        return "ok";
    }
    if (cmd == "status") {
        char buf[256];
        const char* stateStr = (m_client.getState() == DaemonState::CONNECTED)
            ? "connected" : "disconnected";
        if (m_forwarder) {
            snprintf(buf, sizeof(buf),
                     "daemon=running state=%s serial=%s forwarding=%u->%u connections=%d",
                     stateStr,
                     m_client.getSerial().c_str(),
                     m_forwarder->getLocalPort(),
                     m_forwarder->getRemotePort(),
                     m_forwarder->getConnectionCount());
        } else {
            snprintf(buf, sizeof(buf),
                     "daemon=running state=%s serial=%s forwarding=off",
                     stateStr,
                     m_client.getSerial().c_str());
        }
        return buf;
    }
    if (cmd == "devices") {
        // 枚举所有 ADB 设备，返回序列号及授权状态
        std::string result;
        auto devices = UsbDevice::discover();
        std::string connectedSerial = m_client.getSerial();

        std::vector<std::string> foundSerials;
        for (auto& dev : devices) {
            std::string serial = dev.getSerial();
            if (!serial.empty()) foundSerials.push_back(serial);
        }

        // 确保 daemon 已连接的设备在列表中（其 USB 可能被占用导致 getSerial 失败）
        if (!connectedSerial.empty()) {
            bool inList = false;
            for (auto& s : foundSerials) {
                if (s == connectedSerial) { inList = true; break; }
            }
            if (!inList) foundSerials.push_back(connectedSerial);
        }

        for (auto& s : foundSerials) {
            result += s + (s == connectedSerial ? " yes\n" : " ?\n");
        }
        if (result.empty()) result = "(none)\n";
        return result;
    }
    if (cmd.rfind("push ", 0) == 0) {
        std::string args = cmd.substr(5);
        size_t space = args.find(' ');
        if (space == std::string::npos) return "fail: Usage: push <local> <remote>";
        std::string local = args.substr(0, space);
        std::string remote = args.substr(space + 1);
        return pushFile(m_client, local, remote);
    }
    if (cmd.rfind("pull ", 0) == 0) {
        std::string args = cmd.substr(5);
        size_t space = args.find(' ');
        if (space == std::string::npos) return "fail: Usage: pull <remote> <local>";
        std::string remote = args.substr(0, space);
        std::string local = args.substr(space + 1);
        return pullFile(m_client, remote, local);
    }
    if (cmd.rfind("forward ", 0) == 0) {
        std::string args = cmd.substr(8);
        size_t space = args.find(' ');
        if (space == std::string::npos) return "fail: Usage: forward <localPort> <remotePort>";
        uint16_t lp = (uint16_t)atoi(args.substr(0, space).c_str());
        uint16_t rp = (uint16_t)atoi(args.substr(space + 1).c_str());

        if (m_forwarder) {
            m_forwarder->stop();
            m_forwarder.reset();
        }

        m_forwarder = std::make_unique<Forwarder>(m_client, lp, rp);
        if (!m_forwarder->start()) {
            m_forwarder.reset();
            return "fail: Cannot start forwarding";
        }
        char buf[128];
        snprintf(buf, sizeof(buf), "ok: forwarding 127.0.0.1:%u -> device tcp:%u", lp, rp);
        return buf;
    }
    if (cmd.rfind("ls", 0) == 0) {
        std::string path;
        if (cmd.size() > 2) {
            path = cmd.substr(3);
        }
        return listFiles(m_client, path);
    }
    if (cmd.rfind("del ", 0) == 0) {
        std::string path = cmd.substr(4);
        if (path.empty()) return "fail: Usage: del <remote_path>";
        std::string rmCmd = "rm -f '" + path + "'";
        std::string output = shellCommand(m_client, rmCmd);
        if (output.empty()) return "ok";
        return "fail: " + output;
    }
    if (cmd.rfind("shell ", 0) == 0) {
        std::string shellCmd = cmd.substr(6);
        if (shellCmd.empty()) return "fail: Usage: shell <command>";
        return shellCommand(m_client, shellCmd);
    }
    return "unknown";
}

void DaemonApp::transitionToDisconnected() {
    ResetEvent(m_hDisconnectEvent);
    m_client.setState(DaemonState::DISCONNECTED);
    if (m_forwarder) m_forwarder->pause();
    m_lastSerial = m_client.getSerial();
    m_client.disconnect();
    printf("Daemon: Device disconnected (serial=%s), waiting for reconnect...\n",
           m_lastSerial.c_str());
}

bool DaemonApp::tryReconnectDevice() {
    printf("Daemon: Attempting reconnect with serial=%s...\n",
           m_lastSerial.empty() ? "(auto)" : m_lastSerial.c_str());
    if (m_client.connect(m_lastSerial)) {
        if (m_forwarder) m_forwarder->resume();
        m_client.setState(DaemonState::CONNECTED);
        printf("Daemon: Reconnected\n");
        return true;
    }
    return false;
}

int DaemonApp::run() {
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

    m_hDisconnectEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_client.setDisconnectEvent(m_hDisconnectEvent);

    if (!m_client.connect(m_serial)) {
        printf("FAIL: Cannot connect to device\n");
        return 1;
    }
    printf("Device: %s\n", m_client.getSerial().c_str());

    m_hStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    m_pipe.setHandler([this](const std::string& cmd) -> std::string {
        return onPipeCommand(cmd);
    });

    HANDLE hPipeEvent = m_pipe.start();
    if (!hPipeEvent) {
        printf("FAIL: Cannot create named pipe\n");
        return 1;
    }

    writePidFile();
    printf("Daemon ready. Use 'molink stop' to stop.\n");

    m_lastSerial = m_serial;

    bool running = true;
    DWORD lastReconnectAttempt = 0;

    while (running) {
        DaemonState state = m_client.getState();
        DWORD timeout = 500;

        HANDLE handles[3] = { m_hStopEvent, hPipeEvent, m_hDisconnectEvent };
        DWORD ret = WaitForMultipleObjects(3, handles, FALSE, timeout);

        if (ret == WAIT_OBJECT_0) {
            m_client.setState(DaemonState::STOPPING);
            running = false;
        }
        else if (ret == WAIT_OBJECT_0 + 1) {
            m_pipe.processConnection();
        }
        else if (ret == WAIT_OBJECT_0 + 2) {
            transitionToDisconnected();
            lastReconnectAttempt = GetTickCount();
        }

        if (state == DaemonState::DISCONNECTED) {
            DWORD now = GetTickCount();
            if (now - lastReconnectAttempt >= 1000) {
                lastReconnectAttempt = now;
                tryReconnectDevice();
            }
        }
    }

    printf("Daemon stopping...\n");
    m_pipe.stop();
    return 0;
}
