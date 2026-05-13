#include "adb_reader.h"
#include "adb_transport.h"
#include <cstdio>
#include <chrono>

AdbReader::AdbReader() {}
AdbReader::~AdbReader() { stop(); }

void AdbReader::start(AdbTransport* transport, ChannelMap* channels,
                       PendingMap* pending, std::mutex* mapMutex,
                       std::mutex* writeMutex) {
    if (m_running) return;
    m_transport = transport;
    m_channels = channels;
    m_pending = pending;
    m_mapMutex = mapMutex;
    m_writeMutex = writeMutex;
    m_running = true;
    m_thread = std::thread(&AdbReader::readLoop, this);
}

void AdbReader::stop() {
    m_running = false;
    if (m_thread.joinable()) m_thread.join();
}

void AdbReader::readLoop() {
    while (m_running) {
        AdbMessage msg;
        std::vector<uint8_t> data;
        if (!m_transport->recv(msg, data, 100)) {
            // Trigger disconnect on fatal USB errors or protocol errors (bad magic, etc.)
            // Normal timeouts during idle are NOT fatal.
            if ((m_transport->hadFatalError() || m_transport->hadProtocolError()) && m_onDisconnect) {
                printf("ADB: Fatal error (USB=%d protocol=%d), disconnecting...\n",
                       m_transport->hadFatalError(), m_transport->hadProtocolError());
                m_onDisconnect();
                while (m_running) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                break;
            }
            continue;
        }

        uint32_t localId = msg.arg1;

        // 1. 先检查是否是 pending open 的响应
        {
            std::lock_guard<std::mutex> mapLock(*m_mapMutex);
            auto it = m_pending->find(localId);
            if (it != m_pending->end()) {
                auto po = it->second;
                {
                    std::lock_guard<std::mutex> poLock(po->mtx);
                    if (msg.command == A_OKAY) {
                        po->remoteId = msg.arg0;
                        po->done = true;
                    } else if (msg.command == A_CLSE) {
                        po->error = true;
                        po->done = true;
                    }
                }
                po->cv.notify_one();
                continue;
            }
        }

        // 2. 查找活跃 channel
        std::lock_guard<std::mutex> mapLock(*m_mapMutex);
        auto it = m_channels->find(localId);
        if (it == m_channels->end()) continue;

        ChannelPtr ch = it->second;

        if (msg.command == A_WRTE) {
            {
                std::lock_guard<std::mutex> wLock(*m_writeMutex);
                m_transport->send(A_OKAY, ch->localId, ch->remoteId, nullptr, 0);
            }
            {
                std::lock_guard<std::mutex> chLock(ch->mtx);
                if (!ch->draining) {
                    ch->dataQueue.push(std::move(data));
                }
            }
            ch->cv.notify_one();
        }
        else if (msg.command == A_CLSE) {
            std::lock_guard<std::mutex> chLock(ch->mtx);
            ch->closed = true;
            ch->cv.notify_one();
        }
        // A_OKAY for writes 丢弃
    }
}
