#include "cli_utils.h"
#include "../adb/adb_client.h"
#include "../adb/adb_rsa.h"
#include "../usb/usb_device.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <sstream>
#include <windows.h>
#include <io.h>

// ---- 数据结构 ----
struct DevResult {
    std::string serial;
    std::string auth;
};

// ---- 展示 ----
static void printDeviceTable(const std::vector<DevResult>& results) {
    if (results.empty()) {
        printf("\nNo ADB devices found.\n");
        return;
    }
    printf("\n%-4s %-22s %s\n", "#", "SERIAL", "AUTH");
    printf("%-4s %-22s %s\n", "---", "----------------------", "----");
    for (size_t i = 0; i < results.size(); i++) {
        printf("%-4zu %-22s %s\n", i,
               results[i].serial.empty() ? "-" : results[i].serial.c_str(),
               results[i].auth.c_str());
    }
}

// ---- 通过 daemon pipe 查询 ----
static bool queryViaDaemon(std::vector<DevResult>& results) {
    auto resp = sendPipeCmd("devices");
    if (resp.empty() || resp == "unknown") return false;

    std::istringstream ss(resp);
    std::string line;
    while (std::getline(ss, line)) {
        if (line.empty() || line == "(none)") continue;
        if (!line.empty() && line.back() == '\r') line.pop_back();

        std::istringstream ls(line);
        DevResult r;
        ls >> r.serial >> r.auth;
        if (r.serial == "-") r.serial.clear();
        if (r.auth.empty()) r.auth = "-";
        results.push_back(r);
    }
    return true;
}

// ---- 直接 USB 扫描 + 握手 ----
static void queryViaUsb(std::vector<DevResult>& results) {
    fflush(stdout);
    int saved = _dup(1);
    freopen("NUL", "w", stdout);

    auto devices = UsbDevice::discover();

    AdbRsa rsa;
    std::string kp = getDefaultKeyPath();
    bool keyReady = false;
    if (rsa.loadKey(kp)) {
        keyReady = true;
    } else {
        if (rsa.generateKey()) {
            rsa.saveKey(kp);
            keyReady = true;
        }
    }

    for (auto& dev : devices) {
        DevResult r;
        if (!dev.open()) { results.push_back(r); continue; }
        dev.clearHalt(dev.getReadEndpoint());
        dev.clearHalt(dev.getWriteEndpoint());
        dev.drainRead();
        r.serial = dev.getSerial();

        r.auth = "no";
        if (keyReady) {
            AdbTransport tr(dev);
            uint32_t maxdata = MAX_PAYLOAD_V2;
            tr.send(A_CNXN, A_VERSION, maxdata, "host::", 5);
            AdbMessage msg;
            std::vector<uint8_t> data;
            if (tr.recv(msg, data, 5000)) {
                if (msg.command == A_CNXN) {
                    r.auth = "yes";
                } else if (msg.command == A_AUTH && msg.arg0 == AUTH_TOKEN) {
                    auto sig = rsa.signToken(data.data(), data.size());
                    if (!sig.empty()) {
                        tr.send(A_AUTH, AUTH_SIGNATURE, 0, sig.data(), (uint32_t)sig.size());
                        if (tr.recv(msg, data, 2000)) {
                            if (msg.command == A_CNXN) r.auth = "yes";
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
}

// ---- devices 命令入口 ----
int cmdDevices() {
    std::string keyPath = getDefaultKeyPath();
    printf("Key: %s (%s)\n", keyPath.c_str(),
           (GetFileAttributesA(keyPath.c_str()) != INVALID_FILE_ATTRIBUTES) ? "found" : "missing");

    std::vector<DevResult> results;

    if (!queryViaDaemon(results)) {
        queryViaUsb(results);
    }

    printDeviceTable(results);
    return results.empty() ? 1 : 0;
}
