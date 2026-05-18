#include "cli_utils.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>

// ---- forward 命令 ----
int cmdForward(int argc, char* argv[]) {
    uint16_t lp = 1080;
    uint16_t rp = 1081;

    for (int i = 2; i < argc; i++) {
        if ((strcmp(argv[i], "--port") == 0 || strcmp(argv[i], "-p") == 0) && i + 1 < argc)
            lp = (uint16_t)atoi(argv[++i]);
        else if ((strcmp(argv[i], "--rport") == 0 || strcmp(argv[i], "-r") == 0) && i + 1 < argc)
            rp = (uint16_t)atoi(argv[++i]);
    }

    std::string cmd = "forward " + std::to_string(lp) + " " + std::to_string(rp);
    auto resp = sendPipeCmd(cmd);
    if (resp.empty()) {
        printf("Daemon is not running. Use 'molink start' first.\n");
        return 1;
    }
    printf("%s\n", resp.c_str());
    return (resp.rfind("ok", 0) == 0) ? 0 : 1;
}
