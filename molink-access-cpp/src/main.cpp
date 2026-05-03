#include <winsock2.h>
#include <windows.h>
#include <cstdio>
#include <cstring>
#include "adb/adb_client.h"
#include "forward/forwarder.h"

static Forwarder* g_forwarder = nullptr;

static BOOL WINAPI ctrlHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
        printf("\nShutting down...\n");
        if (g_forwarder) g_forwarder->stop();
        return TRUE;
    }
    return FALSE;
}

static void printUsage() {
    printf("MoLink Access - ADB USB Proxy\n\n"
           "Usage:\n"
           "  molink [options]\n\n"
           "Options:\n"
           "  --port, -p <port>    Local TCP port (default: 1080)\n"
           "  --rport, -r <port>   Remote device port (default: 1081)\n"
           "  --serial, -s <sn>    Device serial number\n"
           "  --help, -h           Show this help\n\n"
           "Examples:\n"
           "  molink                           # Forward 127.0.0.1:1080 -> device:1081\n"
           "  molink -p 2080 -r 1081           # Custom ports\n"
           "  molink -s 852QLDV923XMM           # Specific device\n");
}

int main(int argc, char* argv[]) {
    setvbuf(stdout, nullptr, _IONBF, 0);

    uint16_t localPort = 1080;
    uint16_t remotePort = 1081;
    std::string serial;

    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)) {
            printUsage();
            return 0;
        }
        if ((strcmp(argv[i], "--port") == 0 || strcmp(argv[i], "-p") == 0) && i + 1 < argc) {
            localPort = (uint16_t)atoi(argv[++i]);
        } else if ((strcmp(argv[i], "--rport") == 0 || strcmp(argv[i], "-r") == 0) && i + 1 < argc) {
            remotePort = (uint16_t)atoi(argv[++i]);
        } else if ((strcmp(argv[i], "--serial") == 0 || strcmp(argv[i], "-s") == 0) && i + 1 < argc) {
            serial = argv[++i];
        } else {
            printf("Unknown option: %s\n", argv[i]);
            printUsage();
            return 1;
        }
    }

    printf("=== MoLink Access ===\n");
    printf("Local: 127.0.0.1:%u -> Device tcp:%u\n", localPort, remotePort);

    // 1. 连接设备
    AdbClient client;
    if (!client.connect(serial)) {
        printf("FAIL: Cannot connect to device\n");
        return 1;
    }
    printf("Device: %s\n", client.getSerial().c_str());

    // 2. 启动转发
    Forwarder forwarder(client, localPort, remotePort);
    if (!forwarder.start()) {
        printf("FAIL: Cannot start forwarder\n");
        return 1;
    }

    // 3. 等待 Ctrl+C
    g_forwarder = &forwarder;
    SetConsoleCtrlHandler(ctrlHandler, TRUE);

    printf("Forwarding active. Press Ctrl+C to stop.\n");

    // 主线程等待 — forwarder 在自己的线程中运行
    while (forwarder.isRunning()) {
        Sleep(500);
    }

    printf("MoLink stopped.\n");
    return 0;
}
