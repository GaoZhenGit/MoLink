#include "file_pull.h"
#include "../adb/adb_client.h"
#include "../adb/adb_reader.h"
#include "../adb/adb_sync.h"
#include "../adb/adb_shell.h"
#include "../utils/win_utils.h"
#include "../utils/time_utils.h"
#include <cstdio>
#include "log.h"
#include <cstring>

std::string pullFile(AdbClient& client,
                     const std::string& remotePath,
                     const std::string& localPath) {
    // 先通过 shell: 通道取远端 mtime（独立通道，不污染 sync:）。
    // 失败/取不到时返回 0，事后不还原（与 adb pull 默认行为对齐）。
    uint32_t remoteMtime = getRemoteMtime(client, remotePath);
    if (remoteMtime != 0) {
        LOG_DEBUG("PULL", "remote mtime=%u", remoteMtime);
    }

    auto ch = client.openChannel("sync:");
    if (!ch) {
        return "fail: Cannot open sync channel";
    }

    // 注：上一版曾尝试用 syncStat()（走 sync: 通道）拿 mtime，但 Flyme 等
    // 厂商的 adbd 对 STAT 响应格式不一致，导致 ch->syncBuf 残留字节错位后续帧，
    // 整条 pull 链路卡住 60s×2（详见 molinkd.log）。改走 shell 通道后隔离干净。

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

    // 还原 last write / last access time（zip 解压后内部文件的 mtime 由
    // zip_utils.h 的 UT 扩展负责；此处只处理 .molink.zip / 单文件 自身）
    if (remoteMtime != 0) {
        timeu::setMtime(localPath, (time_t)remoteMtime);
    }

    SyncMsg okayHdr;
    okayHdr.id = SYNC_OKAY;
    okayHdr.data_length = 0;
    client.writeChannel(ch, &okayHdr, sizeof(SyncMsg));

    syncQuit(client, ch);
    client.closeChannel(ch);
    return "ok";
}