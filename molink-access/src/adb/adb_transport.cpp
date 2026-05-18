#include "adb_transport.h"
#include "adb_rsa.h"
#include "../usb/usb_device.h"
#include "log.h"
#include <cstring>

AdbTransport::AdbTransport(UsbDevice& device)
    : m_device(device) {}

bool AdbTransport::hadFatalError() const {
    return m_device.isLastErrorFatal();
}

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
    uint32_t sum = 0;
    if (data && data_len > 0) {
        const uint8_t* p = (const uint8_t*)data;
        for (uint32_t i = 0; i < data_len; i++) sum += p[i];
    }
    AdbMessage msg(cmd, arg0, arg1, data_len, sum);

    if (!writeExact(&msg, sizeof(msg), 5000)) {
        LOG_ERROR("ADB", "send header failed");
        return false;
    }
    if (data && data_len > 0) {
        if (!writeExact(data, data_len, 5000)) {
            LOG_ERROR("ADB", "send data failed");
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
        LOG_ERROR("ADB", "bad magic got 0x%08X expect 0x%08X",
                  msg.magic, msg.command ^ 0xFFFFFFFF);
        m_protocolError = true;
        return false;
    }

    if (msg.data_length > MAX_PAYLOAD_V2) {
        LOG_ERROR("ADB", "payload too large: %u", msg.data_length);
        m_protocolError = true;
        return false;
    }

    if (msg.data_length > 0) {
        LOG_DEBUG("ADB", "recv %u bytes", msg.data_length);
        data.resize(msg.data_length);
        const uint32_t kPayloadTimeout = 30000;
        if (!readExact(data.data(), msg.data_length, kPayloadTimeout))
            return false;
    }
    return true;
}

bool AdbTransport::handshake(const std::string& banner) {
    uint32_t maxdata = MAX_PAYLOAD_V2;
    if (!send(A_CNXN, A_VERSION, maxdata, banner.c_str(), (uint32_t)banner.size())) {
        LOG_ERROR("ADB", "send A_CNXN failed");
        return false;
    }

    AdbMessage msg;
    std::vector<uint8_t> data;
    if (!recv(msg, data, 10000)) {
        LOG_ERROR("ADB", "recv after A_CNXN failed");
        return false;
    }

    if (msg.command == A_CNXN) {
        LOG_INFO("ADB", "connected, max=%u banner=%.*s",
                 msg.arg1, msg.data_length, (char*)data.data());
        return true;
    }

    if (msg.command == A_AUTH) {
        uint32_t auth_type = msg.arg0;
        LOG_DEBUG("ADB", "A_AUTH type=%u token=%u bytes",
                  auth_type, (uint32_t)data.size());
        m_auth_token = data;
        return false;
    }

    LOG_ERROR("ADB", "unexpected response 0x%08X", msg.command);
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
        LOG_ERROR("ADB", "expected A_OKAY got 0x%08X", msg.command);
        return 0;
    }

    LOG_INFO("ADB", "channel %u opened to %s remote=%u",
             local_id, destination.c_str(), msg.arg0);
    return msg.arg0;
}

bool AdbTransport::handshake(AdbRsa& rsa, const std::string& banner) {
    uint32_t maxdata = MAX_PAYLOAD_V2;
    if (!send(A_CNXN, A_VERSION, maxdata, banner.c_str(), (uint32_t)banner.size())) {
        LOG_ERROR("ADB", "send A_CNXN failed");
        return false;
    }

    bool waiting_for_authorization = false;

    for (int round = 0; round < 3; round++) {
        AdbMessage msg;
        std::vector<uint8_t> data;
        uint32_t timeout = waiting_for_authorization ? 120000 : 10000;
        LOG_DEBUG("ADB", "handshake round %d timeout=%u ms", round, timeout);
        if (!recv(msg, data, timeout)) {
            LOG_ERROR("ADB", "recv after A_CNXN (round %d)", round);
            return false;
        }

        if (msg.command == A_CNXN) {
            LOG_INFO("ADB", "connected, max=%u banner=%.*s",
                     msg.arg1, (int)data.size(), (char*)data.data());
            return true;
        }

        if (msg.command == A_AUTH && msg.arg0 == AUTH_TOKEN) {
            if (waiting_for_authorization) {
                LOG_DEBUG("ADB", "device requested public key");
                std::string payload = rsa.getPubKeyPayload();
                if (payload.empty()) {
                    LOG_ERROR("ADB", "cannot get public key payload");
                    return false;
                }
                if (!send(A_AUTH, AUTH_RSAPUBLICKEY, 0, payload.data(), (uint32_t)payload.size())) {
                    LOG_ERROR("ADB", "send AUTH_RSAPUBLICKEY failed");
                    return false;
                }
                LOG_DEBUG("ADB", "public key sent (%zu bytes round %d)", payload.size(), round);
            } else {
                LOG_DEBUG("ADB", "signing token (%u bytes)", (uint32_t)data.size());
                auto sig = rsa.signToken(data.data(), data.size());
                if (sig.empty()) {
                    LOG_ERROR("ADB", "RSA signing failed");
                    return false;
                }
                if (!send(A_AUTH, AUTH_SIGNATURE, 0, sig.data(), (uint32_t)sig.size())) {
                    LOG_ERROR("ADB", "send AUTH_SIGNATURE failed");
                    return false;
                }
                LOG_DEBUG("ADB", "signature sent (%u bytes)", (uint32_t)sig.size());
                waiting_for_authorization = true;
            }
        } else {
            LOG_ERROR("ADB", "unexpected response 0x%08X arg0=%u", msg.command, msg.arg0);
            return false;
        }
    }

    LOG_ERROR("ADB", "handshake loop exhausted");
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
            LOG_WARN("ADB", "channel %u/%u closed by device", local_id, remote_id);
            data.clear();
            return false;
        }
        if (msg.command == A_OKAY) {
            continue;
        }
        if (msg.command != A_WRTE) {
            LOG_ERROR("ADB", "unexpected cmd 0x%08X on channel %u/%u",
                      msg.command, local_id, remote_id);
            return false;
        }
        send(A_OKAY, local_id, remote_id, nullptr, 0);
        LOG_DEBUG("ADB", "channel %u/%u read %u bytes",
                  local_id, remote_id, (uint32_t)data.size());
        return true;
    }
}

void AdbTransport::closeChannel(uint32_t local_id, uint32_t remote_id) {
    send(A_CLSE, local_id, remote_id, nullptr, 0);
}
