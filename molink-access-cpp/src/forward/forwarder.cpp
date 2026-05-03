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

    if (listen(m_listenSock, 1) == SOCKET_ERROR) {
        printf("FWD: listen() failed: %d\n", WSAGetLastError());
        closesocket(m_listenSock);
        m_listenSock = INVALID_SOCKET;
        return false;
    }

    m_running = true;
    m_thread = std::thread(&Forwarder::runLoop, this);
    printf("FWD: Listening on 127.0.0.1:%u → device tcp:%u\n",
           m_localPort, m_remotePort);
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

void Forwarder::runLoop() {
    while (m_running) {
        printf("FWD: Waiting for connection...\n");

        SOCKET clientSock = accept(m_listenSock, nullptr, nullptr);
        if (clientSock == INVALID_SOCKET) {
            if (m_running) {
                printf("FWD: accept() failed: %d\n", WSAGetLastError());
            }
            break;
        }
        printf("FWD: Client connected\n");

        relay(clientSock);
        closesocket(clientSock);
    }
}

bool Forwarder::relay(SOCKET clientSock) {
    std::string dest = "tcp:" + std::to_string(m_remotePort);

    auto ch = m_client.openChannel(dest);
    if (!ch.valid()) {
        printf("FWD: Failed to open ADB channel to %s\n", dest.c_str());
        return false;
    }
    printf("FWD: Channel to %s opened (local=%u remote=%u)\n",
           dest.c_str(), ch.localId, ch.remoteId);

    while (m_running) {
        // 本地 socket → ADB
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(clientSock, &fds);
        timeval tv = {0, 100000}; // 100ms
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

        // ADB → 本地 socket
        std::vector<uint8_t> adbData;
        if (m_client.readChannel(ch, adbData, 50)) {
            int sent = send(clientSock, (const char*)adbData.data(),
                            (int)adbData.size(), 0);
            if (sent == SOCKET_ERROR) {
                printf("FWD: send() to client failed: %d\n", WSAGetLastError());
                break;
            }
        }
    }

    m_client.closeChannel(ch);
    printf("FWD: Channel closed\n");
    return true;
}
