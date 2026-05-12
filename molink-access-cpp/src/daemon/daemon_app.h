#ifndef DAEMON_APP_H
#define DAEMON_APP_H

#include <string>
#include <memory>
#include <cstdint>
#include <winsock2.h>
#include <windows.h>
#include "../adb/adb_client.h"
#include "../forward/forwarder.h"
#include "../cli/named_pipe.h"

// ---- PID 文件（daemon / CLI 共用）----
std::string getPidPath();
void writePidFile();
DWORD readPidFile();
void removePidFile();

// ---- 单实例锁 ----
bool tryAcquireDaemonLock();
void releaseDaemonLock();

// ---- Daemon 核心类 ----
class DaemonApp {
public:
    DaemonApp(uint16_t localPort, uint16_t remotePort, const std::string& serial);
    ~DaemonApp();

    int run();

private:
    std::string onPipeCommand(const std::string& cmd);
    void transitionToDisconnected();
    bool tryReconnectDevice();

    uint16_t m_localPort;
    uint16_t m_remotePort;
    std::string m_serial;
    std::string m_lastSerial;

    AdbClient m_client;
    std::unique_ptr<Forwarder> m_forwarder;
    NamedPipeServer m_pipe;

    HANDLE m_hStopEvent = nullptr;
    HANDLE m_hDisconnectEvent = nullptr;
};

#endif
