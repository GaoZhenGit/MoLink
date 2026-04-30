#include <windows.h>
#include <winusb.h>
#include <setupapi.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>

#pragma comment(lib, "winusb.lib")
#pragma comment(lib, "setupapi.lib")

static const GUID ADB_GUID = {0xF72FE0D4, 0xCBCB, 0x407D,
    {0x88, 0x14, 0x9E, 0xD6, 0x73, 0xD0, 0xDD, 0x6B}};

static uint32_t crc32_calc(const uint8_t* data, uint32_t len) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int j = 0; j < 8; j++)
                c = (c >> 1) ^ ((c & 1) ? 0xEDB88320UL : 0);
            table[i] = c;
        }
        ready = true;
    }
    uint32_t crc = 0xFFFFFFFF;
    for (uint32_t i = 0; i < len; i++)
        crc = (crc >> 8) ^ table[(crc ^ data[i]) & 0xFF];
    return crc ^ 0xFFFFFFFF;
}

static void build_msg(uint8_t* buf, uint32_t& len,
                      uint32_t cmd, uint32_t arg0, uint32_t arg1,
                      const void* data, uint32_t dataLen) {
    uint32_t crc = data ? crc32_calc((const uint8_t*)data, dataLen) : 0;
    uint32_t* h = (uint32_t*)buf;
    h[0] = cmd;
    h[1] = arg0;
    h[2] = arg1;
    h[3] = dataLen;
    h[4] = crc;
    h[5] = cmd ^ 0xFFFFFFFF;
    if (data && dataLen > 0)
        memcpy(buf + 24, data, dataLen);
    len = 24 + dataLen;
}

static bool do_write(HANDLE winusb, UCHAR ep, const uint8_t* buf, uint32_t len) {
    ULONG written = 0;
    if (!WinUsb_WritePipe(winusb, ep, (PUCHAR)buf, len, &written, nullptr)) {
        printf("  Write error %lu\n", GetLastError());
        return false;
    }
    printf("  Wrote %lu bytes\n", written);
    return true;
}

