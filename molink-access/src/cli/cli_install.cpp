#include "cli_utils.h"
#include "../utils/win_utils.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <filesystem>

namespace fs = std::filesystem;

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
    fs::path absInput = fs::absolute(fs::u8path(apkPath), ec);
    std::string absPath = ec ? apkPath : pathToUtf8(absInput);
    if (!fs::exists(absInput)) {
        printf("APK not found: %s\n", apkPath.c_str());
        return 1;
    }

    if (sendPipeCmd("status").empty()) {
        printf("Daemon is not running. Use 'molink start' first.\n");
        return 1;
    }

    std::string fname = absInput.filename().u8string();
    std::string remote = "/data/local/tmp/" + fname;

    printf("Pushing %s ...\n", utf8ForConsole(fname).c_str());
    std::string pushCmd = "push " + absPath + " " + remote;
    auto resp = sendPipeCmd(pushCmd);
    if (resp.empty()) { printf("Daemon is not running.\n"); return 1; }
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
