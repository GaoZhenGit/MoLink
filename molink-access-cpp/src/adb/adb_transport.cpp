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
        if (!m_device.bulkRead(p, remain, &bytes, (int)timeout_ms))
            return false;
        if (bytes == 0) return false;
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
        if (!m_device.bulkWrite(p, remain, &bytes, (int)timeout_ms))
            return false;
        p += bytes;
        remain -= bytes;
    }
    return true;
}

bool AdbTransport::send(uint32_t cmd, uint32_t arg0, uint32_t arg1,
                        const void* data, uint32_t data_len) {
    // ADB data_check 是简单的字节求和，不是 CRC32
    uint32_t sum = 0;
    if (data && data_len > 0) {
        const uint8_t* p = (const uint8_t*)data;
        for (uint32_t i = 0; i < data_len; i++) sum += p[i];
    }
    AdbMessage msg(cmd, arg0, arg1, data_len, sum);

    if (!writeExact(&msg, sizeof(msg), 5000)) {
        printf("FAIL: send header\n");
        return false;
    }
    if (data && data_len > 0) {
        if (!writeExact(data, data_len, 5000)) {
            printf("FAIL: send data\n");
            return false;
        }
    }
    return true;
}

bool AdbTransport::recv(AdbMessage& msg, std::vector<uint8_t>& data,
                         uint32_t timeout_ms) {
    data.clear();

    if (!readExact(&msg, sizeof(msg), timeout_ms))
        return false;

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
    if (!send(A_CNXN, A_VERSION, maxdata, banner.c_str(), (uint32_t)banner.size())) {
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
        printf("ADB: A_AUTH type=%u (token %u bytes)\n",
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
           local_id, destination.c_str(), msg.arg0);
    return msg.arg0;
}

bool AdbTransport::handshake(AdbRsa& rsa, const std::string& banner) {
    uint32_t maxdata = MAX_PAYLOAD_V2;
    if (!send(A_CNXN, A_VERSION, maxdata, banner.c_str(), (uint32_t)banner.size())) {
        printf("FAIL: send A_CNXN\n");
        return false;
    }

    bool waiting_for_authorization = false;

    for (int round = 0; round < 3; round++) {
        AdbMessage msg;
        std::vector<uint8_t> data;
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
                // 第二轮 TOKEN: 发送公钥
                printf("ADB: Device requested public key\n");
                std::string payload = rsa.getPubKeyPayload();
                if (payload.empty()) {
                    printf("FAIL: Cannot get public key payload\n");
                    return false;
                }
                if (!send(A_AUTH, AUTH_RSAPUBLICKEY, 0, payload.data(), (uint32_t)payload.size())) {
                    printf("FAIL: send AUTH_RSAPUBLICKEY\n");
                    return false;
                }
                printf("ADB: Public key sent (%zu bytes, round %d)\n", payload.size(), round);
            } else {
                // 第一轮 TOKEN: 签名
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

bool AdbTransport::writeChannel(uint32_t local_id, uint32_t remote_id,
                                const void* data, uint32_t len) {
    return send(A_WRTE, local_id, remote_id, data, len);
}

bool AdbTransport::readChannel(uint32_t local_id, uint32_t remote_id,
                               std::vector<uint8_t>& data, uint32_t timeout_ms) {
    while (true) {
        AdbMessage msg;
        if (!recv(msg, data, timeout_ms)) return false;

        if (msg.command == A_CLSE) {
            printf("ADB: Channel %u/%u closed by remote\n", local_id, remote_id);
            data.clear();
            return false;
        }
        if (msg.command == A_OKAY) {
            // 对方确认收到我们的数据，跳过继续等数据帧
            continue;
        }
        if (msg.command != A_WRTE) {
            printf("ADB: Unexpected cmd 0x%08X on channel %u/%u\n",
                   msg.command, local_id, remote_id);
            return false;
        }
        // 确认收到设备发来的数据，回 A_OKAY
        send(A_OKAY, local_id, remote_id, nullptr, 0);
        printf("ADB: Channel %u/%u read %u bytes\n", local_id, remote_id, (uint32_t)data.size());
        return true;
    }
}

void AdbTransport::closeChannel(uint32_t local_id, uint32_t remote_id) {
    send(A_CLSE, local_id, remote_id, nullptr, 0);
    // 消耗该通道的所有残留消息（A_WRTE/A_OKAY），直到收到 A_CLSE
    for (int i = 0; i < 5; i++) {
        AdbMessage msg;
        std::vector<uint8_t> data;
        if (!recv(msg, data, 500)) break;
        if (msg.command == A_CLSE) break;
    }
    printf("ADB: Channel %u/%u closed\n", local_id, remote_id);
}
