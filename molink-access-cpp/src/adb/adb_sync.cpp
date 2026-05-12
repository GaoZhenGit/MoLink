#include "adb_sync.h"
#include "adb_reader.h"
#include "adb_client.h"
#include <cstdio>
#include <cstring>
#include <algorithm>
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

        // Try to assemble a complete message from syncBuf + queue
        std::vector<uint8_t> raw;
        if (!ch->syncBuf.empty()) {
            raw = std::move(ch->syncBuf);
            ch->syncBuf.clear();
        }

        while (!ch->dataQueue.empty() && raw.size() < sizeof(SyncMsg)) {
            auto& front = ch->dataQueue.front();
            size_t need = sizeof(SyncMsg) - raw.size();
            size_t take = std::min(need, front.size());
            raw.insert(raw.end(), front.begin(), front.begin() + take);
            if (take < front.size()) {
                // Front still has data, shift it
                front.erase(front.begin(), front.begin() + take);
            } else {
                ch->dataQueue.pop();
            }
        }

        // Now check if we have at least a header
        if (raw.size() >= sizeof(SyncMsg)) {
            memcpy(&msg, raw.data(), sizeof(SyncMsg));
            uint32_t msgLen = sizeof(SyncMsg) + msg.data_length;

            // Try to get remaining bytes for payload
            while (!ch->dataQueue.empty() && raw.size() < msgLen) {
                auto& front = ch->dataQueue.front();
                size_t need = msgLen - raw.size();
                size_t take = std::min(need, front.size());
                raw.insert(raw.end(), front.begin(), front.begin() + take);
                if (take < front.size()) {
                    front.erase(front.begin(), front.begin() + take);
                } else {
                    ch->dataQueue.pop();
                }
            }

            if (raw.size() >= msgLen) {
                payload.assign(raw.begin() + sizeof(SyncMsg),
                              raw.begin() + msgLen);

                // Push leftover data back
                if (raw.size() > msgLen) {
                    std::vector<uint8_t> leftover(raw.begin() + msgLen, raw.end());
                    std::queue<std::vector<uint8_t>> temp;
                    temp.push(std::move(leftover));
                    while (!ch->dataQueue.empty()) {
                        temp.push(std::move(ch->dataQueue.front()));
                        ch->dataQueue.pop();
                    }
                    ch->dataQueue = std::move(temp);
                }
                return true;
            }

            // Not enough data yet — save to syncBuf and wait for more
            ch->syncBuf = std::move(raw);
        } else if (!raw.empty()) {
            // Have some data but not even a header yet — save and wait
            ch->syncBuf = std::move(raw);
        }

        if (ch->closed && ch->dataQueue.empty()) {
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
