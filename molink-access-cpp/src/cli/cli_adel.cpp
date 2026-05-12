#include "cli_utils.h"
#include "base64.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <sstream>

// ---- adel 命令 ----
int cmdAdel(int argc, char* argv[]) {
    std::string rdir = kRemoteDir;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--rdir") == 0 && i + 1 < argc) rdir = argv[++i];
    }

    if (sendPipeCmd("status").empty()) {
        printf("Daemon is not running. Use 'molink start' first.\n");
        return 1;
    }

    std::string lsCmd = "ls " + rdir;
    std::string lsOutput = sendPipeCmd(lsCmd);
    if (lsOutput.empty() || lsOutput.find("fail:") == 0) {
        printf("Directory not found or empty: %s\n", rdir.c_str());
        return 1;
    }

    auto files = listRemoteFiles(lsOutput);
    if (files.empty()) {
        printf("No files to delete\n");
        return 1;
    }

    printf("\n=== Select file to delete ===\n");
    for (size_t i = 0; i < files.size(); i++) {
        const char* tag = files[i].isZip ? " [ZIP]" : "";
        printf("  [%zu] %s%s\n", i, files[i].displayName.c_str(), tag);
    }
    printf("  [a] Delete ALL listed files\n");

    printf("\nEnter number (or 'a'): ");
    char input[32];
    if (!fgets(input, sizeof(input), stdin)) {
        printf("Invalid input\n");
        return 1;
    }

    size_t len = strlen(input);
    while (len > 0 && (input[len-1] == '\n' || input[len-1] == '\r'))
        input[--len] = '\0';

    if (len == 0) {
        printf("Invalid input\n");
        return 1;
    }

    int deleted = 0;
    if (input[0] == 'a' || input[0] == 'A') {
        for (auto& f : files) {
            std::string delCmd = "del " + rdir + "/" + f.rawName;
            auto resp = sendPipeCmd(delCmd);
            if (resp == "ok") {
                printf("Deleted: %s\n", f.displayName.c_str());
                deleted++;
            } else {
                printf("Failed: %s (%s)\n", f.displayName.c_str(), resp.c_str());
            }
        }
    } else {
        int choice = atoi(input);
        if (choice < 0 || choice >= (int)files.size()) {
            printf("Invalid selection\n");
            return 1;
        }
        RemoteFile& selected = files[choice];
        std::string delCmd = "del " + rdir + "/" + selected.rawName;
        auto resp = sendPipeCmd(delCmd);
        if (resp == "ok") {
            printf("Deleted: %s\n", selected.displayName.c_str());
            deleted++;
        } else {
            printf("Failed: %s\n", resp.c_str());
            return 1;
        }
    }

    printf("Deleted %d file(s)\n", deleted);
    return 0;
}
