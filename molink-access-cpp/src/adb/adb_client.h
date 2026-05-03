#ifndef ADB_CLIENT_H
#define ADB_CLIENT_H

#include "adb_transport.h"
#include "adb_rsa.h"
#include "../usb/usb_device.h"
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <cstdint>

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

    struct Channel {
        uint32_t localId = 0;
        uint32_t remoteId = 0;
        bool valid() const { return remoteId != 0; }
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
    Channel openChannel(const std::string& destination);
    void closeChannel(const Channel& ch);
    bool readChannel(const Channel& ch, std::vector<uint8_t>& data,
                     uint32_t timeoutMs = 5000);
    bool writeChannel(const Channel& ch, const void* data, uint32_t len);

    // 底层 transport
    AdbTransport* getTransport() { return m_transport.get(); }
    std::mutex& getWriteMutex() { return m_writeMutex; }

private:
    std::vector<UsbDevice> m_devices;       // 持有设备列表，保证指针有效
    UsbDevice* m_selectedDev = nullptr;     // 当前使用的设备
    std::unique_ptr<AdbTransport> m_transport;
    std::unique_ptr<AdbRsa> m_rsa;
    std::string m_serial;
    bool m_connected = false;
    uint32_t m_nextLocalId = 1;
    std::mutex m_writeMutex;

    bool loadOrGenerateKey();
};

#endif