static bool do_read(HANDLE winusb, UCHAR ep, uint8_t* buf, ULONG* len, ULONG timeout_ms) {
    // Set temporary read timeout
    WinUsb_SetPipePolicy(winusb, ep, PIPE_TRANSFER_TIMEOUT,
                          sizeof(timeout_ms), &timeout_ms);
    bool ok = WinUsb_ReadPipe(winusb, ep, (PUCHAR)buf, 256, len, nullptr);
    if (!ok) {
        DWORD err = GetLastError();
        if (err == ERROR_SEM_TIMEOUT)
            printf("  Read timeout (%lums)\n", timeout_ms);
        else
            printf("  Read error %lu\n", err);
        return false;
    }
    printf("  Read %lu bytes: ", *len);
    for (ULONG i = 0; i < *len && i < 48; i++)
        printf("%02X ", buf[i]);
    printf("\n");

    // Parse header
    if (*len >= 24) {
        uint32_t* h = (uint32_t*)buf;
        printf("  ADB: cmd=0x%08X arg0=%u arg1=%u len=%u crc=%u magic=0x%08X\n",
               h[0], h[1], h[2], h[3], h[4], h[5]);
    }
    return true;
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("=== ADB Protocol Diagnostic ===\n");

    // Enumerate and open device (same as before)
    HDEVINFO devInfo = SetupDiGetClassDevsW(
        &ADB_GUID, nullptr, nullptr,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devInfo == INVALID_HANDLE_VALUE) {
        printf("FAIL: SetupDiGetClassDevsW error %lu\n", GetLastError());
        return 1;
    }

    SP_DEVICE_INTERFACE_DATA ifaceData = {sizeof(SP_DEVICE_INTERFACE_DATA)};
    if (!SetupDiEnumDeviceInterfaces(devInfo, nullptr, &ADB_GUID, 0, &ifaceData)) {
        printf("FAIL: No ADB device found\n");
        SetupDiDestroyDeviceInfoList(devInfo);
        return 1;
    }

    DWORD requiredSize = 0;
    SetupDiGetDeviceInterfaceDetailW(devInfo, &ifaceData, nullptr, 0, &requiredSize, nullptr);
    std::vector<uint8_t> detailBuf(requiredSize);
    PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail =
        (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)detailBuf.data();
    detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

    if (!SetupDiGetDeviceInterfaceDetailW(devInfo, &ifaceData, detail,
                                          requiredSize, nullptr, nullptr)) {
        printf("FAIL: GetDeviceInterfaceDetail error %lu\n", GetLastError());
        SetupDiDestroyDeviceInfoList(devInfo);
        return 1;
    }

    HANDLE hDev = CreateFileW(
        detail->DevicePath,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
    if (hDev == INVALID_HANDLE_VALUE) {
        printf("FAIL: CreateFile error %lu\n", GetLastError());
        SetupDiDestroyDeviceInfoList(devInfo);
        return 1;
    }

    HANDLE winusb;
    if (!WinUsb_Initialize(hDev, &winusb)) {
        printf("FAIL: WinUsb_Initialize error %lu\n", GetLastError());
        CloseHandle(hDev);
        SetupDiDestroyDeviceInfoList(devInfo);
        return 1;
    }

    // Query pipes
    WINUSB_PIPE_INFORMATION pipeInfo[2];
    UCHAR writeEp = 0, readEp = 0;
    for (UCHAR i = 0; i < 2; i++) {
        WinUsb_QueryPipe(winusb, 0, i, &pipeInfo[i]);
        if (pipeInfo[i].PipeId & 0x80) readEp = pipeInfo[i].PipeId;
        else writeEp = pipeInfo[i].PipeId;
    }

    // Set infinite timeout for read pipe
    ULONG inf = 0;
    WinUsb_SetPipePolicy(winusb, readEp, PIPE_TRANSFER_TIMEOUT, sizeof(inf), &inf);

    printf("READY. writeEp=0x%02X readEp=0x%02X\n\n", writeEp, readEp);

    // Test 1: Send A_CNXN (standard handshake)
    printf("--- Test 1: A_CNXN (standard handshake) ---\n");
    {
        const char* banner = "host::";
        uint8_t buf[256]; uint32_t len;
        build_msg(buf, len, 0x4e584e43, 0x01000000, 256*1024, banner, (uint32_t)strlen(banner)+1);
        do_write(winusb, writeEp, buf, len);

        uint8_t rbuf[256]; ULONG rlen;
        if (do_read(winusb, readEp, rbuf, &rlen, 5000)) {
            printf("  SUCCESS! Device responded to A_CNXN\n");
        } else {
            printf("  NO RESPONSE to A_CNXN\n");
        }
    }

    // Test 2: Send A_CLSE (close non-existent channel) - should trigger error
    printf("\n--- Test 2: A_CLSE (close invalid channel) ---\n");
    {
        uint8_t buf[256]; uint32_t len;
        build_msg(buf, len, 0x45534c43, 999, 888, nullptr, 0);
        do_write(winusb, writeEp, buf, len);

        uint8_t rbuf[256]; ULONG rlen;
        if (do_read(winusb, readEp, rbuf, &rlen, 5000)) {
            printf("  SUCCESS! Got response\n");
        } else {
            printf("  NO RESPONSE\n");
        }
    }

    // Test 3: Send A_CNXN with different version (maybe old protocol)
    printf("\n--- Test 3: A_CNXN v0 (old protocol) ---\n");
    {
        const char* banner = "host::";
        uint8_t buf[256]; uint32_t len;
        build_msg(buf, len, 0x4e584e43, 0, 0, banner, (uint32_t)strlen(banner)+1);
        do_write(winusb, writeEp, buf, len);

        uint8_t rbuf[256]; ULONG rlen;
        if (do_read(winusb, readEp, rbuf, &rlen, 5000)) {
            printf("  SUCCESS! Got response\n");
        } else {
            printf("  NO RESPONSE\n");
        }
    }

    // Test 4: Send garbage (invalid magic) - device should ignore, but might trigger reset
    printf("\n--- Test 4: Invalid message (bad magic) ---\n");
    {
        uint8_t buf[256];
        memset(buf, 0xAA, 24);
        do_write(winusb, writeEp, buf, 24);

        uint8_t rbuf[256]; ULONG rlen;
        if (do_read(winusb, readEp, rbuf, &rlen, 2000)) {
            printf("  Got response to invalid message!\n");
        } else {
            printf("  No response (expected)\n");
        }
    }

    // Test 5: Send A_CNXN one more time (maybe device state changed)
    printf("\n--- Test 5: A_CNXN (retry) ---\n");
    {
        const char* banner = "host::";
        uint8_t buf[256]; uint32_t len;
        build_msg(buf, len, 0x4e584e43, 0x01000000, 256*1024, banner, (uint32_t)strlen(banner)+1);
        do_write(winusb, writeEp, buf, len);

        uint8_t rbuf[256]; ULONG rlen;
        if (do_read(winusb, readEp, rbuf, &rlen, 10000)) {
            printf("  SUCCESS! Got response on retry\n");
        } else {
            printf("  STILL NO RESPONSE\n");
        }
    }

    WinUsb_Free(winusb);
    CloseHandle(hDev);
    SetupDiDestroyDeviceInfoList(devInfo);
    printf("\n=== Diagnostic Complete ===\n");
    return 0;
}
