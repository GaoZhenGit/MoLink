#include "cli_utils.h"
#include "../adb/adb_transport.h"
#include "../adb/adb_rsa.h"
#include "../usb/usb_device.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <windows.h>
#include <io.h>

static const int kShortTimeout = 5000;   // 已授权时响应很快
static const int kUserTimeout  = 60000;  // 等待用户在设备上操作

// 与单个设备握手，返回授权结果
// 逻辑与 AdbTransport::handshake(AdbRsa&) 一致：多轮循环
static std::string doHandshake(AdbTransport& tr, AdbRsa& rsa) {
    uint32_t maxdata = MAX_PAYLOAD_V2;
    tr.send(A_CNXN, A_VERSION, maxdata, "host::", 5);

    bool waitingForAuth = false;

    for (int round = 0; round < 3; round++) {
        AdbMessage msg;
        std::vector<uint8_t> data;
        uint32_t timeout = waitingForAuth ? kUserTimeout : kShortTimeout;

        if (!tr.recv(msg, data, timeout)) {
            if (waitingForAuth)
                return "fail: No response from device (timeout)";
            return "fail: Device not responding";
        }

        if (msg.command == A_CNXN) {
            if (round == 0)
                return "ok: Already authorized";
            return "ok: User accepted";
        }

        if (msg.command == A_AUTH && msg.arg0 == AUTH_TOKEN) {
            if (waitingForAuth) {
                // 第二次收到 TOKEN：key 不在设备上，发送公钥
                fprintf(stderr, "  Sending public key...\n");
                fflush(stderr);
                std::string payload = rsa.getPubKeyPayload();
                if (payload.empty()) return "fail: Cannot get public key";
                tr.send(A_AUTH, AUTH_RSAPUBLICKEY, 0, payload.data(), (uint32_t)payload.size());
            } else {
                // 第一次收到 TOKEN：签名
                fprintf(stderr, "  Device requires authorization. Check device screen...\n");
                fflush(stderr);
                auto sig = rsa.signToken(data.data(), data.size());
                if (sig.empty()) return "fail: Cannot sign token";
                tr.send(A_AUTH, AUTH_SIGNATURE, 0, sig.data(), (uint32_t)sig.size());
                waitingForAuth = true;
            }
        } else {
            return "fail: Unexpected response (cmd=0x" +
                   std::to_string(msg.command) + ")";
        }
    }

    return "fail: Authorization failed after 3 rounds";
}

// ---- auth 命令 ----
int cmdAuth(int argc, char* argv[]) {
    setvbuf(stdout, nullptr, _IONBF, 0);

    // 解析参数
    std::string targetSerial;
    for (int i = 2; i < argc; i++) {
        if ((strcmp(argv[i], "--serial") == 0 || strcmp(argv[i], "-s") == 0) && i + 1 < argc)
            targetSerial = argv[++i];
    }

    // 如果 daemon 已连接，说明已授权
    auto statusResp = sendPipeCmd("status");
    if (!statusResp.empty() && statusResp.find("connected") != std::string::npos) {
        size_t pos = statusResp.find("serial=");
        if (pos != std::string::npos) {
            std::string s = statusResp.substr(pos + 7);
            size_t end = s.find_first_of(" \t");
            if (end != std::string::npos) s.resize(end);
            if (targetSerial.empty() || s == targetSerial) {
                printf("Already authorized: %s (daemon connected)\n", s.c_str());
                return 0;
            }
        }
    }

    // 加载密钥（抑制内部 debug 输出）
    fflush(stdout);
    int saved = _dup(1);
    freopen("NUL", "w", stdout);

    AdbRsa rsa;
    bool keyReady = loadOrGenerateKey(rsa);

    fflush(stdout);
    _dup2(saved, 1);
    _close(saved);

    if (!keyReady) {
        printf("No ADB key found. Run 'molink devices' first.\n");
        return 1;
    }

    // 枚举设备
    fflush(stdout);
    saved = _dup(1);
    freopen("NUL", "w", stdout);

    auto devices = UsbDevice::discover();

    fflush(stdout);
    _dup2(saved, 1);
    _close(saved);

    if (devices.empty()) {
        printf("No ADB devices found.\n");
        return 1;
    }

    int okCount = 0;
    int failCount = 0;

    for (auto& dev : devices) {
        std::string serial = dev.getSerial();
        if (!targetSerial.empty() && serial != targetSerial) continue;

        if (serial.empty()) {
            printf("[?] Unknown device...\n");
        } else {
            printf("[%s] ", serial.c_str());
        }

        // 打开设备 + 握手全程静默
        fflush(stdout);
        saved = _dup(1);
        freopen("NUL", "w", stdout);

        bool opened = dev.prepare();

        std::string result;
        if (opened) {
            AdbTransport tr(dev);
            result = doHandshake(tr, rsa);
        }

        fflush(stdout);
        _dup2(saved, 1);
        _close(saved);

        if (!opened) {
            printf("Cannot open device (in use by daemon?)\n");
            failCount++;
            continue;
        }

        printf("%s\n", result.c_str());
        dev.close();

        if (result.rfind("ok:", 0) == 0) okCount++;
        else failCount++;
    }

    if (okCount > 0 && failCount == 0) {
        return 0;
    }
    if (okCount > 0) {
        printf("\n%d authorized, %d failed\n", okCount, failCount);
        return 0;
    }
    return 1;
}
