#include <winsock2.h>
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <memory>

#include "daemon/daemon_app.h"
#include "cli/cli_utils.h"
#include "version.h"

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

// ---- Named Pipe 客户端 ----
std::string sendPipeCmd(const std::string& cmd) {
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

// ---- status / stop（简单 pipe 命令，保留在主文件） ----

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

// ---- molink start 命令 ----
static int cmdStart(uint16_t localPort, uint16_t remotePort,
                    const std::string& serial) {
    if (!sendPipeCmd("status").empty()) {
        printf("Daemon is running, restarting...\n");
        auto resp = sendPipeCmd("stop");
        if (resp != "ok") {
            printf("FAIL: Cannot stop existing daemon\n");
            return 1;
        }
        Sleep(1500);
    }

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

    printf("Starting daemon (pid=%lu)...\n", pi.dwProcessId);
    bool ok = false;
    for (int i = 0; i < 16; i++) {
        Sleep(500);
        auto resp = sendPipeCmd("status");
        if (!resp.empty()) {
            printf("%s\n", resp.c_str());
            ok = true;
            break;
        }
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
            if (!reconnected) break;
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
           "  molink run      [options]         Run daemon in foreground\n"
           "  molink start    [options]         Start daemon in background\n"
           "  molink stop                       Stop running daemon\n"
           "  molink status                     Show daemon status\n"
           "  molink devices                    List USB ADB devices\n"
           "  molink --version, -v              Show version\n"
           "\n"
           "  molink forward  [options]         Start TCP port forwarding\n"
           "\n"
           "  molink push     <local> <remote>  Upload file to device\n"
           "  molink pull     <remote> <local>  Download file from device\n"
           "  molink apush    <path> [--rdir R] Auto upload (zip + .gitignore)\n"
           "  molink apull    [--rdir R]        Interactive auto download + unzip\n"
           "\n"
           "  molink ls       [remote_path]     List device directory\n"
           "  molink del      <remote_path>     Delete file on device\n"
           "  molink adel     [--rdir R]        Interactive delete (b64_ files)\n"
           "  molink shell    <command>         Execute shell command on device\n"
           "  molink auth     [--serial,-s SN]  Trigger device authorization dialog\n"
           "\n"
           "Options (run/start/forward):\n"
           "  --port, -p <port>                Local TCP port (default: 1080)\n"
           "  --rport, -r <port>               Remote device port (default: 1081)\n"
           "  --serial, -s <sn>                Device serial number\n"
           "\n"
           "Options (apush/apull/adel):\n"
           "  --rdir <remote_dir>              Remote directory (default: /sdcard/tmp)\n"
           "\n"
           "  molink --help, -h                Show this help\n");
}

// ---- entry ----
int main(int argc, char* argv[]) {
    setvbuf(stdout, nullptr, _IONBF, 0);

    if (argc < 2) {
        printUsage();
        return 0;
    }

    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        printUsage();
        return 0;
    }
    if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-v") == 0) {
        printVersion();
        return 0;
    }

    // ---- 无连接命令 ----
    if (strcmp(argv[1], "devices") == 0) return cmdDevices();
    if (strcmp(argv[1], "auth") == 0)    return cmdAuth(argc, argv);
    if (strcmp(argv[1], "status") == 0)  return cmdStatus();
    if (strcmp(argv[1], "stop") == 0)    return cmdStop();
    if (strcmp(argv[1], "forward") == 0) return cmdForward(argc, argv);

    // ---- 需要 daemon 的 thin 管道命令 ----
    if (strcmp(argv[1], "push") == 0) {
        if (argc < 4) {
            printf("Usage: molink push <local_file> <remote_path>\n");
            return 1;
        }
        std::string cmd = std::string("push ") + argv[2] + " " + argv[3];
        auto resp = sendPipeCmd(cmd);
        if (resp.empty()) { printf("Daemon is not running. Use 'molink start' first.\n"); return 1; }
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
        if (resp.empty()) { printf("Daemon is not running. Use 'molink start' first.\n"); return 1; }
        printf("%s\n", resp.c_str());
        return (resp == "ok") ? 0 : 1;
    }

    if (strcmp(argv[1], "ls") == 0) {
        std::string path = (argc >= 3) ? argv[2] : "/sdcard/";
        std::string cmd = "ls " + path;
        auto resp = sendPipeCmd(cmd);
        if (resp.empty()) { printf("Daemon is not running. Use 'molink start' first.\n"); return 1; }
        printf("%s", resp.c_str());
        return 0;
    }

    if (strcmp(argv[1], "shell") == 0) {
        if (argc < 3) {
            printf("Usage: molink shell <command>\n");
            return 1;
        }
        std::string shellCmd;
        for (int i = 2; i < argc; i++) {
            if (i > 2) shellCmd += " ";
            shellCmd += argv[i];
        }
        std::string cmd = "shell " + shellCmd;
        auto resp = sendPipeCmd(cmd);
        if (resp.empty()) { printf("Daemon is not running. Use 'molink start' first.\n"); return 1; }
        printf("%s", resp.c_str());
        return 0;
    }

    // ---- 复杂命令（已拆分到 cli/） ----
    if (strcmp(argv[1], "del") == 0)    return cmdDel(argc, argv);
    if (strcmp(argv[1], "adel") == 0)   return cmdAdel(argc, argv);
    if (strcmp(argv[1], "apush") == 0)  return cmdApush(argc, argv);
    if (strcmp(argv[1], "apull") == 0)  return cmdApull(argc, argv);

    // ---- 解析 options（run / start / --daemon） ----
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
    if (isDaemon) {
        DaemonApp app(localPort, remotePort, serial);
        return app.run();
    }
    if (isRun)    return foregroundMode(localPort, remotePort, serial);

    printf("Unknown command: %s\n", argv[1]);
    printUsage();
    return 1;
}
