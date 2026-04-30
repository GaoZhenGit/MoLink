#ifndef USB_DEVICE_H
#define USB_DEVICE_H

#include <cstdint>
#include <string>
#include <vector>

struct libusb_device;
struct libusb_device_handle;
struct libusb_context;

class UsbDevice {
public:
    static std::vector<UsbDevice> discover();

    UsbDevice(libusb_device* dev, libusb_context* ctx,
              int iface, uint8_t read_ep, uint8_t write_ep);
    ~UsbDevice();

    bool open();
    std::string getSerial() const;
    uint8_t getReadEndpoint() const  { return m_read_ep; }
    uint8_t getWriteEndpoint() const { return m_write_ep; }
    bool bulkRead(void* buf, int len, int* transferred, int timeout_ms);
    bool bulkWrite(const void* buf, int len, int* transferred, int timeout_ms);
    void close();
    bool isOpen() const { return m_open; }

private:
    libusb_device*       m_device;
    libusb_device_handle* m_handle;
    libusb_context*      m_ctx;
    uint8_t              m_read_ep;
    uint8_t              m_write_ep;
    int                  m_interface_number;
    bool                 m_open;
};

#endif
