#include "file_pull.h"
#include "../adb/adb_client.h"
#include "../adb/adb_reader.h"
#include "../adb/adb_sync.h"
#include "../utils/win_utils.h"
#include <cstdio>
#include "log.h"
#include <cstring>

std::string pullFile(AdbClient& client,
                     const std::string& remotePath,
                     const std::string& localPath) {
    auto ch = client.openChannel("sync:");
    if (!ch) {
        return "fail: Cannot open sync channel";
    }

    if (!syncRecv(client, ch, remotePath)) {
        client.closeChannel(ch);
        return "fail: Failed to send RECV";
    }

    SyncMsg msg;
    std::vector<uint8_t> payload;
    if (!syncReadResponse(ch, msg, payload, 60000)) {
        client.closeChannel(ch);
        return "fail: Sync timeout waiting for file data";
    }

    if (msg.id == SYNC_FAIL) {
        std::string err(payload.begin(), payload.end());
        client.closeChannel(ch);
        char buf[512];
        snprintf(buf, sizeof(buf), "fail: %s", err.c_str());
        return buf;
    }

    FILE* f = fopenUtf8(localPath.c_str(), "wb");
    if (!f) {
        client.closeChannel(ch);
        return "fail: Cannot create local file";
    }

    int64_t totalReceived = 0;
    bool done = false;
    std::string error;

    while (!done) {
        if (msg.id == SYNC_DATA) {
            fwrite(payload.data(), 1, payload.size(), f);
            totalReceived += payload.size();
            LOG_DEBUG("PULL", "Received %lld bytes...\r", totalReceived);
        } else if (msg.id == SYNC_DONE) {
            done = true;
            LOG_DEBUG("PULL", "DONE, total %lld bytes", totalReceived);
            break;
        } else if (msg.id == SYNC_FAIL) {
            std::string err(payload.begin(), payload.end());
            char buf[512];
            snprintf(buf, sizeof(buf), "fail: %s", err.c_str());
            error = buf;
            break;
        }

        if (!done) {
            if (!syncReadResponse(ch, msg, payload, 60000)) {
                error = "fail: Sync timeout during pull";
                break;
            }
        }
    }

    fclose(f);

    if (!error.empty()) {
        client.closeChannel(ch);
        return error;
    }

    SyncMsg okayHdr;
    okayHdr.id = SYNC_OKAY;
    okayHdr.data_length = 0;
    client.writeChannel(ch, &okayHdr, sizeof(SyncMsg));

    syncQuit(client, ch);
    client.closeChannel(ch);
    return "ok";
}