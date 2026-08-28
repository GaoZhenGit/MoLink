#ifndef ADB_CLIENT_H
#define ADB_CLIENT_H

#include "adb_transport.h"
#include "adb_rsa.h"
#include "adb_reader.h"
#include "../usb/usb_device.h"
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <atomic>
#include <cstdint>
#include <windows.h>

enum class DaemonState {
    CONNECTED,
    DISCONNECTED,
    STOPPING
};

class AdbClient {
public:
    struct DeviceInfo {
        std::string serial;
        uint16_t vid;
        uint16_t pid;
        uint8_t readEp;
        uint8_t writeEp;
        int interfaceNumber;
    };

    AdbClient();
    ~AdbClient();

    // 设备发现
    static std::vector<DeviceInfo> discover();

    // 连接管理
    bool connect(const std::string& serial = "");
    void disconnect();
    bool isConnected() const { return m_connected; }
    std::string getSerial() const { return m_serial; }

    // 通道 I/O（线程安全）
    ChannelPtr openChannel(const std::string& destination);
    void closeChannel(ChannelPtr ch);
    bool writeChannel(ChannelPtr ch, const void* data, uint32_t len);

    // 线程安全
    std::mutex& getWriteMutex() { return m_writeMutex; }

    // 状态查询
    int getActiveChannelCount() const;

    // 热插拔
    void setDisconnectEvent(HANDLE h) { m_hDisconnectEvent = h; }
    DaemonState getState() const { return m_state; }
    void setState(DaemonState s) { m_state = s; }

private:
    std::vector<UsbDevice> m_devices;
    UsbDevice* m_selectedDev = nullptr;
    std::unique_ptr<AdbTransport> m_transport;
    std::unique_ptr<AdbRsa> m_rsa;
    AdbReader m_reader;
    ChannelMap m_channels;
    PendingMap m_pending;
    mutable std::mutex m_channelMutex;
    std::string m_serial;
    bool m_connected = false;
    // openChannel 会被 daemon 主线程和 forwarder 的 relay 线程并发调用，
    // 必须原子自增，否则两个线程可能拿到相同 localId 互相覆盖 pending。
    std::atomic<uint32_t> m_nextLocalId{1};
    std::mutex m_writeMutex;
    HANDLE m_hDisconnectEvent = nullptr;
    DaemonState m_state = DaemonState::CONNECTED;

    bool loadOrGenerateKey();
};

#endif
