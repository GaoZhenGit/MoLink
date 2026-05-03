#include "usb_device.h"
#include <libusb.h>
#include <cstdio>

std::vector<UsbDevice> UsbDevice::discover() {
    std::vector<UsbDevice> result;

    libusb_context* ctx = nullptr;
    int ret = libusb_init(&ctx);
    if (ret != 0) {
        printf("USB: libusb_init failed: %s\n", libusb_error_name(ret));
        return result;
    }

    libusb_device** devs = nullptr;
    ssize_t count = libusb_get_device_list(ctx, &devs);
    if (count < 0) {
        printf("USB: libusb_get_device_list failed: %s\n", libusb_error_name((int)count));
        libusb_exit(ctx);
        return result;
    }

    printf("USB: Scanning %zd USB devices...\n", count);
    for (ssize_t i = 0; i < count; i++) {
        libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(devs[i], &desc) != 0) continue;

        libusb_config_descriptor* config = nullptr;
        if (libusb_get_active_config_descriptor(devs[i], &config) != 0) continue;

        bool found = false;
        int adb_iface = 0;
        uint8_t read_ep = 0, write_ep = 0;

        for (uint8_t j = 0; j < config->bNumInterfaces && !found; j++) {
            const libusb_interface& iface = config->interface[j];
            for (int k = 0; k < iface.num_altsetting && !found; k++) {
                const libusb_interface_descriptor& iface_desc = iface.altsetting[k];
                if (iface_desc.bInterfaceClass == 0xFF &&
                    iface_desc.bInterfaceSubClass == 0x42 &&
                    iface_desc.bInterfaceProtocol == 0x01) {
                    adb_iface = iface_desc.bInterfaceNumber;
                    for (uint8_t ep = 0; ep < iface_desc.bNumEndpoints; ep++) {
                        const libusb_endpoint_descriptor& ep_desc = iface_desc.endpoint[ep];
                        uint8_t addr = ep_desc.bEndpointAddress;
                        if ((addr & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_IN) {
                            read_ep = addr;
                        } else {
                            write_ep = addr;
                        }
                    }
                    found = true;
                }
            }
        }

        libusb_free_config_descriptor(config);

        if (found && read_ep != 0 && write_ep != 0) {
            printf("USB: Found ADB device [%zd]: VID=0x%04X PID=0x%04X "
                   "iface=%d read_ep=0x%02X write_ep=0x%02X\n",
                   i, desc.idVendor, desc.idProduct,
                   adb_iface, read_ep, write_ep);
            UsbDevice dev(devs[i], ctx, adb_iface, read_ep, write_ep);
            result.push_back(dev);
        }
    }

    libusb_free_device_list(devs, 0);
    printf("USB: Found %zu ADB device(s)\n", result.size());
    return result;
}

UsbDevice::UsbDevice(libusb_device* dev, libusb_context* ctx,
                     int iface, uint8_t read_ep, uint8_t write_ep)
    : m_device(dev), m_handle(nullptr), m_ctx(ctx)
    , m_read_ep(read_ep), m_write_ep(write_ep)
    , m_interface_number(iface), m_open(false) {}

UsbDevice::~UsbDevice() { close(); }

bool UsbDevice::open() {
    if (m_open) return true;

    int ret = libusb_open(m_device, &m_handle);
    if (ret != 0) {
        printf("USB: libusb_open failed: %s\n", libusb_error_name(ret));
        return false;
    }

    int config = 0;
    libusb_get_configuration(m_handle, &config);
    if (config != 1) {
        ret = libusb_set_configuration(m_handle, 1);
        if (ret != 0) {
            printf("USB: libusb_set_configuration(1) failed: %s\n", libusb_error_name(ret));
        }
    }

    // Windows: 尝试 detach kernel driver（WinUSB 替换驱动后通常没有）
    libusb_detach_kernel_driver(m_handle, m_interface_number);

    ret = libusb_claim_interface(m_handle, m_interface_number);
    if (ret != 0) {
        printf("USB: libusb_claim_interface(%d) failed: %s\n",
               m_interface_number, libusb_error_name(ret));
        if (ret == LIBUSB_ERROR_NOT_SUPPORTED || ret == LIBUSB_ERROR_BUSY) {
            printf("USB: Hint: Use Zadig to replace driver of ADB "
                   "interface with WinUSB\n");
        }
        libusb_close(m_handle);
        m_handle = nullptr;
        return false;
    }

    m_open = true;
    printf("USB: Device opened, interface %d claimed (read=0x%02X write=0x%02X)\n",
           m_interface_number, m_read_ep, m_write_ep);
    return true;
}

void UsbDevice::drainRead() {
    if (!m_handle) return;
    uint8_t buf[256];
    int got = 0;
    int drained = 0;
    while (bulkRead(buf, sizeof(buf), &got, 200))
        drained += got;
    if (drained > 0) printf("USB: drained %d bytes stale data\n", drained);
}

bool UsbDevice::clearHalt(uint8_t ep) {
    if (!m_handle) return false;
    int ret = libusb_clear_halt(m_handle, ep);
    if (ret != 0) {
        printf("USB: clear_halt(0x%02X) failed: %s\n", ep, libusb_error_name(ret));
        return false;
    }
    printf("USB: clear_halt(0x%02X) OK\n", ep);
    return true;
}

std::string UsbDevice::getSerial() const {
    if (!m_handle) return "";

    libusb_device_descriptor desc;
    if (libusb_get_device_descriptor(m_device, &desc) != 0) return "";
    if (desc.iSerialNumber == 0) return "";

    unsigned char buf[256] = {0};
    int len = libusb_get_string_descriptor_ascii(
        m_handle, desc.iSerialNumber, buf, sizeof(buf));
    if (len < 0) return "";

    return std::string((char*)buf, len);
}

bool UsbDevice::bulkRead(void* buf, int len, int* transferred, int timeout_ms) {
    int ret = libusb_bulk_transfer(m_handle, m_read_ep,
                                   (unsigned char*)buf, len,
                                   transferred, timeout_ms);
    if (ret < 0) {
        if (ret != LIBUSB_ERROR_TIMEOUT) {
            printf("USB: bulkRead error: %s\n", libusb_error_name(ret));
        }
        return false;
    }
    return true;
}

bool UsbDevice::bulkWrite(const void* buf, int len, int* transferred, int timeout_ms) {
    int ret = libusb_bulk_transfer(m_handle, m_write_ep,
                                   (unsigned char*)buf, len,
                                   transferred, timeout_ms);
    if (ret < 0) {
        printf("USB: bulkWrite error: %s\n", libusb_error_name(ret));
        return false;
    }
    return true;
}

void UsbDevice::close() {
    if (m_handle) {
        libusb_release_interface(m_handle, m_interface_number);
        libusb_close(m_handle);
        m_handle = nullptr;
    }
    m_open = false;
}
