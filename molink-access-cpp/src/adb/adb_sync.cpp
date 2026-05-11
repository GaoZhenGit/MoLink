#include "adb_sync.h"
#include "adb_reader.h"
#include "adb_client.h"
#include <cstdio>
#include <cstring>
#include <chrono>

static bool sendSyncMsg(AdbClient& client, ChannelPtr ch,
                         uint32_t id, const void* data, uint32_t len) {
    SyncMsg hdr;
    hdr.id = id;
    hdr.data_length = len;

    std::vector<uint8_t> buf(sizeof(hdr) + len);
    memcpy(buf.data(), &hdr, sizeof(hdr));
    if (data && len > 0) {
        memcpy(buf.data() + sizeof(hdr), data, len);
    }
    return client.writeChannel(ch, buf.data(), (uint32_t)buf.size());
}

bool syncReadResponse(ChannelPtr ch, SyncMsg& msg,
                      std::vector<uint8_t>& payload, int timeoutMs) {
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeoutMs);

    while (std::chrono::steady_clock::now() < deadline) {
        std::unique_lock<std::mutex> lock(ch->mtx);
        if (!ch->dataQueue.empty()) {
            auto raw = std::move(ch->dataQueue.front());
            ch->dataQueue.pop();

            if (raw.size() < sizeof(SyncMsg)) {
                lock.unlock();
                printf("SYNC: Short message (%zu bytes)\n", raw.size());
                return false;
            }
            memcpy(&msg, raw.data(), sizeof(SyncMsg));

            uint32_t msgLen = sizeof(SyncMsg) + msg.data_length;
            if (raw.size() < msgLen) {
                lock.unlock();
                printf("SYNC: Incomplete message (%zu < %u)\n", raw.size(), msgLen);
                return false;
            }

            payload.assign(raw.begin() + sizeof(SyncMsg),
                          raw.begin() + msgLen);

            // Push leftover data back (device may batch multiple sync msgs)
            if (raw.size() > msgLen) {
                std::vector<uint8_t> leftover(raw.begin() + msgLen, raw.end());
                // Push to front by swapping with a temporary queue
                std::queue<std::vector<uint8_t>> temp;
                temp.push(std::move(leftover));
                while (!ch->dataQueue.empty()) {
                    temp.push(std::move(ch->dataQueue.front()));
                    ch->dataQueue.pop();
                }
                ch->dataQueue = std::move(temp);
            }

            lock.unlock();
            return true;
        }
        if (ch->closed) {
            printf("SYNC: Channel closed by device\n");
            return false;
        }
        ch->cv.wait_for(lock, std::chrono::milliseconds(100));
    }
    printf("SYNC: Read timeout (%d ms)\n", timeoutMs);
    return false;
}

bool syncSend(AdbClient& client, ChannelPtr ch,
              const std::string& remotePath, uint32_t mode) {
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s,%d", remotePath.c_str(), mode);
    uint32_t len = (uint32_t)strlen(buf);

    if (!sendSyncMsg(client, ch, SYNC_SEND, buf, len)) {
        printf("SYNC: Failed to send SEND\n");
        return false;
    }

    SyncMsg resp;
    std::vector<uint8_t> payload;
    if (!syncReadResponse(ch, resp, payload, 60000)) return false;
    if (resp.id == SYNC_FAIL) {
        std::string err(payload.begin(), payload.end());
        printf("SYNC: SEND failed: %s\n", err.c_str());
        return false;
    }
    if (resp.id != SYNC_OKAY) {
        printf("SYNC: Expected OKAY, got 0x%08X\n", resp.id);
        return false;
    }
    return true;
}

bool syncRecv(AdbClient& client, ChannelPtr ch,
              const std::string& remotePath) {
    uint32_t len = (uint32_t)remotePath.size();
    if (!sendSyncMsg(client, ch, SYNC_RECV, remotePath.data(), len)) {
        printf("SYNC: Failed to send RECV\n");
        return false;
    }
    return true;
}

bool syncQuit(AdbClient& client, ChannelPtr ch) {
    return sendSyncMsg(client, ch, SYNC_QUIT, nullptr, 0);
}
