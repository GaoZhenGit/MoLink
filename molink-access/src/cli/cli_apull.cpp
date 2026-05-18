#include "cli_utils.h"
#include "base64.h"
#include "zip_utils.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <windows.h>
#include <sstream>

// ---- 解压目录 ----
static std::string extractFolder(const std::string& zipPath,
                                  const std::string& destDir,
                                  int& fileCount) {
    if (zip::extractZip(zipPath, destDir, &fileCount))
        return "ok";
    return "fail: Cannot extract zip file";
}

// ---- apull 命令 ----
int cmdApull(int argc, char* argv[]) {
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
        printf("No files available for download\n");
        return 1;
    }

    printf("\n=== Select file to download ===\n");
    for (size_t i = 0; i < files.size(); i++) {
        const char* tag = files[i].isZip ? " [ZIP]" : "";
        printf("  [%zu] %s%s\n", i, files[i].displayName.c_str(), tag);
    }

    printf("\nEnter number: ");
    int choice = -1;
    if (scanf("%d", &choice) != 1 || choice < 0 || choice >= (int)files.size()) {
        printf("Invalid selection\n");
        return 1;
    }

    RemoteFile& selected = files[choice];
    char tempDir[MAX_PATH];
    GetTempPathA(sizeof(tempDir), tempDir);
    std::string tempPath = std::string(tempDir) + selected.rawName;

    std::string pullCmd = "pull " + rdir + "/" + selected.rawName + " " + tempPath;
    auto resp = sendPipeCmd(pullCmd);
    if (resp != "ok") {
        printf("%s\n", resp.c_str());
        return 1;
    }

    if (selected.isZip) {
        printf("Detected .molink.zip, extracting...\n");
        std::string folderName = selected.displayName.substr(0, selected.displayName.size() - 11);
        std::string destDir = ".\\" + folderName;
        CreateDirectoryA(destDir.c_str(), nullptr);
        int fileCount = 0;
        std::string result = extractFolder(tempPath, destDir, fileCount);
        remove(tempPath.c_str());

        if (result != "ok") {
            printf("Extract failed: %s\n", result.c_str());
            return 1;
        }
        printf("Extracted to: %s (%d file(s))\n", destDir.c_str(), fileCount);
    } else {
        std::string localPath = ".\\" + selected.displayName;
        MoveFileA(tempPath.c_str(), localPath.c_str());
        printf("Downloaded to: %s\n", localPath.c_str());
    }

    return 0;
}
