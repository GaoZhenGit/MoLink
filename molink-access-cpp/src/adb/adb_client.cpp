#include "adb_client.h"
#include <cstdio>
#include <shlobj.h>

// ---- Device Discovery ----

std::vector<AdbClient::DeviceInfo> AdbClient::discover() {
    std::vector<DeviceInfo> result;
    auto devices = UsbDevice::discover();

    for (auto& dev : devices) {
        DeviceInfo info;
        info.serial = dev.getSerial();
        info.readEp = dev.getReadEndpoint();
        info.writeEp = dev.getWriteEndpoint();
        result.push_back(info);
    }

    return result;
}

// ---- Lifecycle ----

AdbClient::AdbClient() {}

AdbClient::~AdbClient() {
    disconnect();
}

bool AdbClient::loadOrGenerateKey() {
    m_rsa.reset(new AdbRsa());

    char appdata[MAX_PATH] = {};
    if (SHGetFolderPathA(nullptr, CSIDL_PROFILE, nullptr, 0, appdata) == S_OK) {
        std::string keyPath = std::string(appdata) + "\\.android\\molink_key.bin";
        if (m_rsa->loadKey(keyPath)) return true;

        std::string adbKeyPath = std::string(appdata) + "\\.android\\adbkey";
        if (m_rsa->loadPkcs8(adbKeyPath)) {
            printf("ADB: Key imported from adbkey\n");
            return true;
        }
    }

    printf("ADB: Generating new RSA key...\n");
    if (!m_rsa->generateKey()) {
        printf("FAIL: RSA key generation failed\n");
        return false;
    }

    if (SHGetFolderPathA(nullptr, CSIDL_PROFILE, nullptr, 0, appdata) == S_OK) {
        std::string keyPath = std::string(appdata) + "\\.android\\molink_key.bin";
        m_rsa->saveKey(keyPath);
    }
    return true;
}

bool AdbClient::connect(const std::string& serial) {
    if (m_connected) return true;

    // 1. 发现设备
    m_devices = UsbDevice::discover();
    if (m_devices.empty()) {
        printf("ADB: No device found\n");
        return false;
    }

    // 2. 选择设备
    UsbDevice* selected = nullptr;
    for (auto& dev : m_devices) {
        if (serial.empty() || dev.getSerial() == serial) {
            selected = &dev;
            break;
        }
    }
    if (!selected) selected = &m_devices[0];

    // 3. 打开设备
    if (!selected->open()) return false;

    // 4. 清除 halt + 排空残留数据
    selected->clearHalt(selected->getReadEndpoint());
    selected->clearHalt(selected->getWriteEndpoint());
    selected->drainRead();

    m_serial = selected->getSerial();
    printf("ADB: Device %s opened\n", m_serial.empty() ? "(no serial)" : m_serial.c_str());

    // 5. 加载 RSA 密钥
    if (!loadOrGenerateKey()) {
        selected->close();
        return false;
    }

    // 6. 握手
    m_transport.reset(new AdbTransport(*selected));

    const char* banner = "host::features=shell_v2,cmd,stat_v2,ls_v2,"
        "fixed_push_mkdir,apex,abb,fixed_push_symlink_timestamp,abb_exec,"
        "remount_shell,track_app,sendrecv_v2,sendrecv_v2_brotli,"
        "sendrecv_v2_lz4,sendrecv_v2_zstd,sendrecv_v2_dry_run_send,"
        "openscreen_mdns";

    for (int attempt = 0; attempt < 3; attempt++) {
        printf("ADB: Handshake attempt %d...\n", attempt + 1);
        if (m_transport->handshake(*m_rsa, banner)) {
            m_selectedDev = selected;
            m_connected = true;
            printf("ADB: Connected\n");
            return true;
        }

        if (attempt < 2) {
            printf("ADB: Retrying in 2s...\n");
            selected->close();
            Sleep(2000);
            if (!selected->open()) {
                printf("FAIL: Cannot re-open device\n");
                return false;
            }
        }
    }

    printf("FAIL: Handshake failed after 3 attempts\n");
    selected->close();
    return false;
}

void AdbClient::disconnect() {
    m_connected = false;
    m_transport.reset();
    m_rsa.reset();
    if (m_selectedDev) {
        m_selectedDev->close();
        m_selectedDev = nullptr;
    }
    m_devices.clear();
    m_serial.clear();
    m_nextLocalId = 1;
}

// ---- Channel Management ----

AdbClient::Channel AdbClient::openChannel(const std::string& destination) {
    Channel ch;
    if (!m_connected || !m_transport) return ch;

    ch.localId = m_nextLocalId++;
    ch.remoteId = m_transport->openChannel(destination, ch.localId);
    if (ch.remoteId == 0) {
        printf("ADB: Failed to open channel to %s\n", destination.c_str());
    }
    return ch;
}

void AdbClient::closeChannel(const Channel& ch) {
    if (m_transport) {
        m_transport->closeChannel(ch.localId, ch.remoteId);
    }
}

bool AdbClient::readChannel(const Channel& ch, std::vector<uint8_t>& data,
                             uint32_t timeoutMs) {
    if (!m_transport) return false;
    return m_transport->readChannel(ch.localId, ch.remoteId, data, timeoutMs);
}

bool AdbClient::writeChannel(const Channel& ch, const void* data, uint32_t len) {
    if (!m_transport) return false;
    std::lock_guard<std::mutex> lock(m_writeMutex);
    return m_transport->writeChannel(ch.localId, ch.remoteId, data, len);
}
