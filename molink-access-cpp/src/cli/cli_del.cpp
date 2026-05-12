#include "cli_utils.h"

#include <cstdio>
#include <cstring>

// ---- del 命令 ----
int cmdDel(int argc, char* argv[]) {
    if (argc < 3) {
        printf("Usage: molink del <remote_path>\n");
        return 1;
    }

    if (sendPipeCmd("status").empty()) {
        printf("Daemon is not running. Use 'molink start' first.\n");
        return 1;
    }

    std::string cmd = "del " + std::string(argv[2]);
    auto resp = sendPipeCmd(cmd);
    if (resp.empty()) {
        printf("Daemon is not running. Use 'molink start' first.\n");
        return 1;
    }
    printf("%s\n", resp.c_str());
    return (resp == "ok") ? 0 : 1;
}
