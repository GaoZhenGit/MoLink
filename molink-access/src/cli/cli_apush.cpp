#include "cli_utils.h"
#include "base64.h"
#include "gitignore.h"
#include "zip_utils.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <windows.h>

namespace fs = std::filesystem;

// ---- 压缩目录 ----
static std::string compressFolder(const std::string& folderPath,
                                   const std::string& zipPath,
                                   GitignoreMatcher* gitSpec) {
    zip::ZipWriter writer;
    if (!writer.open(zipPath))
        return "fail: Cannot create zip file";

    std::error_code ec;
    std::string base = fs::absolute(folderPath, ec).string();
    if (ec) return "fail: Cannot resolve folder path";
    std::replace(base.begin(), base.end(), '\\', '/');
    if (!base.empty() && base.back() != '/') base += '/';

    int fileCount = 0;
    auto it = fs::recursive_directory_iterator(base, ec);
    auto end = fs::recursive_directory_iterator();
    for (; it != end && !ec; ++it) {
        auto& entry = *it;

        std::string absPath = entry.path().string();
        std::replace(absPath.begin(), absPath.end(), '\\', '/');

        std::string relPath;
        if (absPath.size() >= base.size())
            relPath = absPath.substr(base.size());

        bool isDir = entry.is_directory(ec);

        if (relPath.find(".git/") == 0 || relPath == ".git") continue;
        if (relPath == ".gitignore") continue;

        if (gitSpec && gitSpec->hasRules() && gitSpec->isIgnored(relPath, isDir)) {
            if (isDir) it.disable_recursion_pending();
            continue;
        }

        if (isDir) {
            std::string dirEntry = relPath + "/";
            writer.addFile(dirEntry, nullptr, 0);
        } else {
            std::ifstream f(absPath, std::ios::binary);
            if (!f.good()) continue;
            std::string content((std::istreambuf_iterator<char>(f)),
                                 std::istreambuf_iterator<char>());
            if (!writer.addFile(relPath, content.data(), content.size()))
                return "fail: Cannot add file to zip: " + relPath;
            fileCount++;
        }
    }

    if (!writer.close()) return "fail: Cannot finalize zip";
    if (fileCount == 0) return "fail: No files to compress";
    return "ok";
}

// ---- apush 命令 ----
int cmdApush(int argc, char* argv[]) {
    if (argc < 3) {
        printf("Usage: molink apush <path> [--git] [--no-git] [--rdir <remote_dir>]\n");
        return 1;
    }

    std::error_code ec;
    std::string path = fs::absolute(argv[2], ec).string();
    if (ec) {
        printf("Cannot resolve path: %s\n", argv[2]);
        return 1;
    }
    while (!path.empty() && (path.back() == '\\' || path.back() == '/'))
        path.pop_back();
    std::string rdir = kRemoteDir;

    enum { AUTO, ENABLED, DISABLED } gitMode = AUTO;
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--git") == 0) gitMode = ENABLED;
        else if (strcmp(argv[i], "--no-git") == 0) gitMode = DISABLED;
        else if (strcmp(argv[i], "--rdir") == 0 && i + 1 < argc) rdir = argv[++i];
    }

    if (!fs::exists(path)) {
        printf("File/directory not found: %s\n", path.c_str());
        return 1;
    }

    if (sendPipeCmd("status").empty()) {
        printf("Daemon is not running. Use 'molink start' first.\n");
        return 1;
    }

    std::string originalName = fs::path(path).filename().string();
    GitignoreMatcher gitSpec;
    int ignoredCount = 0;

    if (fs::is_directory(path)) {
        bool useGitignore = false;
        std::string gitignorePath;

        if (gitMode == AUTO || gitMode == ENABLED) {
            gitignorePath = findGitignore(path);
            useGitignore = !gitignorePath.empty();
            if (gitMode == ENABLED && !useGitignore) {
                printf("Warning: .gitignore not found\n");
            }
        }

        if (useGitignore) {
            gitSpec.load(gitignorePath);
            std::error_code ec;
            std::string base = fs::absolute(path, ec).string();
            std::replace(base.begin(), base.end(), '\\', '/');
            if (!base.empty() && base.back() != '/') base += '/';
            auto it2 = fs::recursive_directory_iterator(base, ec);
            auto end2 = fs::recursive_directory_iterator();
            for (; it2 != end2 && !ec; ++it2) {
                auto& entry = *it2;
                std::string absPath = entry.path().string();
                std::replace(absPath.begin(), absPath.end(), '\\', '/');
                std::string rel;
                if (absPath.size() >= base.size()) rel = absPath.substr(base.size());
                if (rel.find(".git/") == 0 || rel == ".git" || rel == ".gitignore") continue;
                bool isDir = entry.is_directory(ec);
                if (gitSpec.hasRules() && gitSpec.isIgnored(rel, isDir)) {
                    if (isDir) it2.disable_recursion_pending();
                    ignoredCount++;
                }
            }
            printf(".gitignore detected, ignoring %d path(s)\n", ignoredCount);
        }

        printf("Directory detected, compressing...\n");
        char tempDir[MAX_PATH];
        GetTempPathA(sizeof(tempDir), tempDir);
        std::string zipPath = std::string(tempDir) + originalName + ".molink.zip";

        std::string result = compressFolder(path, zipPath,
                                            (useGitignore ? &gitSpec : nullptr));
        if (result != "ok") {
            printf("%s\n", result.c_str());
            return 1;
        }

        std::string remoteName = kEncodePrefix + base64Encode(originalName + ".molink.zip");
        std::string pipeCmd = "push " + zipPath + " " + rdir + "/" + remoteName;
        auto resp = sendPipeCmd(pipeCmd);
        printf("%s\n", resp.c_str());

        remove(zipPath.c_str());

        if (resp == "ok") {
            printf("Uploaded directory: %s -> %s/%s\n", originalName.c_str(), rdir.c_str(), remoteName.c_str());
            if (ignoredCount > 0) printf("(%d path(s) ignored)\n", ignoredCount);
        }
        return (resp == "ok") ? 0 : 1;

    } else {
        std::string remoteName = kEncodePrefix + base64Encode(originalName);
        std::string pipeCmd = "push " + path + " " + rdir + "/" + remoteName;
        auto resp = sendPipeCmd(pipeCmd);
        printf("%s\n", resp.c_str());
        if (resp == "ok") {
            printf("Uploaded: %s -> %s/%s\n", originalName.c_str(), rdir.c_str(), remoteName.c_str());
        }
        return (resp == "ok") ? 0 : 1;
    }
}
