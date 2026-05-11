#include "forwarder.h"
#include <cstdio>

Forwarder::Forwarder(AdbClient& client, uint16_t localPort, uint16_t remotePort)
    : m_client(client), m_localPort(localPort), m_remotePort(remotePort) {}

Forwarder::~Forwarder() {
    stop();
}

bool Forwarder::start() {
    if (m_running) return true;

    if (!m_client.isConnected()) {
        printf("FWD: ADB not connected\n");
        return false;
    }

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    m_listenSock = socket(AF_INET, SOCK_STREAM, 0);
    if (m_listenSock == INVALID_SOCKET) {
        printf("FWD: socket() failed: %d\n", WSAGetLastError());
        return false;
    }

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = htons(m_localPort);

    if (bind(m_listenSock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        printf("FWD: bind(:%u) failed: %d\n", m_localPort, WSAGetLastError());
        closesocket(m_listenSock);
        m_listenSock = INVALID_SOCKET;
        return false;
    }

    if (listen(m_listenSock, SOMAXCONN) == SOCKET_ERROR) {
        printf("FWD: listen() failed: %d\n", WSAGetLastError());
        closesocket(m_listenSock);
        m_listenSock = INVALID_SOCKET;
        return false;
    }

    m_running = true;
    m_thread = std::thread(&Forwarder::acceptLoop, this);
    printf("FWD: Listening on 127.0.0.1:%u → device tcp:%u (max %d conn)\n",
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
}

int Forwarder::getConnectionCount() const {
    // m_activeCount 只在 slot mutex 里改，这里给个近似值
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
    printf("FWD: Paused (device disconnected), waiting for %d active relays...\n", m_activeCount);
    for (int i = 0; i < 15 && m_activeCount > 0; i++) {
        Sleep(100);
    }
    if (m_activeCount > 0) {
        printf("FWD: Warning — %d relays still active after 1.5s\n", m_activeCount);
    }
}

void Forwarder::resume() {
    m_paused = false;
    printf("FWD: Resumed\n");
}

void Forwarder::acceptLoop() {
    while (m_running) {
        if (m_paused) {
            Sleep(200);
            continue;
        }
        SOCKET clientSock = accept(m_listenSock, nullptr, nullptr);
        if (clientSock == INVALID_SOCKET) {
            if (m_running) printf("FWD: accept() failed: %d\n", WSAGetLastError());
            break;
        }

        if (!tryAcquireSlot()) {
            printf("FWD: Too many connections, rejecting\n");
            closesocket(clientSock);
            continue;
        }

        printf("FWD: Client connected (%d active)\n", m_activeCount);
        std::thread(&Forwarder::relay, this, clientSock).detach();
    }
}

void Forwarder::relay(SOCKET clientSock) {
    std::string dest = "tcp:" + std::to_string(m_remotePort);

    auto ch = m_client.openChannel(dest);
    if (!ch) {
        printf("FWD: Failed to open ADB channel to %s\n", dest.c_str());
        closesocket(clientSock);
        releaseSlot();
        return;
    }
    printf("FWD: Channel to %s opened (local=%u remote=%u)\n",
           dest.c_str(), ch->localId, ch->remoteId);

    while (m_running) {
        // 客户端 → ADB
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(clientSock, &fds);
        timeval tv = {0, 100000};
        int selRet = select(0, &fds, nullptr, nullptr, &tv);

        if (selRet > 0 && FD_ISSET(clientSock, &fds)) {
            char buf[8192];
            int n = recv(clientSock, buf, sizeof(buf), 0);
            if (n > 0) {
                if (!m_client.writeChannel(ch, buf, n)) {
                    printf("FWD: ADB write failed\n");
                    break;
                }
            } else {
                printf("FWD: Client disconnected (%d)\n", n);
                break;
            }
        }

        // ADB → 客户端（读 channel 队列 + 条件变量）
        {
            std::unique_lock<std::mutex> lock(ch->mtx);
            if (!ch->dataQueue.empty()) {
                auto data = std::move(ch->dataQueue.front());
                ch->dataQueue.pop();
                lock.unlock();

                int sent = send(clientSock, (const char*)data.data(),
                               (int)data.size(), 0);
                if (sent == SOCKET_ERROR) {
                    printf("FWD: send() to client failed: %d\n", WSAGetLastError());
                    break;
                }
            } else if (ch->closed) {
                printf("FWD: Channel closed by device\n");
                break;
            } else {
                ch->cv.wait_for(lock, std::chrono::milliseconds(50));
            }
        }
    }

    m_client.closeChannel(ch);
    closesocket(clientSock);
    releaseSlot();
    printf("FWD: Relay ended (%d active)\n", m_activeCount);
}
