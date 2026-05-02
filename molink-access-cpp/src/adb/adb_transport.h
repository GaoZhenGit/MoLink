#ifndef ADB_TRANSPORT_H
#define ADB_TRANSPORT_H

#include <cstdint>
#include <vector>
#include <string>

class UsbDevice;
class AdbRsa;

constexpr uint32_t A_CNXN = 0x4e584e43;
constexpr uint32_t A_AUTH = 0x48545541;
constexpr uint32_t A_OPEN = 0x4e45504f;
constexpr uint32_t A_OKAY = 0x59414b4f;
constexpr uint32_t A_CLSE = 0x45534c43;
constexpr uint32_t A_WRTE = 0x45545257;

constexpr uint32_t A_VERSION = 0x01000001;
constexpr uint32_t MAX_PAYLOAD_V2 = 0x00100000;  // 1MB, 与 adb.exe 一致

constexpr uint32_t AUTH_TOKEN = 1;
constexpr uint32_t AUTH_SIGNATURE = 2;
constexpr uint32_t AUTH_RSAPUBLICKEY = 3;

#pragma pack(push, 1)
struct AdbMessage {
    uint32_t command;
    uint32_t arg0;
    uint32_t arg1;
    uint32_t data_length;
    uint32_t data_crc32;
    uint32_t magic;

    AdbMessage()
        : command(0), arg0(0), arg1(0), data_length(0), data_crc32(0), magic(0) {}

    AdbMessage(uint32_t cmd, uint32_t a0, uint32_t a1, uint32_t len, uint32_t crc)
        : command(cmd), arg0(a0), arg1(a1), data_length(len), data_crc32(crc)
        , magic(cmd ^ 0xFFFFFFFF) {}
};
#pragma pack(pop)

class AdbTransport {
public:
    explicit AdbTransport(UsbDevice& device);

    bool send(uint32_t cmd, uint32_t arg0, uint32_t arg1,
              const void* data, uint32_t data_len);
    bool recv(AdbMessage& msg, std::vector<uint8_t>& data, uint32_t timeout_ms = 5000);
    bool handshake(const std::string& banner = "host::");
    bool handshake(AdbRsa& rsa, const std::string& banner = "host::");

    uint32_t openChannel(const std::string& destination, uint32_t local_id);
    void closeChannel(uint32_t local_id, uint32_t remote_id);

    const std::vector<uint8_t>& getAuthToken() const { return m_auth_token; }
    static uint32_t checksum(const uint8_t* data, uint32_t len) {
        uint32_t sum = 0;
        for (uint32_t i = 0; i < len; i++) sum += data[i];
        return sum;
    }

private:
    UsbDevice& m_device;
    std::vector<uint8_t> m_auth_token;

    bool readExact(void* buf, uint32_t len, uint32_t timeout_ms);
    bool writeExact(const void* buf, uint32_t len, uint32_t timeout_ms);
    static uint32_t crc32(const uint8_t* data, uint32_t len);
};

#endif
