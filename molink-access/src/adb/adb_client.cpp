#include "adb_client.h"
#include "log.h"
#include <cstdio>
#include <chrono>

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
    return ::loadOrGenerateKey(*m_rsa);
}

bool AdbClient::connect(const std::string& serial) {
    if (m_connected) return true;

    m_devices = UsbDevice::discover();
    if (m_devices.empty()) {
        LOG_ERROR("ADB", "no device found");
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

    if (!selected->prepare()) return false;

    m_serial = selected->getSerial();
    LOG_INFO("ADB", "device %s opened", m_serial.empty() ? "(no serial)" : m_serial.c_str());

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
        LOG_INFO("ADB", "handshake attempt %d", attempt + 1);
        if (m_transport->handshake(*m_rsa, banner)) {
            m_selectedDev = selected;
            m_connected = true;
            m_state = DaemonState::CONNECTED;
            m_reader.start(m_transport.get(), &m_channels, &m_pending,
                          &m_channelMutex, &m_writeMutex);

            if (m_hDisconnectEvent) {
                m_reader.setDisconnectCallback([this]() {
                    SetEvent(m_hDisconnectEvent);
                });
            }

            LOG_INFO("ADB", "connected");
            return true;
        }

        if (attempt < 2) {
            LOG_INFO("ADB", "retrying in 2s");
            selected->close();
            Sleep(2000);
            if (!selected->prepare()) {
                LOG_ERROR("ADB", "cannot re-open device");
                return false;
            }
        }
    }

    LOG_ERROR("ADB", "handshake failed after 3 attempts");
    selected->close();
    return false;
}

void AdbClient::disconnect() {
    m_reader.stop();

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
    m_state = DaemonState::DISCONNECTED;
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

    {
        std::lock_guard<std::mutex> lock(m_channelMutex);
        m_pending[localId] = po;
    }

    {
        std::lock_guard<std::mutex> wLock(m_writeMutex);
        m_transport->send(A_OPEN, localId, 0,
                         destination.c_str(),
                         (uint32_t)destination.size() + 1);
    }

    uint32_t remoteId = 0;
    bool error = false;
    {
        std::unique_lock<std::mutex> poLock(po->mtx);
        // 超时保护：adbd 挂起不应答 A_OPEN 时，不能把调用方（daemon 主线程）
        // 永久卡死。超时后 remoteId 保持 0，走下方 error/remoteId==0 分支
        // 返回 nullptr，调用方按打开失败处理。
        po->cv.wait_for(poLock, std::chrono::seconds(30), [&] { return po->done; });
        remoteId = po->remoteId;
        error = po->error;
    }

    {
        std::lock_guard<std::mutex> lock(m_channelMutex);
        m_pending.erase(localId);
    }

    if (error || remoteId == 0) {
        LOG_ERROR("ADB", "failed to open channel to %s", destination.c_str());
        return nullptr;
    }

    auto ch = std::make_shared<Channel>();
    ch->localId = localId;
    ch->remoteId = remoteId;

    {
        std::lock_guard<std::mutex> lock(m_channelMutex);
        m_channels[localId] = ch;
    }

    LOG_INFO("ADB", "channel %u opened to %s remote=%u",
             localId, destination.c_str(), remoteId);
    return ch;
}

void AdbClient::closeChannel(ChannelPtr ch) {
    if (!ch || !m_transport) return;

    {
        std::lock_guard<std::mutex> chLock(ch->mtx);
        ch->draining = true;
    }

    {
        std::lock_guard<std::mutex> lock(m_channelMutex);
        m_channels.erase(ch->localId);
    }

    {
        std::lock_guard<std::mutex> wLock(m_writeMutex);
        m_transport->closeChannel(ch->localId, ch->remoteId);
    }

    LOG_INFO("ADB", "channel %u/%u closed", ch->localId, ch->remoteId);
}

bool AdbClient::writeChannel(ChannelPtr ch, const void* data, uint32_t len) {
    if (!m_transport || !ch) return false;
    std::lock_guard<std::mutex> lock(m_writeMutex);
    return m_transport->writeChannel(ch->localId, ch->remoteId, data, len);
}
