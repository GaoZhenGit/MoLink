#include "file_push.h"
#include "../adb/adb_client.h"
#include "../adb/adb_reader.h"
#include "../adb/adb_sync.h"
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

std::string pushFile(AdbClient& client,
                     const std::string& localPath,
                     const std::string& remotePath) {
    FILE* f = fopen(localPath.c_str(), "rb");
    if (!f) {
        char buf[512];
        snprintf(buf, sizeof(buf), "fail: Local file not found: %s",
                 localPath.c_str());
        return buf;
    }

    struct stat st;
    if (stat(localPath.c_str(), &st) != 0) {
        fclose(f);
        return "fail: Cannot stat local file";
    }

    auto ch = client.openChannel("sync:");
    if (!ch) {
        fclose(f);
        return "fail: Cannot open sync channel";
    }

    // Send SEND (don't wait for OKAY — pipeline with data)
    {
        char sendBuf[1024];
        snprintf(sendBuf, sizeof(sendBuf), "%s,%d", remotePath.c_str(), 0644);
        uint32_t sendLen = (uint32_t)strlen(sendBuf);
        SyncMsg sendHdr;
        sendHdr.id = SYNC_SEND;
        sendHdr.data_length = sendLen;
        std::vector<uint8_t> sendPkt(sizeof(SyncMsg) + sendLen);
        memcpy(sendPkt.data(), &sendHdr, sizeof(SyncMsg));
        memcpy(sendPkt.data() + sizeof(SyncMsg), sendBuf, sendLen);
        if (!client.writeChannel(ch, sendPkt.data(), (uint32_t)sendPkt.size())) {
            fclose(f);
            client.closeChannel(ch);
            return "fail: USB write error sending SEND";
        }
        printf("PUSH: SEND %s\n", sendBuf);
    }

    constexpr size_t kChunkSize = 65536;
    std::vector<uint8_t> chunk(kChunkSize + sizeof(SyncMsg));
    int64_t totalSent = 0;

    while (!feof(f)) {
        size_t n = fread(chunk.data() + sizeof(SyncMsg), 1, kChunkSize, f);
        if (n == 0 && ferror(f)) {
            fclose(f);
            client.closeChannel(ch);
            return "fail: Error reading local file";
        }
        if (n == 0) break;

        SyncMsg* hdr = (SyncMsg*)chunk.data();
        hdr->id = SYNC_DATA;
        hdr->data_length = (uint32_t)n;

        if (!client.writeChannel(ch, chunk.data(),
                                  (uint32_t)(sizeof(SyncMsg) + n))) {
            fclose(f);
            client.closeChannel(ch);
            return "fail: USB write error during push";
        }
        totalSent += n;
        printf("PUSH: Sent %lld bytes...\r", totalSent);
    }
    printf("\nPUSH: Total %lld bytes sent\n", totalSent);
    fclose(f);

    uint32_t mtime = (uint32_t)st.st_mtime;
    {
        SyncMsg doneHdr;
        doneHdr.id = SYNC_DONE;
        doneHdr.data_length = 4;
        std::vector<uint8_t> doneBuf(sizeof(SyncMsg) + 4);
        memcpy(doneBuf.data(), &doneHdr, sizeof(SyncMsg));
        memcpy(doneBuf.data() + sizeof(SyncMsg), &mtime, 4);

        if (!client.writeChannel(ch, doneBuf.data(), (uint32_t)doneBuf.size())) {
            client.closeChannel(ch);
            return "fail: USB write error sending DONE";
        }
        printf("PUSH: DONE sent (mtime=%u)\n", mtime);
    }

    // Now wait for the final response
    SyncMsg resp;
    std::vector<uint8_t> payload;
    if (!syncReadResponse(ch, resp, payload, 60000)) {
        client.closeChannel(ch);
        return "fail: Sync timeout waiting for DONE response";
    }

    syncQuit(client, ch);
    client.closeChannel(ch);

    if (resp.id == SYNC_FAIL) {
        std::string err(payload.begin(), payload.end());
        char buf[512];
        snprintf(buf, sizeof(buf), "fail: %s", err.c_str());
        return buf;
    }
    if (resp.id == SYNC_OKAY) {
        return "ok";
    }
    return "fail: Unexpected sync response";
}
