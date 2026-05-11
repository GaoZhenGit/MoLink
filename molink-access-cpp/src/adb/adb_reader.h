#ifndef ADB_READER_H
#define ADB_READER_H

#include <unordered_map>
#include <queue>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <vector>
#include <cstdint>
#include <functional>

class AdbTransport;

struct Channel {
    uint32_t localId;
    uint32_t remoteId;
    std::queue<std::vector<uint8_t>> dataQueue;
    std::mutex mtx;
    std::condition_variable cv;
    bool closed = false;
    bool draining = false;
};

using ChannelPtr = std::shared_ptr<Channel>;
using ChannelMap = std::unordered_map<uint32_t, ChannelPtr>;

struct PendingOpen {
    uint32_t remoteId = 0;
    bool done = false;
    bool error = false;
    std::mutex mtx;
    std::condition_variable cv;
};
using PendingOpenPtr = std::shared_ptr<PendingOpen>;
using PendingMap = std::unordered_map<uint32_t, PendingOpenPtr>;

using DisconnectCallback = std::function<void()>;

class AdbReader {
public:
    AdbReader();
    ~AdbReader();

    void start(AdbTransport* transport, ChannelMap* channels,
               PendingMap* pending, std::mutex* mapMutex, std::mutex* writeMutex);
    void stop();

    void setDisconnectCallback(DisconnectCallback cb) { m_onDisconnect = cb; }

private:
    void readLoop();

    DisconnectCallback m_onDisconnect;

    AdbTransport* m_transport = nullptr;
    ChannelMap* m_channels = nullptr;
    PendingMap* m_pending = nullptr;
    std::mutex* m_mapMutex = nullptr;
    std::mutex* m_writeMutex = nullptr;
    std::thread m_thread;
    std::atomic<bool> m_running{false};
};

#endif
