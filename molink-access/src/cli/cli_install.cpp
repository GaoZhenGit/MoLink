#include "cli_utils.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <filesystem>

int cmdInstall(int argc, char* argv[]) {
    if (argc < 3) {
        printf("Usage: molink install [options] <apk_file>\n"
               "Options:\n"
               "  -r                     Replace existing application\n"
               "  -d                     Allow downgrade\n"
               "  -g                     Grant all runtime permissions\n");
        return 1;
    }

    std::string apkPath;
    std::string installFlags;
    for (int i = 2; i < argc; i++) {
        if (argv[i][0] == '-') {
            installFlags += " " + std::string(argv[i]);
        } else {
            apkPath = argv[i];
        }
    }
    if (apkPath.empty()) {
        printf("No APK file specified\n");
        return 1;
    }

    std::error_code ec;
    std::string absPath = std::filesystem::absolute(apkPath, ec).string();
    if (ec) absPath = apkPath;
    if (!std::filesystem::exists(absPath)) {
        printf("APK not found: %s\n", apkPath.c_str());
        return 1;
    }

    if (sendPipeCmd("status").empty()) {
        printf("Daemon is not running. Use 'molink start' first.\n");
        return 1;
    }

    std::string fname = std::filesystem::path(absPath).filename().string();
    std::string remote = "/data/local/tmp/" + fname;

    printf("Pushing %s ...\n", fname.c_str());
    std::string pushCmd = "push " + absPath + " " + remote;
    auto resp = sendPipeCmd(pushCmd);
    if (resp != "ok") {
        printf("Push failed: %s\n", resp.c_str());
        return 1;
    }

    printf("Installing...\n");
    std::string shellCmd = "shell pm install" + installFlags + " " + remote;
    auto result = sendPipeCmd(shellCmd);
    printf("%s\n", result.c_str());

    std::string rmCmd = "shell rm " + remote;
    sendPipeCmd(rmCmd);

    return (result.find("Success") != std::string::npos) ? 0 : 1;
}
