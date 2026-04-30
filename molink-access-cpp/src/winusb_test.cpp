#include <windows.h>
#include <winusb.h>
#include <setupapi.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>

#pragma comment(lib, "winusb.lib")
#pragma comment(lib, "setupapi.lib")

// ADB device interface GUID
// {F72FE0D4-CBCB-407D-8814-9ED673D0DD6B}
static const GUID ADB_GUID = {0xF72FE0D4, 0xCBCB, 0x407D,
    {0x88, 0x14, 0x9E, 0xD6, 0x73, 0xD0, 0xDD, 0x6B}};

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("=== WinUSB Direct Test ===\n");

    // Step 1: Enumerate devices with ADB GUID
    HDEVINFO devInfo = SetupDiGetClassDevsW(
        &ADB_GUID, nullptr, nullptr,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devInfo == INVALID_HANDLE_VALUE) {
        printf("FAIL: SetupDiGetClassDevsW error %lu\n", GetLastError());
        return 1;
    }

    SP_DEVICE_INTERFACE_DATA ifaceData = {sizeof(SP_DEVICE_INTERFACE_DATA)};
    if (!SetupDiEnumDeviceInterfaces(devInfo, nullptr, &ADB_GUID, 0, &ifaceData)) {
        printf("FAIL: SetupDiEnumDeviceInterfaces error %lu\n", GetLastError());
        SetupDiDestroyDeviceInfoList(devInfo);
        return 1;
    }

    // Get required size
    DWORD requiredSize = 0;
    SetupDiGetDeviceInterfaceDetailW(devInfo, &ifaceData, nullptr, 0, &requiredSize, nullptr);
    if (requiredSize == 0) {
        printf("FAIL: SetupDiGetDeviceInterfaceDetailW size error %lu\n", GetLastError());
        SetupDiDestroyDeviceInfoList(devInfo);
        return 1;
    }

    std::vector<uint8_t> detailBuf(requiredSize);
    PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail =
        (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)detailBuf.data();
    detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

    SP_DEVINFO_DATA devInfoData = {sizeof(SP_DEVINFO_DATA)};
    if (!SetupDiGetDeviceInterfaceDetailW(devInfo, &ifaceData, detail,
                                          requiredSize, nullptr, &devInfoData)) {
        printf("FAIL: SetupDiGetDeviceInterfaceDetailW error %lu\n", GetLastError());
        SetupDiDestroyDeviceInfoList(devInfo);
        return 1;
    }

    printf("Device path: %ls\n", detail->DevicePath);

    // Step 2: Open device via CreateFile
    HANDLE hDev = CreateFileW(
        detail->DevicePath,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
        nullptr);
    if (hDev == INVALID_HANDLE_VALUE) {
        printf("FAIL: CreateFileW error %lu\n", GetLastError());
        SetupDiDestroyDeviceInfoList(devInfo);
        return 1;
    }
    printf("Device opened\n");

    // Step 3: Initialize WinUSB
    WINUSB_INTERFACE_HANDLE winusbHandle;
    if (!WinUsb_Initialize(hDev, &winusbHandle)) {
        printf("FAIL: WinUsb_Initialize error %lu\n", GetLastError());
        CloseHandle(hDev);
        SetupDiDestroyDeviceInfoList(devInfo);
        return 1;
    }
    printf("WinUSB initialized\n");

    // Step 4: Query pipes (pipe index is 0-based, NOT endpoint address)
    WINUSB_PIPE_INFORMATION pipeInfo[2];
    ULONG timeout = 0; // 0 = infinite

    for (UCHAR i = 0; i < 2; i++) {
        if (!WinUsb_QueryPipe(winusbHandle, 0, i, &pipeInfo[i])) {
            printf("FAIL: WinUsb_QueryPipe(index=%d) error %lu\n", i, GetLastError());
            WinUsb_Free(winusbHandle);
            CloseHandle(hDev);
            SetupDiDestroyDeviceInfoList(devInfo);
            return 1;
        }
        printf("Pipe[%d]: PipeId=0x%02X type=%d maxpkt=%d\n",
               i, pipeInfo[i].PipeId, pipeInfo[i].PipeType,
               pipeInfo[i].MaximumPacketSize);

        // Set PIPE_TRANSFER_TIMEOUT to 0 (infinite)
        if (!WinUsb_SetPipePolicy(winusbHandle, pipeInfo[i].PipeId,
                                   PIPE_TRANSFER_TIMEOUT,
                                   sizeof(timeout), &timeout)) {
            printf("WARN: SetPipePolicy TIMEOUT ep=0x%02X error %lu\n",
                   pipeInfo[i].PipeId, GetLastError());
        }

        // Set AUTO_CLEAR_STALL for IN endpoint
        if (pipeInfo[i].PipeId & 0x80) { // IN endpoint
            UCHAR autoClear = 1;
            if (!WinUsb_SetPipePolicy(winusbHandle, pipeInfo[i].PipeId,
                                       AUTO_CLEAR_STALL,
                                       sizeof(autoClear), &autoClear)) {
                printf("WARN: SetPipePolicy AUTO_CLEAR_STALL ep=0x%02X error %lu\n",
                       pipeInfo[i].PipeId, GetLastError());
            }
        }
    }

    // Determine endpoint addresses
    UCHAR writeEp = 0, readEp = 0;
    for (int i = 0; i < 2; i++) {
        if (pipeInfo[i].PipeId & 0x80) // IN endpoint
            readEp = pipeInfo[i].PipeId;
        else
            writeEp = pipeInfo[i].PipeId;
    }
    printf("Write EP: 0x%02X, Read EP: 0x%02X\n", writeEp, readEp);
    printf("Pipe timeouts set to INFINITE\n");

    // Step 5: Build A_CNXN message
    constexpr uint32_t A_CNXN = 0x4e584e43;
    constexpr uint32_t A_VERSION = 0x01000000;
    constexpr uint32_t MAX_PAYLOAD = 256 * 1024;
    const char* banner = "host::";
    uint32_t bannerLen = (uint32_t)strlen(banner) + 1; // include null

    // CRC32
    uint32_t crc = 0xFFFFFFFF;
    static uint32_t crcTable[256];
    static bool crcReady = false;
    if (!crcReady) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int j = 0; j < 8; j++)
                c = (c >> 1) ^ ((c & 1) ? 0xEDB88320UL : 0);
            crcTable[i] = c;
        }
        crcReady = true;
    }
    for (uint32_t i = 0; i < bannerLen; i++)
        crc = (crc >> 8) ^ crcTable[(crc ^ (uint8_t)banner[i]) & 0xFF];
    crc ^= 0xFFFFFFFF;

    // Build header manually
    uint8_t sendBuf[256];
    uint32_t* header = (uint32_t*)sendBuf;
    header[0] = A_CNXN;
    header[1] = A_VERSION;
    header[2] = MAX_PAYLOAD;
    header[3] = bannerLen;
    header[4] = crc;
    header[5] = A_CNXN ^ 0xFFFFFFFF;
    memcpy(sendBuf + 24, banner, bannerLen);
    uint32_t sendLen = 24 + bannerLen;

    printf("ADB A_CNXN: cmd=0x%08X ver=%u max=%u banner='%s' len=%u crc=0x%08X\n",
           header[0], header[1], header[2], banner, bannerLen, crc);

    // Step 6: Bulk write
    ULONG written = 0;
    if (!WinUsb_WritePipe(winusbHandle, writeEp, sendBuf, sendLen, &written, nullptr)) {
        printf("FAIL: WinUsb_WritePipe error %lu\n", GetLastError());
        WinUsb_Free(winusbHandle);
        CloseHandle(hDev);
        SetupDiDestroyDeviceInfoList(devInfo);
        return 1;
    }
    printf("Bulk write: %lu bytes sent\n", written);

    // Step 7: Bulk read (infinite timeout)
    printf("Bulk read: waiting for response (infinite)...\n");
    uint8_t readBuf[256];
    ULONG readLen = 0;
    if (!WinUsb_ReadPipe(winusbHandle, readEp, readBuf, sizeof(readBuf), &readLen, nullptr)) {
        DWORD err = GetLastError();
        printf("FAIL: WinUsb_ReadPipe error %lu\n", err);
    } else {
        printf("Bulk read: %lu bytes received\n", readLen);
        printf("Hex: ");
        for (ULONG i = 0; i < readLen && i < 64; i++)
            printf("%02X ", readBuf[i]);
        printf("\n");
    }

    // Cleanup
    WinUsb_Free(winusbHandle);
    CloseHandle(hDev);
    SetupDiDestroyDeviceInfoList(devInfo);
    printf("\n=== Test Complete ===\n");
    return 0;
}
