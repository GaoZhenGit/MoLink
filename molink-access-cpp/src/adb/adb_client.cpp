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

    m_devices = UsbDevice::discover();
    if (m_devices.empty()) {
        printf("ADB: No device found\n");
        return false;
    }

    UsbDevice* selected = nullptr;
    for (auto& dev : m_devices) {
        if (serial.empty() || dev.getSerial() == serial) {
            selected = &dev;
            break;
        }
    }
    if (!selected) selected = &m_devices[0];

    if (!selected->open()) return false;

    selected->clearHalt(selected->getReadEndpoint());
    selected->clearHalt(selected->getWriteEndpoint());
    selected->drainRead();

    m_serial = selected->getSerial();
    printf("ADB: Device %s opened\n", m_serial.empty() ? "(no serial)" : m_serial.c_str());

    if (!loadOrGenerateKey()) {
        selected->close();
        return false;
    }

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
            m_reader.start(m_transport.get(), &m_channels, &m_pending,
                          &m_channelMutex, &m_writeMutex);
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
    m_reader.stop();

    // 唤醒所有 pending open
    {
        std::lock_guard<std::mutex> lock(m_channelMutex);
        for (auto& pair : m_pending) {
            {
                std::lock_guard<std::mutex> poLock(pair.second->mtx);
                pair.second->error = true;
                pair.second->done = true;
            }
            pair.second->cv.notify_one();
        }
    }

    // 清空所有通道
    {
        std::lock_guard<std::mutex> lock(m_channelMutex);
        for (auto& pair : m_channels) {
            std::lock_guard<std::mutex> chLock(pair.second->mtx);
            pair.second->closed = true;
            pair.second->draining = true;
        }
        m_channels.clear();
    }

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

int AdbClient::getActiveChannelCount() const {
    std::lock_guard<std::mutex> lock(m_channelMutex);
    return (int)m_channels.size();
}

// ---- Channel Management ----

ChannelPtr AdbClient::openChannel(const std::string& destination) {
    if (!m_connected || !m_transport) return nullptr;

    uint32_t localId = m_nextLocalId++;
    auto po = std::make_shared<PendingOpen>();

    // 先注册 pending，再发送 A_OPEN（避免响应比注册先到）
    {
        std::lock_guard<std::mutex> lock(m_channelMutex);
        m_pending[localId] = po;
    }

    // 发送 A_OPEN 不自己读 recv（AdbReader 负责分发响应）
    {
        std::lock_guard<std::mutex> wLock(m_writeMutex);
        m_transport->send(A_OPEN, localId, 0,
                         destination.c_str(),
                         (uint32_t)destination.size() + 1);
    }

    // 等待 AdbReader 收到响应
    uint32_t remoteId = 0;
    bool error = false;
    {
        std::unique_lock<std::mutex> poLock(po->mtx);
        po->cv.wait(poLock, [&] { return po->done; });
        remoteId = po->remoteId;
        error = po->error;
    }

    // 移除 pending
    {
        std::lock_guard<std::mutex> lock(m_channelMutex);
        m_pending.erase(localId);
    }

    if (error || remoteId == 0) {
        printf("ADB: Failed to open channel to %s\n", destination.c_str());
        return nullptr;
    }

    auto ch = std::make_shared<Channel>();
    ch->localId = localId;
    ch->remoteId = remoteId;

    {
        std::lock_guard<std::mutex> lock(m_channelMutex);
        m_channels[localId] = ch;
    }

    printf("ADB: Channel %u opened to %s (remote=%u)\n",
           localId, destination.c_str(), remoteId);
    return ch;
}

void AdbClient::closeChannel(ChannelPtr ch) {
    if (!ch || !m_transport) return;

    // 标记 draining，阻止 AdbReader 继续往队列推数据
    {
        std::lock_guard<std::mutex> chLock(ch->mtx);
        ch->draining = true;
    }

    // 从 map 移除（AdbReader 之后读到的消息会丢弃）
    {
        std::lock_guard<std::mutex> lock(m_channelMutex);
        m_channels.erase(ch->localId);
    }

    // 发送 A_CLSE
    {
        std::lock_guard<std::mutex> wLock(m_writeMutex);
        m_transport->closeChannel(ch->localId, ch->remoteId);
    }

    printf("ADB: Channel %u/%u closed\n", ch->localId, ch->remoteId);
}

bool AdbClient::writeChannel(ChannelPtr ch, const void* data, uint32_t len) {
    if (!m_transport || !ch) return false;
    std::lock_guard<std::mutex> lock(m_writeMutex);
    return m_transport->writeChannel(ch->localId, ch->remoteId, data, len);
}
