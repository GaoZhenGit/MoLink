#include "forwarder.h"
#include "log.h"
#include <cstdio>

Forwarder::Forwarder(AdbClient& client, uint16_t localPort, uint16_t remotePort)
    : m_client(client), m_localPort(localPort), m_remotePort(remotePort) {}

Forwarder::~Forwarder() {
    stop();
}

bool Forwarder::start() {
    if (m_running) return true;

    if (!m_client.isConnected()) {
        LOG_ERROR("FWD", "ADB not connected");
        return false;
    }

    if (!m_wsaStarted) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        m_wsaStarted = true;
    }

    m_listenSock = socket(AF_INET, SOCK_STREAM, 0);
    if (m_listenSock == INVALID_SOCKET) {
        LOG_ERROR("FWD", "socket failed: %d", WSAGetLastError());
        return false;
    }

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = htons(m_localPort);

    if (bind(m_listenSock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        LOG_ERROR("FWD", "bind :%u failed: %d", m_localPort, WSAGetLastError());
        closesocket(m_listenSock);
        m_listenSock = INVALID_SOCKET;
        return false;
    }

    if (listen(m_listenSock, SOMAXCONN) == SOCKET_ERROR) {
        LOG_ERROR("FWD", "listen failed: %d", WSAGetLastError());
        closesocket(m_listenSock);
        m_listenSock = INVALID_SOCKET;
        return false;
    }

    m_running = true;
    m_thread = std::thread(&Forwarder::acceptLoop, this);
    LOG_INFO("FWD", "listening 127.0.0.1:%u -> device tcp:%u max %d",
             m_localPort, m_remotePort, kMaxConnections);
    return true;
}

void Forwarder::stop() {
    m_running = false;
    if (m_listenSock != INVALID_SOCKET) {
        closesocket(m_listenSock);
        m_listenSock = INVALID_SOCKET;
    }
    if (m_thread.joinable()) {
        m_thread.join();
    }
    // 等所有 relay 结束再返回（openChannel/send 均有超时兜底，最坏 ~30s），
    // 保证析构时没有线程还在访问 this。
    for (auto& e : m_relays) {
        if (e.thread.joinable()) e.thread.join();
    }
    m_relays.clear();
    if (m_wsaStarted) {
        WSACleanup();
        m_wsaStarted = false;
    }
}

int Forwarder::getConnectionCount() const {
    return m_activeCount;
}

bool Forwarder::tryAcquireSlot() {
    std::lock_guard<std::mutex> lock(m_slotMutex);
    if (m_activeCount >= kMaxConnections) return false;
    m_activeCount++;
    return true;
}

void Forwarder::releaseSlot() {
    std::lock_guard<std::mutex> lock(m_slotMutex);
    m_activeCount--;
}

void Forwarder::pause() {
    m_paused = true;
    LOG_INFO("FWD", "paused, %d active relays", m_activeCount);
    for (int i = 0; i < 15 && m_activeCount > 0; i++) {
        Sleep(100);
    }
    if (m_activeCount > 0) {
        LOG_WARN("FWD", "%d relays still active after 1.5s", m_activeCount);
    }
}

void Forwarder::resume() {
    m_paused = false;
    LOG_INFO("FWD", "resumed");
}

void Forwarder::acceptLoop() {
    while (m_running) {
        if (m_paused) {
            Sleep(200);
            continue;
        }
        SOCKET clientSock = accept(m_listenSock, nullptr, nullptr);
        if (clientSock == INVALID_SOCKET) {
            if (m_running) LOG_ERROR("FWD", "accept failed: %d", WSAGetLastError());
            break;
        }

        if (!tryAcquireSlot()) {
            LOG_WARN("FWD", "too many connections, rejecting");
            closesocket(clientSock);
            continue;
        }

        LOG_INFO("FWD", "client connected (%d active)", m_activeCount);
        auto done = std::make_shared<std::atomic<bool>>(false);
        m_relays.push_back(RelayEntry{
            std::thread(&Forwarder::relay, this, clientSock, done), done});
        reapFinishedRelays();
    }
}

void Forwarder::relay(SOCKET clientSock,
                      std::shared_ptr<std::atomic<bool>> done) {
    relayImpl(clientSock);
    *done = true;
}

void Forwarder::reapFinishedRelays() {
    for (auto it = m_relays.begin(); it != m_relays.end();) {
        if (it->done->load() && it->thread.joinable()) {
            it->thread.join();
            it = m_relays.erase(it);
        } else {
            ++it;
        }
    }
}

void Forwarder::relayImpl(SOCKET clientSock) {
    // 限制阻塞 send 的时长：客户端停住接收时，不能让 relay 线程（以及
    // stop() 的 join）无限期卡住。
    int sndTimeout = 30000;
    setsockopt(clientSock, SOL_SOCKET, SO_SNDTIMEO,
               (const char*)&sndTimeout, sizeof(sndTimeout));

    std::string dest = "tcp:" + std::to_string(m_remotePort);

    auto ch = m_client.openChannel(dest);
    if (!ch) {
        LOG_ERROR("FWD", "failed to open channel to %s", dest.c_str());
        closesocket(clientSock);
        releaseSlot();
        return;
    }

    char tag[32];
    snprintf(tag, sizeof(tag), "FWD:%u/%u", ch->localId, ch->remoteId);
    LOG_INFO(tag, "channel to %s opened", dest.c_str());

    while (m_running) {
        // 1. 优先排空 ADB → 客户端（高吞吐方向）
        {
            std::unique_lock<std::mutex> lock(ch->mtx);
            while (!ch->dataQueue.empty()) {
                auto data = std::move(ch->dataQueue.front());
                ch->dataQueue.pop();
                lock.unlock();

                int sent = send(clientSock, (const char*)data.data(),
                               (int)data.size(), 0);
                if (sent == SOCKET_ERROR) {
                    LOG_ERROR(tag, "send to client failed: %d", WSAGetLastError());
                    goto done;
                }

                lock.lock();
            }
            if (ch->closed) {
                LOG_WARN(tag, "channel closed by device");
                break;
            }
        }

        // 2. 客户端 → ADB（非阻塞排空）
        {
            while (true) {
                fd_set fds;
                FD_ZERO(&fds);
                FD_SET(clientSock, &fds);
                timeval tv = {0, 0};
                if (select(0, &fds, nullptr, nullptr, &tv) <= 0) break;

                char buf[8192];
                int n = recv(clientSock, buf, sizeof(buf), 0);
                if (n > 0) {
                    if (!m_client.writeChannel(ch, buf, n)) {
                        LOG_ERROR(tag, "ADB write failed");
                        goto done;
                    }
                } else if (n == 0) {
                    LOG_INFO(tag, "client disconnected");
                    goto done;
                } else {
                    break;
                }
            }
        }

        // 3. 等待新数据
        {
            std::unique_lock<std::mutex> lock(ch->mtx);
            if (ch->dataQueue.empty() && !ch->closed) {
                ch->cv.wait_for(lock, std::chrono::milliseconds(100));
            }
        }
    }

done:
    m_client.closeChannel(ch);
    closesocket(clientSock);
    releaseSlot();
    LOG_INFO(tag, "relay ended (%d active)", m_activeCount);
}
