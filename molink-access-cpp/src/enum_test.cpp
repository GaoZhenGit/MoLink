#include <windows.h>
#include <setupapi.h>
#include <cstdio>
#include <cstdint>
#include <vector>

int main() {
    // Enumerate ALL USB devices and print their device paths
    HDEVINFO devInfo = SetupDiGetClassDevsW(
        nullptr, L"USB", nullptr,
        DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (devInfo == INVALID_HANDLE_VALUE) {
        printf("FAIL: SetupDiGetClassDevsW error %lu\n", GetLastError());
        return 1;
    }

    SP_DEVINFO_DATA devInfoData = {sizeof(SP_DEVINFO_DATA)};
    for (DWORD idx = 0; SetupDiEnumDeviceInfo(devInfo, idx, &devInfoData); idx++) {
        // Get device description
        WCHAR desc[256] = {0};
        SetupDiGetDeviceRegistryPropertyW(devInfo, &devInfoData, SPDRP_DEVICEDESC,
                                          nullptr, (PBYTE)desc, sizeof(desc), nullptr);

        // Get hardware ID
        WCHAR hwid[256] = {0};
        SetupDiGetDeviceRegistryPropertyW(devInfo, &devInfoData, SPDRP_HARDWAREID,
                                          nullptr, (PBYTE)hwid, sizeof(hwid), nullptr);

        // Check if it's our Meizu device
        char hwid_a[256] = {0};
        wcstombs(hwid_a, hwid, sizeof(hwid_a));
        if (strstr(hwid_a, "2A45") && strstr(hwid_a, "4EE7")) {
            char desc_a[256] = {0};
            wcstombs(desc_a, desc, sizeof(desc_a));
            printf("\nDevice[%lu]: %s\n", idx, desc_a);
            printf("  HWID: %s\n", hwid_a);

            // Enumerate device interfaces
            SP_DEVICE_INTERFACE_DATA ifaceData = {sizeof(SP_DEVICE_INTERFACE_DATA)};
            for (DWORD j = 0;
                 SetupDiEnumDeviceInterfaces(devInfo, &devInfoData, nullptr, j, &ifaceData);
                 j++) {
                DWORD requiredSize = 0;
                SetupDiGetDeviceInterfaceDetailW(devInfo, &ifaceData, nullptr, 0,
                                                  &requiredSize, nullptr);
                if (requiredSize == 0) continue;

                std::vector<uint8_t> buf(requiredSize);
                PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail =
                    (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)buf.data();
                detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

                if (SetupDiGetDeviceInterfaceDetailW(devInfo, &ifaceData, detail,
                                                      requiredSize, nullptr, nullptr)) {
                    char path[512] = {0};
                    wcstombs(path, detail->DevicePath, sizeof(path));
                    printf("  Iface[%lu] GUID=%08X-... path=%s\n",
                           j, ifaceData.InterfaceClassGuid.Data1, path);

                    // Check if it's the WinUSB GUID
                    // {DEE824EF-729B-4A0E-9C69-B433F3A7E7A4} = standard WinUSB
                    if (ifaceData.InterfaceClassGuid.Data1 == 0xDEE824EF) {
                        printf("    -> This is the standard WinUSB GUID!\n");
                    }
                }
            }
        }
    }

    SetupDiDestroyDeviceInfoList(devInfo);
    return 0;
}
