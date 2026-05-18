#include <cstdio>
#include <cstring>
#include <vector>
#include <string>
#include <winsock2.h>
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

    // 3. Clear endpoint halts + drain stale data
    printf("\n--- Step 3: Clear halts + drain ---\n");
    dev.clearHalt(dev.getReadEndpoint());
    dev.clearHalt(dev.getWriteEndpoint());
    dev.drainRead();

    // 4. Serial
    printf("\n--- Step 4: Serial ---\n");
    std::string serial = dev.getSerial();
    printf("Serial: %s\n", serial.empty() ? "(unknown)" : serial.c_str());

    // 5. adb.exe 的完整 banner
    std::string fullBanner = "host::features=shell_v2,cmd,stat_v2,ls_v2,"
        "fixed_push_mkdir,apex,abb,fixed_push_symlink_timestamp,abb_exec,"
        "remount_shell,track_app,sendrecv_v2,sendrecv_v2_brotli,"
        "sendrecv_v2_lz4,sendrecv_v2_zstd,sendrecv_v2_dry_run_send,"
        "openscreen_mdns";

    // RSA key — 优先用 adb 已授权密钥（避免重复弹窗）
    printf("\n--- Step 5: RSA Key ---\n");
    AdbRsa rsa;
    std::string keyPath = getKeyPath();
    bool keyReady = false;
    if (rsa.loadKey(keyPath)) {
        keyReady = true;
    } else {
        // 尝试从 adbkey 导入（BCrypt blob，签名可被设备识别）
        char appdata2[MAX_PATH] = {};
        if (SHGetFolderPathA(nullptr, CSIDL_PROFILE, nullptr, 0, appdata2) == S_OK) {
            std::string adbKeyPath = std::string(appdata2) + "\\.android\\adbkey";
            if (rsa.loadPkcs8(adbKeyPath)) {
                printf("RSA: Adb key imported via BCrypt\n");
                keyReady = true;
            }
        }
    }
    if (!keyReady) {
        printf("Generating new RSA key pair (BCrypt)...\n");
        if (!rsa.generateKey()) {
            printf("FAIL: Cannot generate RSA key\n");
            dev.close();
            return 1;
        }
        rsa.saveKey(keyPath);
    }

    // 6. RSA handshake
    printf("\n--- Step 6: RSA Handshake ---\n");
    bool connected = false;
    for (int attempt = 0; attempt < 3; attempt++) {
        printf("\n*** Handshake attempt %d ***\n", attempt + 1);
        AdbTransport transport(dev);
        if (transport.handshake(rsa, fullBanner)) {
            printf("Handshake: CONNECTED!\n");
            connected = true;

            // --- Forward Mode: 本地 TCP → ADB 通道 → 设备 SOCKS5 ---
            printf("\n--- Step 7: Forward Mode ---\n");

            WSADATA wsa;
            WSAStartup(MAKEWORD(2, 2), &wsa);

            SOCKET listen_sock = socket(AF_INET, SOCK_STREAM, 0);
            sockaddr_in addr = {};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = inet_addr("127.0.0.1");
            addr.sin_port = htons(1080);
            bind(listen_sock, (sockaddr*)&addr, sizeof(addr));
            listen(listen_sock, 1);
            printf("Forward: Listening on 127.0.0.1:1080\n");
            printf("Forward: Run: curl --socks5 127.0.0.1:1080 --proxy-user socks5:password123 http://example.com\n");

            SOCKET client = accept(listen_sock, nullptr, nullptr);
            if (client == INVALID_SOCKET) {
                printf("Forward: Accept failed: %d\n", WSAGetLastError());
                closesocket(listen_sock);
                WSACleanup();
                break;
            }
            printf("Forward: Client connected\n");

            uint32_t remote_id = transport.openChannel("tcp:1081", 1);
            if (remote_id == 0) {
                printf("Forward: Cannot open ADB channel\n");
                closesocket(client);
                closesocket(listen_sock);
                WSACleanup();
                break;
            }
            printf("Forward: ADB channel to tcp:1081 opened (remote_id=%u)\n", remote_id);

            // 双向转发: select 监听本地 socket → ADB, poll ADB → 本地
            printf("Forward: Starting data relay...\n");
            bool fwd_running = true;
            while (fwd_running) {
                fd_set fds;
                FD_ZERO(&fds);
                FD_SET(client, &fds);
                timeval tv = {0, 100000}; // 100ms
                int sel_ret = select(0, &fds, nullptr, nullptr, &tv);

                // 本地 → ADB
                if (sel_ret > 0 && FD_ISSET(client, &fds)) {
                    char buf[8192];
                    int n = recv(client, buf, sizeof(buf), 0);
                    if (n > 0) {
                        printf("FWD: local→ADB %d bytes\n", n);
                        if (!transport.writeChannel(1, remote_id, buf, n)) {
                            printf("FWD: ADB write failed\n");
                            break;
                        }
                    } else {
                        printf("FWD: Client disconnected (%d)\n", n);
                        break;
                    }
                }

                // ADB → 本地
                std::vector<uint8_t> adb_data;
                if (transport.readChannel(1, remote_id, adb_data, 50)) {
                    printf("FWD: ADB→local %zu bytes\n", adb_data.size());
                    send(client, (char*)adb_data.data(), adb_data.size(), 0);
                }
            }

            transport.closeChannel(1, remote_id);
            closesocket(client);
            closesocket(listen_sock);
            WSACleanup();
            break;
        }

        printf("Handshake attempt %d failed.\n", attempt + 1);
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
