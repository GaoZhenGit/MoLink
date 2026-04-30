#include "adb_transport.h"
#include "adb_rsa.h"
#include "../usb/usb_device.h"
#include <cstdio>
#include <cstring>

AdbTransport::AdbTransport(UsbDevice& device)
    : m_device(device) {}

bool AdbTransport::readExact(void* buf, uint32_t len, uint32_t timeout_ms) {
    uint8_t* p = (uint8_t*)buf;
    int remain = (int)len;
    while (remain > 0) {
        int bytes = 0;
        if (!m_device.bulkRead(p, remain, &bytes, (int)timeout_ms)) {
            printf("FAIL: bulk read\n");
            return false;
        }
        if (bytes == 0) {
            printf("FAIL: bulk read returned 0 bytes\n");
            return false;
        }
        p += bytes;
        remain -= bytes;
    }
    return true;
}

bool AdbTransport::writeExact(const void* buf, uint32_t len, uint32_t timeout_ms) {
    const uint8_t* p = (const uint8_t*)buf;
    int remain = (int)len;
    while (remain > 0) {
        int bytes = 0;
        if (!m_device.bulkWrite(p, remain, &bytes, (int)timeout_ms)) {
            printf("FAIL: bulk write\n");
            return false;
        }
        p += bytes;
        remain -= bytes;
    }
    return true;
}

uint32_t AdbTransport::crc32(const uint8_t* data, uint32_t len) {
    static uint32_t table[256];
    static bool table_ready = false;
    if (!table_ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t crc = i;
            for (int j = 0; j < 8; j++)
                crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320UL : 0);
            table[i] = crc;
        }
        table_ready = true;
    }
    uint32_t crc = 0xFFFFFFFF;
    for (uint32_t i = 0; i < len; i++)
        crc = (crc >> 8) ^ table[(crc ^ data[i]) & 0xFF];
    return crc ^ 0xFFFFFFFF;
}

bool AdbTransport::send(uint32_t cmd, uint32_t arg0, uint32_t arg1,
                        const void* data, uint32_t data_len) {
    uint32_t crc = data ? crc32((const uint8_t*)data, data_len) : 0;
    AdbMessage msg(cmd, arg0, arg1, data_len, crc);

    printf("ADB SEND: cmd=0x%08X arg0=%u arg1=%u len=%u crc=%u magic=0x%08X\n",
           cmd, arg0, arg1, data_len, crc, msg.magic);

    // 合并 header + data 为单次 USB bulk transfer（与 AOSP adb 一致）
    uint32_t total = sizeof(msg) + data_len;
    std::vector<uint8_t> buf(total);
    memcpy(buf.data(), &msg, sizeof(msg));
    if (data && data_len > 0) {
        memcpy(buf.data() + sizeof(msg), data, data_len);
    }
    if (!writeExact(buf.data(), total, 5000)) {
        printf("FAIL: send header+data\n");
        return false;
    }
    printf("ADB SEND: OK (%u bytes)\n", total);
    return true;
}

bool AdbTransport::recv(AdbMessage& msg, std::vector<uint8_t>& data,
                         uint32_t timeout_ms) {
    data.clear();

    printf("ADB RECV: waiting for header (%u ms)...\n", timeout_ms);
    if (!readExact(&msg, sizeof(msg), timeout_ms)) {
        printf("FAIL: recv header\n");
        return false;
    }

    printf("ADB RECV: cmd=0x%08X arg0=%u arg1=%u len=%u crc=%u magic=0x%08X\n",
           msg.command, msg.arg0, msg.arg1, msg.data_length, msg.data_crc32, msg.magic);

    if (msg.magic != (msg.command ^ 0xFFFFFFFF)) {
        printf("FAIL: bad magic, got 0x%08X expect 0x%08X\n",
               msg.magic, msg.command ^ 0xFFFFFFFF);
        return false;
    }

    if (msg.data_length > MAX_PAYLOAD_V2) {
        printf("FAIL: payload too large: %u\n", msg.data_length);
        return false;
    }

    if (msg.data_length > 0) {
        printf("ADB RECV: reading %u bytes data...\n", msg.data_length);
        data.resize(msg.data_length);
        if (!readExact(data.data(), msg.data_length, timeout_ms)) return false;
    }
    return true;
}

bool AdbTransport::handshake(const std::string& banner) {
    uint32_t maxdata = MAX_PAYLOAD_V2;
    if (!send(A_CNXN, A_VERSION, maxdata, banner.c_str(), (uint32_t)banner.size() + 1)) {
        printf("FAIL: send A_CNXN\n");
        return false;
    }

    AdbMessage msg;
    std::vector<uint8_t> data;
    if (!recv(msg, data, 10000)) {
        printf("FAIL: recv after A_CNXN\n");
        return false;
    }

    if (msg.command == A_CNXN) {
        printf("ADB: Connected! Max=%u Banner=%.*s\n",
               msg.arg1, msg.data_length, (char*)data.data());
        return true;
    }

    if (msg.command == A_AUTH) {
        uint32_t auth_type = msg.arg0;
        printf("ADB: A_AUTH type=%u (token %u bytes, needs RSA signature)\n",
               auth_type, (uint32_t)data.size());
        m_auth_token = data;
        return false;
    }

    printf("FAIL: Unexpected response 0x%08X\n", msg.command);
    return false;
}

