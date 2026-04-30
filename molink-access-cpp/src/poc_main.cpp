#include <cstdio>
#include <vector>
#include <string>
#include <windows.h>
#include <shlobj.h>
#include "usb/usb_device.h"
#include "adb/adb_transport.h"
#include "adb/adb_rsa.h"

static std::string getKeyPath() {
    char appdata[MAX_PATH] = {};
    if (SHGetFolderPathA(nullptr, CSIDL_PROFILE, nullptr, 0, appdata) == S_OK) {
        return std::string(appdata) + "\\.android\\molink_key.bin";
    }
    return "molink_key.bin";
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);

    printf("=== MoLink POC: libusb + RSA ===\n");

    // 1. Discover
    printf("\n--- Step 1: Discover ---\n");
    std::vector<UsbDevice> devices = UsbDevice::discover();
    if (devices.empty()) {
        printf("FAIL: No ADB device found\n");
        return 1;
    }

    // 2. Open
    printf("\n--- Step 2: Open ---\n");
    UsbDevice& dev = devices[0];
    if (!dev.open()) {
        printf("FAIL: Cannot open device\n");
        return 1;
    }

    // 3. Test read
    printf("\n--- Step 3: Test read pipe ---\n");
    {
        unsigned char dummy[64];
        int got = 0;
        if (dev.bulkRead(dummy, sizeof(dummy), &got, 500)) {
            printf("Pre-read got %d bytes (unexpected stale data)\n", got);
        } else {
            printf("Pre-read timeout (pipe is clean, expected)\n");
        }
    }

    // 4. Serial
    printf("\n--- Step 4: Serial ---\n");
    std::string serial = dev.getSerial();
    printf("Serial: %s\n", serial.empty() ? "(unknown)" : serial.c_str());

    // 5. RSA key
    printf("\n--- Step 5: RSA Key ---\n");
    AdbRsa rsa;
    std::string keyPath = getKeyPath();
    if (!rsa.loadKey(keyPath)) {
        printf("Generating new RSA key pair...\n");
        if (!rsa.generateKey()) {
            printf("FAIL: Cannot generate RSA key\n");
            dev.close();
            return 1;
        }
        rsa.saveKey(keyPath);
    }

    // 6. Handshake (may need retry after device accepts public key and resets)
    printf("\n--- Step 6: Handshake ---\n");
    bool connected = false;
    for (int attempt = 0; attempt < 3; attempt++) {
        printf("\n*** Handshake attempt %d ***\n", attempt + 1);
        AdbTransport transport(dev);
        if (transport.handshake(rsa)) {
            printf("Handshake: CONNECTED!\n");
            connected = true;

            // 7. Open channel
            printf("\n--- Step 7: Open Channel ---\n");
            uint32_t remote_id = transport.openChannel("tcp:1080", 1);
            if (remote_id > 0) {
                printf("OK: Channel to tcp:1080 opened! remote_id=%u\n", remote_id);
                transport.closeChannel(1, remote_id);
            }
            break;
        }

        printf("Handshake attempt %d failed. Device may need to accept key.\n", attempt + 1);
        if (attempt < 2) {
            printf("Waiting 3s, then re-opening device...\n");
            dev.close();
            Sleep(3000);
            if (!dev.open()) {
                printf("FAIL: Cannot re-open device\n");
                return 1;
            }
        }
    }

    dev.close();
    if (connected) {
        printf("\n=== POC SUCCESS ===\n");
        return 0;
    } else {
        printf("\n=== POC: Handshake incomplete ===\n");
        return 1;
    }
}
