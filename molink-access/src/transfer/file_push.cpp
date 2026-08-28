#include "file_push.h"
#include "../adb/adb_client.h"
#include "../adb/adb_reader.h"
#include "../adb/adb_sync.h"
#include "../adb/adb_shell.h"
#include "../utils/win_utils.h"
#include <cstdio>
#include "log.h"
#include <cstring>
#include <ctime>
#include <sys/stat.h>

std::string pushFile(AdbClient& client,
                     const std::string& localPath,
                     const std::string& remotePath) {
    FILE* f = fopenUtf8(localPath.c_str(), "rb");
    if (!f) {
        char buf[512];
        snprintf(buf, sizeof(buf), "fail: Local file not found: %s",
                 localPath.c_str());
        return buf;
    }

    std::wstring wpath = utf8ToWide(localPath);
    struct _stati64 st;
    if (_wstati64(wpath.c_str(), &st) != 0) {
        fclose(f);
        return "fail: Cannot stat local file";
    }
    uint32_t mtime = (uint32_t)st.st_mtime;

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
        LOG_DEBUG("PUSH", "SEND %s", sendBuf);
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
        LOG_DEBUG("PUSH", "Sent %lld bytes...\r", totalSent);
    }
    LOG_INFO("PUSH", "total %lld bytes sent", totalSent);
    fclose(f);

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
        LOG_DEBUG("PUSH", "DONE sent (mtime=%u)", mtime);
    }

    // SEND + DATA + DONE 是流水线发出的。标准 adbd 回两个帧：
    // 先 OKAY(SEND)，后 OKAY(DONE)；写入中途失败（如磁盘满）时第二帧是 FAIL，
    // SEND 打开失败时第一帧就是 FAIL。
    // 但 Flyme 等厂商的老 adbd 整个 push 只回一个 OKAY（且从不回 FAIL，
    // 实测连坏路径 push 都回 OKAY，见 molinkd.log），所以第二个响应只给
    // 一个短窗口：窗口内来 FAIL 判失败，没有任何帧视为厂商单 OKAY 按成功处理。
    // 大文件写盘耗时长、FAIL 可能晚到，窗口随数据量放宽（16MB 以上给 15s）。
    // 窗口只在"只回单 OKAY"的设备上付出等待代价，AOSP 设备第二帧即时到达。
    int secondTimeout = (totalSent > 16 * 1024 * 1024) ? 15000 : 2000;

    SyncMsg resp;
    std::vector<uint8_t> payload;

    if (!syncReadResponse(ch, resp, payload, 60000)) {
        client.closeChannel(ch);
        return "fail: Sync timeout waiting for SEND response";
    }
    if (resp.id == SYNC_FAIL) {
        std::string err(payload.begin(), payload.end());
        char buf[512];
        snprintf(buf, sizeof(buf), "fail: %s", err.c_str());
        client.closeChannel(ch);
        return buf;
    }
    if (resp.id != SYNC_OKAY) {
        client.closeChannel(ch);
        return "fail: Unexpected sync response";
    }

    if (syncReadResponse(ch, resp, payload, secondTimeout)) {
        if (resp.id == SYNC_FAIL) {
            std::string err(payload.begin(), payload.end());
            char buf[512];
            snprintf(buf, sizeof(buf), "fail: %s", err.c_str());
            client.closeChannel(ch);
            return buf;
        }
        if (resp.id != SYNC_OKAY) {
            client.closeChannel(ch);
            return "fail: Unexpected sync response";
        }
    }

    syncQuit(client, ch);
    client.closeChannel(ch);

    // Flyme 等厂商老 adbd 会把 DONE 的 mtime 字段解析错位（真机实测把远端
    // mtime 设成了 4 = DONE 帧 data_length），导致 mtime 丢失。这里用
    // shell touch 补偿（toybox touch -t 已验证可用）。
    // 注意：touch -t 按设备本地时间解释，假设 PC 与设备同时区（本工具
    // 使用场景成立）；失败不致命——只记日志，AOSP 设备上此步只是把
    // 正确值再设一遍。
    if (mtime != 0) {
        time_t t = (time_t)mtime;
        struct tm tmv;
        if (localtime_s(&tmv, &t) != 0) {
            LOG_WARN("PUSH", "localtime_s failed (mtime=%u), skip touch", mtime);
        } else {
            char stamp[32];
            snprintf(stamp, sizeof(stamp), "%04d%02d%02d%02d%02d.%02d",
                     tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                     tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
            std::string fixCmd = "touch -t " + std::string(stamp) +
                                 " '" + remotePath + "'";
            shellCommand(client, fixCmd);
        }
    }

    return "ok";
}
