#include "file_list.h"
#include "../adb/adb_client.h"
#include "../adb/adb_shell.h"
#include <cstdio>

std::string listFiles(AdbClient& client, const std::string& remotePath) {
    std::string path = remotePath.empty() ? "/sdcard/" : remotePath;
    std::string cmd = "ls -la " + path;
    std::string output = shellCommand(client, cmd);

    if (output.empty()) {
        return "fail: No output from shell ls command";
    }
    return output;
}
