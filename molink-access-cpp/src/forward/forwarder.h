#ifndef FORWARDER_H
#define FORWARDER_H

#include <winsock2.h>
#include <windows.h>
#include "../adb/adb_client.h"
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>

class Forwarder {
public:
    Forwarder(AdbClient& client, uint16_t localPort, uint16_t remotePort);
    ~Forwarder();

    bool start();
    void stop();
    bool isRunning() const { return m_running; }
    uint16_t getLocalPort() const { return m_localPort; }

private:
    void runLoop();        // 主循环: accept → relay → accept ...
    bool relay(SOCKET clientSock);

    AdbClient& m_client;
    uint16_t m_localPort;
    uint16_t m_remotePort;
    SOCKET m_listenSock = INVALID_SOCKET;
    std::atomic<bool> m_running{false};
    std::thread m_thread;
};

#endif
