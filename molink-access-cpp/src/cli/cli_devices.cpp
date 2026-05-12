#include "cli_utils.h"
#include "../adb/adb_client.h"
#include "../adb/adb_rsa.h"
#include "../usb/usb_device.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <windows.h>
#include <shlobj.h>
#include <io.h>

// ---- devices 命令 ----
int cmdDevices() {
    char appdata[MAX_PATH] = {};
    if (SHGetFolderPathA(nullptr, CSIDL_PROFILE, nullptr, 0, appdata) == S_OK) {
        printf("Keys: %s\\.android\n", appdata);
        auto exists = [](const std::string& p) {
            return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES;
        };
        std::string base = std::string(appdata) + "\\.android\\";
        printf("  molink_key.bin : %s\n", exists(base + "molink_key.bin") ? "found" : "missing");
        printf("  adbkey         : %s\n", exists(base + "adbkey") ? "found" : "missing");
        printf("  adbkey.pub     : %s\n", exists(base + "adbkey.pub") ? "found" : "missing");
    }

    fflush(stdout);
    int saved = _dup(1);
    freopen("NUL", "w", stdout);

    auto devices = UsbDevice::discover();

    AdbRsa rsa;
    bool keyReady = false;
    if (appdata[0]) {
        std::string kp = std::string(appdata) + "\\.android\\molink_key.bin";
        if (!rsa.loadKey(kp)) {
            std::string ak = std::string(appdata) + "\\.android\\adbkey";
            if (rsa.loadPkcs8(ak)) keyReady = true;
        } else keyReady = true;
    }

    struct DevResult { std::string serial; bool authorized; };
    std::vector<DevResult> results;

    for (auto& dev : devices) {
        DevResult r;
        if (!dev.open()) { results.push_back(r); continue; }
        dev.clearHalt(dev.getReadEndpoint());
        dev.clearHalt(dev.getWriteEndpoint());
        dev.drainRead();
        r.serial = dev.getSerial();

        r.authorized = false;
        if (keyReady) {
            AdbTransport tr(dev);
            uint32_t maxdata = MAX_PAYLOAD_V2;
            tr.send(A_CNXN, A_VERSION, maxdata, "host::", 5);
            AdbMessage msg;
            std::vector<uint8_t> data;
            if (tr.recv(msg, data, 5000)) {
                if (msg.command == A_CNXN) {
                    r.authorized = true;
                } else if (msg.command == A_AUTH && msg.arg0 == AUTH_TOKEN) {
                    auto sig = rsa.signToken(data.data(), data.size());
                    if (!sig.empty()) {
                        tr.send(A_AUTH, AUTH_SIGNATURE, 0, sig.data(), (uint32_t)sig.size());
                        if (tr.recv(msg, data, 2000)) {
                            if (msg.command == A_CNXN) r.authorized = true;
                        }
                    }
                }
            }
        }

        dev.close();
        results.push_back(r);
    }

    fflush(stdout);
    _dup2(saved, 1);
    _close(saved);

    if (devices.empty()) { printf("No ADB devices found.\n"); return 1; }
    printf("\n%-4s %-22s %s\n", "#", "SERIAL", "AUTH");
    printf("%-4s %-22s %s\n", "---", "----------------------", "----");
    for (size_t i = 0; i < results.size(); i++) {
        printf("%-4zu %-22s %s\n", i,
               results[i].serial.empty() ? "-" : results[i].serial.c_str(),
               results[i].serial.empty() ? "-" : (results[i].authorized ? "yes" : "no"));
    }
    return 0;
}
