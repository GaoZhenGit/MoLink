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
#include <memory>
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
    // 线程入口：relayImpl 返回后置 done，供 acceptLoop 回收线程对象
    void relay(SOCKET clientSock, std::shared_ptr<std::atomic<bool>> done);
    void relayImpl(SOCKET clientSock);
    void reapFinishedRelays();

    bool tryAcquireSlot();
    void releaseSlot();

    AdbClient& m_client;
    const uint16_t m_localPort;
    const uint16_t m_remotePort;
    SOCKET m_listenSock = INVALID_SOCKET;
    std::atomic<bool> m_running{false};
    std::thread m_thread;
    struct RelayEntry {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;  // relay 结束后置 true
    };
    // relay 线程保持 joinable：stop() 里逐个 join，避免 detach 线程在
    // Forwarder 析构后仍访问 this（use-after-free）。acceptLoop 定期回收
    // 已结束的线程，防止 SOCKS5 长跑（连接频繁建立/断开）下 vector 无界
    // 增长。只有 acceptLoop 写入 vector；relay 线程只写各自的 done 标志，
    // 无需额外加锁。stop() 先 join accept 线程再遍历，顺序安全。
    std::vector<RelayEntry> m_relays;
    bool m_wsaStarted = false;

    // 并发控制
    static constexpr int kMaxConnections = 16;
    std::atomic<bool> m_paused{false};
    int m_activeCount = 0;
    std::mutex m_slotMutex;
};

#endif