uint32_t AdbTransport::openChannel(const std::string& destination, uint32_t local_id) {
    std::vector<uint8_t> data;
    AdbMessage msg;

    if (!send(A_OPEN, local_id, 0,
              destination.c_str(), (uint32_t)destination.size() + 1)) {
        return 0;
    }

    if (!recv(msg, data, 5000)) return 0;
    if (msg.command != A_OKAY) {
        printf("FAIL: Expected A_OKAY, got 0x%08X\n", msg.command);
        return 0;
    }

    printf("ADB: Channel %u opened to %s, remote_id=%u\n",
           local_id, destination.c_str(), msg.arg1);
    return msg.arg1;
}

bool AdbTransport::handshake(AdbRsa& rsa, const std::string& banner) {
    uint32_t maxdata = MAX_PAYLOAD_V2;
    if (!send(A_CNXN, A_VERSION, maxdata, banner.c_str(), (uint32_t)banner.size() + 1)) {
        printf("FAIL: send A_CNXN\n");
        return false;
    }

    bool waiting_for_authorization = false;

    for (int round = 0; round < 3; round++) {
        AdbMessage msg;
        std::vector<uint8_t> data;
        // 发送公钥后可能需要用户在设备上确认授权，超时延长到 120 秒
        uint32_t timeout = waiting_for_authorization ? 120000 : 10000;
        printf("ADB: Waiting for response (round %d, timeout=%u ms)...\n", round, timeout);
        if (!recv(msg, data, timeout)) {
            printf("FAIL: recv after A_CNXN (round %d)\n", round);
            return false;
        }

        if (msg.command == A_CNXN) {
            printf("ADB: Connected! Max=%u Banner=%.*s\n",
                   msg.arg1, (int)data.size(), (char*)data.data());
            return true;
        }

        if (msg.command == A_AUTH && msg.arg0 == AUTH_TOKEN) {
            if (waiting_for_authorization) {
                printf("ADB: Device requested public key\n");
                auto pubkey = rsa.getPublicKey();
                if (pubkey.empty()) {
                    printf("FAIL: Cannot get public key\n");
                    return false;
                }
                // AOSP 格式: [4 字节 LE 长度前缀][Android 公钥数据]
                uint32_t keyLen = (uint32_t)pubkey.size();
                std::vector<uint8_t> payload(4 + pubkey.size());
                payload[0] = keyLen & 0xFF;
                payload[1] = (keyLen >> 8) & 0xFF;
                payload[2] = (keyLen >> 16) & 0xFF;
                payload[3] = (keyLen >> 24) & 0xFF;
                memcpy(payload.data() + 4, pubkey.data(), pubkey.size());
                if (!send(A_AUTH, AUTH_RSAPUBLICKEY, 0, payload.data(), (uint32_t)payload.size())) {
                    printf("FAIL: send AUTH_RSAPUBLICKEY\n");
                    return false;
                }
                printf("ADB: Public key sent (key=%u bytes, total=%u bytes)\n",
                       keyLen, (uint32_t)payload.size());
                waiting_for_authorization = true;  // 下一个 recv 需要长超时（设备授权）
            } else {
                printf("ADB: Signing token (%u bytes)...\n", (uint32_t)data.size());
                auto sig = rsa.signToken(data.data(), data.size());
                if (sig.empty()) {
                    printf("FAIL: RSA signing failed\n");
                    return false;
                }
                if (!send(A_AUTH, AUTH_SIGNATURE, 0, sig.data(), (uint32_t)sig.size())) {
                    printf("FAIL: send AUTH_SIGNATURE\n");
                    return false;
                }
                printf("ADB: Signature sent (%u bytes)\n", (uint32_t)sig.size());
                // 如果设备不认识我们的公钥，下次会再发 AUTH_TOKEN
                waiting_for_authorization = true;
            }
        } else {
            printf("FAIL: Unexpected response 0x%08X (arg0=%u)\n", msg.command, msg.arg0);
            return false;
        }
    }

    printf("FAIL: Handshake loop exhausted\n");
    return false;
}

void AdbTransport::closeChannel(uint32_t local_id, uint32_t remote_id) {
    send(A_CLSE, local_id, remote_id, nullptr, 0);
    printf("ADB: Channel %u/%u closed\n", local_id, remote_id);
}
