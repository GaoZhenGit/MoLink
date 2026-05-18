#ifndef FORWARDER_H
#define FORWARDER_H

#include <winsock2.h>
#include <windows.h>
#include "../adb/adb_client.h"
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <cstdint>

class Forwarder {
public:
    Forwarder(AdbClient& client, uint16_t localPort, uint16_t remotePort);
    ~Forwarder();

    bool start();
    void stop();
    void pause();
    void resume();
    bool isRunning() const { return m_running; }
    bool isPaused() const { return m_paused; }
    uint16_t getLocalPort() const { return m_localPort; }
    uint16_t getRemotePort() const { return m_remotePort; }
    int getConnectionCount() const;

private:
    void acceptLoop();
    void relay(SOCKET clientSock);

    bool tryAcquireSlot();
    void releaseSlot();

    AdbClient& m_client;
    const uint16_t m_localPort;
    const uint16_t m_remotePort;
    SOCKET m_listenSock = INVALID_SOCKET;
    std::atomic<bool> m_running{false};
    std::thread m_thread;
    bool m_wsaStarted = false;

    // 并发控制
    static constexpr int kMaxConnections = 16;
    std::atomic<bool> m_paused{false};
    int m_activeCount = 0;
    std::mutex m_slotMutex;
};

#endif
