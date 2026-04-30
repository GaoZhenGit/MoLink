#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <cstdio>
#include <cstdint>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "cfgmgr32.lib")

int main() {
    // Find the Meizu ADB device
    HDEVINFO devInfo = SetupDiGetClassDevsW(
        nullptr, L"USB", nullptr,
        DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (devInfo == INVALID_HANDLE_VALUE) {
        printf("FAIL: SetupDiGetClassDevsW error %lu\n", GetLastError());
        return 1;
    }

    SP_DEVINFO_DATA devInfoData = {sizeof(SP_DEVINFO_DATA)};
    bool found = false;
    for (DWORD idx = 0; SetupDiEnumDeviceInfo(devInfo, idx, &devInfoData); idx++) {
        WCHAR hwid[256] = {0};
        SetupDiGetDeviceRegistryPropertyW(devInfo, &devInfoData, SPDRP_HARDWAREID,
                                          nullptr, (PBYTE)hwid, sizeof(hwid), nullptr);
        char hwid_a[256] = {0};
        wcstombs(hwid_a, hwid, sizeof(hwid_a));
        if (strstr(hwid_a, "2A45") && strstr(hwid_a, "4EE7")) {
            printf("Found Meizu device: %s\n", hwid_a);
            found = true;

            // Disable and re-enable the device
            SP_PROPCHANGE_PARAMS params = {
                sizeof(SP_CLASSINSTALL_HEADER),
                DIF_PROPERTYCHANGE,
                HKEY_LOCAL_MACHINE,
                0, // scope
                0, // hwProfile
                DICS_DISABLE,
                0, 0
            };

            // Disable
            printf("Disabling device...\n");
            if (!SetupDiSetClassInstallParamsW(devInfo, &devInfoData,
                (PSP_CLASSINSTALL_HEADER)&params, sizeof(params))) {
                printf("SetClassInstallParams (disable) failed: %lu\n", GetLastError());
            }
            if (!SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, devInfo, &devInfoData)) {
                printf("CallClassInstaller (disable) failed: %lu\n", GetLastError());
            } else {
                printf("Device disabled. Waiting 2s...\n");
                Sleep(2000);

                // Re-enable
                params.StateChange = DICS_ENABLE;
                printf("Enabling device...\n");
                if (!SetupDiSetClassInstallParamsW(devInfo, &devInfoData,
                    (PSP_CLASSINSTALL_HEADER)&params, sizeof(params))) {
                    printf("SetClassInstallParams (enable) failed: %lu\n", GetLastError());
                }
                if (!SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, devInfo, &devInfoData)) {
                    printf("CallClassInstaller (enable) failed: %lu\n", GetLastError());
                } else {
                    printf("Device re-enabled. Waiting 3s for re-enumeration...\n");
                    Sleep(3000);
                }
            }
            break;
        }
    }

    SetupDiDestroyDeviceInfoList(devInfo);
    if (!found) printf("Device not found\n");
    else printf("Done\n");
    return 0;
}
