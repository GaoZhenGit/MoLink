#include <winsock2.h>
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <memory>
#include <vector>
#include <sstream>
#include <filesystem>
#include <shlobj.h>
#include <io.h>
#include "adb/adb_client.h"
#include "forward/forwarder.h"
#include "cli/named_pipe.h"
#include "transfer/file_push.h"
#include "transfer/file_pull.h"
#include "transfer/file_list.h"
#include "adb/adb_shell.h"
#include "base64.h"
#include "gitignore.h"
#include "zip_utils.h"

// ---- 全局（Ctrl+C 用） ----
static Forwarder* g_forwarder = nullptr;
static BOOL WINAPI ctrlHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
        printf("\nShutting down...\n");
        if (g_forwarder) g_forwarder->stop();
        return TRUE;
    }
    return FALSE;
}

// ---- PID 文件 ----
static std::string getPidPath() {
    char dir[MAX_PATH];
    GetModuleFileNameA(nullptr, dir, sizeof(dir));
    char* lastSlash = strrchr(dir, '\\');
    if (!lastSlash) lastSlash = strrchr(dir, '/');
    if (lastSlash) *(lastSlash + 1) = '\0';
    return std::string(dir) + "molinkd.pid";
}

static void writePidFile() {
    auto path = getPidPath();
    FILE* f = fopen(path.c_str(), "w");
    if (f) {
        fprintf(f, "%lu", GetCurrentProcessId());
        fclose(f);
    }
}

static DWORD readPidFile() {
    auto path = getPidPath();
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return 0;
    DWORD pid = 0;
    fscanf(f, "%lu", &pid);
    fclose(f);
    return pid;
}

static void removePidFile() {
    auto path = getPidPath();
    remove(path.c_str());
}

// ---- 单实例 ----
static HANDLE g_daemonMutex = nullptr;

static bool tryAcquireDaemonLock() {
    g_daemonMutex = CreateMutexW(nullptr, TRUE, L"Global\\MoLinkDaemon");
    return (GetLastError() != ERROR_ALREADY_EXISTS);
}

static void releaseDaemonLock() {
    if (g_daemonMutex) {
        ReleaseMutex(g_daemonMutex);
        CloseHandle(g_daemonMutex);
        g_daemonMutex = nullptr;
    }
}

// ---- Named Pipe 客户端 ----
static std::string sendPipeCmd(const std::string& cmd) {
    HANDLE pipe = CreateFileA("\\\\.\\pipe\\molink",
        GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) return {};

    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);

    std::string msg = cmd + "\n";
    DWORD written = 0;
    WriteFile(pipe, msg.c_str(), (DWORD)msg.size(), &written, nullptr);

    char buf[4096] = {};
    DWORD read = 0;
    ReadFile(pipe, buf, sizeof(buf) - 1, &read, nullptr);
    CloseHandle(pipe);

    std::string result(buf, read);
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
        result.pop_back();
    return result;
}

// ---- 命令实现 ----

static int cmdDevices() {
    // 1. 密钥文件
    char appdata[MAX_PATH] = {};
    if (SHGetFolderPathA(nullptr, CSIDL_PROFILE, nullptr, 0, appdata) == S_OK) {
        printf("Keys: %s\\.android\n", appdata);
        auto exists = [](const std::string& p) {
            return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES;
        };
        std::string base = std::string(appdata) + "\\.android\\";
        printf("  molink_key.bin : %s\n", exists(base + "molink_key.bin") ? "found" : "missing");
        printf("  adbkey         : %s\n", exists(base + "adbkey") ? "found" : "missing");
        printf("  adbkey.pub     : %s\n", exists(base + "adbkey.pub") ? "found" : "missing");
    }

    // 2. 静默探测（抑制 USB/ADB/RSA 内部 debug 输出）
    fflush(stdout);
    int saved = _dup(1);
    freopen("NUL", "w", stdout);

    auto devices = UsbDevice::discover();

    AdbRsa rsa;
    bool keyReady = false;
    if (appdata[0]) {
        std::string kp = std::string(appdata) + "\\.android\\molink_key.bin";
        if (!rsa.loadKey(kp)) {
            std::string ak = std::string(appdata) + "\\.android\\adbkey";
            if (rsa.loadPkcs8(ak)) keyReady = true;
        } else keyReady = true;
    }

    struct DevResult { std::string serial; bool authorized; };
    std::vector<DevResult> results;

    for (auto& dev : devices) {
        DevResult r;
        if (!dev.open()) { results.push_back(r); continue; }
        dev.clearHalt(dev.getReadEndpoint());
        dev.clearHalt(dev.getWriteEndpoint());
        dev.drainRead();
        r.serial = dev.getSerial();

        // 快速握手（5s 超时 → 未授权）
        r.authorized = false;
        if (keyReady) {
            AdbTransport tr(dev);
            uint32_t maxdata = MAX_PAYLOAD_V2;
            tr.send(A_CNXN, A_VERSION, maxdata, "host::", 5);
            AdbMessage msg;
            std::vector<uint8_t> data;
            // 单轮：等 5s，期望直接 A_CNXN 或签名后 A_CNXN
            if (tr.recv(msg, data, 5000)) {
                if (msg.command == A_CNXN) {
                    r.authorized = true;
                } else if (msg.command == A_AUTH && msg.arg0 == AUTH_TOKEN) {
                    auto sig = rsa.signToken(data.data(), data.size());
                    if (!sig.empty()) {
                        tr.send(A_AUTH, AUTH_SIGNATURE, 0, sig.data(), (uint32_t)sig.size());
                        if (tr.recv(msg, data, 2000)) {
                            if (msg.command == A_CNXN) r.authorized = true;
                        }
                    }
                }
            }
        }

        dev.close();
        results.push_back(r);
    }

    fflush(stdout);
    _dup2(saved, 1);
    _close(saved);

    // 3. 输出
    if (devices.empty()) { printf("No ADB devices found.\n"); return 1; }
    printf("\n%-4s %-22s %s\n", "#", "SERIAL", "AUTH");
    printf("%-4s %-22s %s\n", "---", "----------------------", "----");
    for (size_t i = 0; i < results.size(); i++) {
        printf("%-4zu %-22s %s\n", i,
               results[i].serial.empty() ? "-" : results[i].serial.c_str(),
               results[i].serial.empty() ? "-" : (results[i].authorized ? "yes" : "no"));
    }
    return 0;
}

static int cmdStatus() {
    auto resp = sendPipeCmd("status");
    if (resp.empty()) {
        printf("Daemon is not running.\n");
        return 1;
    }
    printf("%s\n", resp.c_str());
    return 0;
}

static int cmdStop() {
    auto resp = sendPipeCmd("stop");
    if (resp.empty()) {
        printf("Daemon is not running.\n");
        return 1;
    }
    if (resp == "ok") {
        printf("Daemon stopped.\n");
        return 0;
    }

    // 超时强杀（但 sendPipeCmd 已经等了 ReadFile，这里补充 TerminateProcess）
    printf("Daemon unresponsive, force killing...\n");
    DWORD pid = readPidFile();
    if (pid) {
        HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
        if (hProc) {
            TerminateProcess(hProc, 1);
            CloseHandle(hProc);
            printf("Daemon killed (pid=%lu).\n", pid);
            removePidFile();
            return 0;
        }
    }
    printf("Cannot kill daemon.\n");
    return 1;
}

// ---- forward 命令 ----
static int cmdForward(int argc, char* argv[]) {
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

// ---- apush / apull 辅助 ----

namespace fs = std::filesystem;

static const char* kRemoteDir = "/sdcard/tmp";
static const char* kEncodePrefix = "b64_";

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

        // 跳过 .git
        if (relPath.find(".git/") == 0 || relPath == ".git") continue;
        // 跳过 .gitignore 自身
        if (relPath == ".gitignore") continue;

        // 应用 gitignore 规则
        if (gitSpec && gitSpec->hasRules() && gitSpec->isIgnored(relPath, isDir)) {
            if (isDir) it.disable_recursion_pending();
            continue;
        }

        if (isDir) {
            // 目录条目
            std::string dirEntry = relPath + "/";
            writer.addFile(dirEntry, nullptr, 0);
        } else {
            // 读取文件
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

static std::string extractFolder(const std::string& zipPath,
                                  const std::string& destDir,
                                  int& fileCount) {
    if (zip::extractZip(zipPath, destDir, &fileCount))
        return "ok";
    return "fail: Cannot extract zip file";
}

// ---- apush 命令 ----
static int cmdApush(int argc, char* argv[]) {
    if (argc < 3) {
        printf("Usage: molink apush <path> [--git] [--no-git] [--rdir <remote_dir>]\n");
        return 1;
    }

    std::string path = argv[2];
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
        // 检测 .gitignore
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
            // 统计忽略的文件数
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

        // 压缩
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

        // push
        std::string remoteName = kEncodePrefix + base64Encode(originalName + ".molink.zip");
        std::string pipeCmd = "push " + zipPath + " " + rdir + "/" + remoteName;
        auto resp = sendPipeCmd(pipeCmd);
        printf("%s\n", resp.c_str());

        // 清理临时文件
        remove(zipPath.c_str());

        if (resp == "ok") {
            printf("Uploaded directory: %s -> %s/%s\n", originalName.c_str(), rdir.c_str(), remoteName.c_str());
            if (ignoredCount > 0) printf("(%d path(s) ignored)\n", ignoredCount);
        }
        return (resp == "ok") ? 0 : 1;

    } else {
        // 单文件
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

// ---- apull 命令 ----
static int cmdApull(int argc, char* argv[]) {
    std::string rdir = kRemoteDir;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--rdir") == 0 && i + 1 < argc) rdir = argv[++i];
    }

    if (sendPipeCmd("status").empty()) {
        printf("Daemon is not running. Use 'molink start' first.\n");
        return 1;
    }

    // 获取远程文件列表
    std::string lsCmd = "ls " + rdir;
    std::string lsOutput = sendPipeCmd(lsCmd);
    if (lsOutput.empty() || lsOutput.find("fail:") == 0) {
        printf("Directory not found or empty: %s\n", rdir.c_str());
        return 1;
    }

    // 解析 ls -la 输出
    struct RemoteFile {
        std::string rawName;
        std::string displayName;
        bool isZip;
    };
    std::vector<RemoteFile> files;

    std::istringstream ss(lsOutput);
    std::string line;
    while (std::getline(ss, line)) {
        if (line.empty() || line.find("total ") == 0) continue;
        if (!line.empty() && (line[0] == 'd' || line[0] == 'c' || line[0] == 'l' || line[0] == 't'))
            continue;

        std::istringstream ls(line);
        std::string token, lastToken;
        while (ls >> token) lastToken = token;
        if (lastToken.empty()) continue;

        if (lastToken.find(kEncodePrefix) != 0) continue;

        RemoteFile rf;
        rf.rawName = lastToken;
        std::string encoded = lastToken.substr(strlen(kEncodePrefix));
        rf.displayName = base64Decode(encoded);
        rf.isZip = (rf.displayName.size() >= 11 &&
                    rf.displayName.substr(rf.displayName.size() - 11) == ".molink.zip");

        files.push_back(rf);
    }

    if (files.empty()) {
        printf("No files available for download\n");
        return 1;
    }

    // 交互菜单
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

    // 拉取文件
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

// ---- del 命令 ----
static int cmdDel(int argc, char* argv[]) {
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

// ---- adel 命令 ----
static int cmdAdel(int argc, char* argv[]) {
    std::string rdir = kRemoteDir;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--rdir") == 0 && i + 1 < argc) rdir = argv[++i];
    }

    if (sendPipeCmd("status").empty()) {
        printf("Daemon is not running. Use 'molink start' first.\n");
        return 1;
    }

    // List remote files
    std::string lsCmd = "ls " + rdir;
    std::string lsOutput = sendPipeCmd(lsCmd);
    if (lsOutput.empty() || lsOutput.find("fail:") == 0) {
        printf("Directory not found or empty: %s\n", rdir.c_str());
        return 1;
    }

    // Parse ls -la output, filter b64_ files
    struct RemoteFile {
        std::string rawName;
        std::string displayName;
        bool isZip;
    };
    std::vector<RemoteFile> files;

    std::istringstream ss(lsOutput);
    std::string line;
    while (std::getline(ss, line)) {
        if (line.empty() || line.find("total ") == 0) continue;
        if (!line.empty() && (line[0] == 'd' || line[0] == 'c' || line[0] == 'l' || line[0] == 't'))
            continue;

        std::istringstream ls(line);
        std::string token, lastToken;
        while (ls >> token) lastToken = token;
        if (lastToken.empty()) continue;

        if (lastToken.find(kEncodePrefix) != 0) continue;

        RemoteFile rf;
        rf.rawName = lastToken;
        std::string encoded = lastToken.substr(strlen(kEncodePrefix));
        rf.displayName = base64Decode(encoded);
        rf.isZip = (rf.displayName.size() >= 11 &&
                    rf.displayName.substr(rf.displayName.size() - 11) == ".molink.zip");

        files.push_back(rf);
    }

    if (files.empty()) {
        printf("No files to delete\n");
        return 1;
    }

    // Interactive menu
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

    // Remove trailing newline
    size_t len = strlen(input);
    while (len > 0 && (input[len-1] == '\n' || input[len-1] == '\r'))
        input[--len] = '\0';

    if (len == 0) {
        printf("Invalid input\n");
        return 1;
    }

    int deleted = 0;
    if (input[0] == 'a' || input[0] == 'A') {
        // Delete all
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

// ---- 实际 daemon 入口（由 cmdStart 通过 CreateProcess 调起） ----
static int daemonMain(uint16_t localPort, uint16_t remotePort,
                      const std::string& serial) {
    if (!tryAcquireDaemonLock()) {
        printf("Daemon is already running.\n");
        return 1;
    }

    // 日志重定向
    char exeDir[MAX_PATH];
    GetModuleFileNameA(nullptr, exeDir, sizeof(exeDir));
    char* lastSlash = strrchr(exeDir, '\\');
    if (!lastSlash) lastSlash = strrchr(exeDir, '/');
    if (lastSlash) *(lastSlash + 1) = '\0';
    std::string logPath = std::string(exeDir) + "molinkd.log";

    freopen(logPath.c_str(), "w", stdout);
    freopen(logPath.c_str(), "a", stderr);
    setvbuf(stdout, nullptr, _IONBF, 0);

    printf("MoLink daemon starting (pid=%lu)...\n", GetCurrentProcessId());

    AdbClient client;
    HANDLE hDisconnectEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    client.setDisconnectEvent(hDisconnectEvent);

    if (!client.connect(serial)) {
        printf("FAIL: Cannot connect to device\n");
        CloseHandle(hDisconnectEvent);
        releaseDaemonLock();
        return 1;
    }
    printf("Device: %s\n", client.getSerial().c_str());

    // Forwarder 按需创建，启动时不自动转发
    std::unique_ptr<Forwarder> forwarder;

    HANDLE hStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    NamedPipeServer pipe("\\\\.\\pipe\\molink");
    pipe.setHandler([&](const std::string& cmd) -> std::string {
        if (cmd == "stop") {
            SetEvent(hStopEvent);
            return "ok";
        }
        if (cmd == "status") {
            char buf[256];
            const char* stateStr = (client.getState() == DaemonState::CONNECTED)
                ? "connected" : "disconnected";
            if (forwarder) {
                snprintf(buf, sizeof(buf),
                         "%s  serial=%s  forwarding=%u->%u  connections=%d",
                         stateStr,
                         client.getSerial().c_str(),
                         forwarder->getLocalPort(),
                         forwarder->getRemotePort(),
                         forwarder->getConnectionCount());
            } else {
                snprintf(buf, sizeof(buf),
                         "%s  serial=%s  forwarding=off",
                         stateStr,
                         client.getSerial().c_str());
            }
            return buf;
        }
        // File transfer commands
        if (cmd.rfind("push ", 0) == 0) {
            std::string args = cmd.substr(5);
            size_t space = args.find(' ');
            if (space == std::string::npos) return "fail: Usage: push <local> <remote>";
            std::string local = args.substr(0, space);
            std::string remote = args.substr(space + 1);
            return pushFile(client, local, remote);
        }
        if (cmd.rfind("pull ", 0) == 0) {
            std::string args = cmd.substr(5);
            size_t space = args.find(' ');
            if (space == std::string::npos) return "fail: Usage: pull <remote> <local>";
            std::string remote = args.substr(0, space);
            std::string local = args.substr(space + 1);
            return pullFile(client, remote, local);
        }
        if (cmd.rfind("forward ", 0) == 0) {
            std::string args = cmd.substr(8);
            size_t space = args.find(' ');
            if (space == std::string::npos) return "fail: Usage: forward <localPort> <remotePort>";
            uint16_t lp = (uint16_t)atoi(args.substr(0, space).c_str());
            uint16_t rp = (uint16_t)atoi(args.substr(space + 1).c_str());

            if (forwarder) {
                forwarder->stop();
                forwarder.reset();
            }

            forwarder = std::make_unique<Forwarder>(client, lp, rp);
            if (!forwarder->start()) {
                forwarder.reset();
                return "fail: Cannot start forwarding";
            }
            char buf[128];
            snprintf(buf, sizeof(buf), "ok: forwarding 127.0.0.1:%u -> device tcp:%u", lp, rp);
            return buf;
        }
        if (cmd.rfind("ls", 0) == 0) {
            std::string path;
            if (cmd.size() > 2) {
                path = cmd.substr(3);
            }
            return listFiles(client, path);
        }
        if (cmd.rfind("del ", 0) == 0) {
            std::string path = cmd.substr(4);
            if (path.empty()) return "fail: Usage: del <remote_path>";
            std::string rmCmd = "rm -f '" + path + "'";
            std::string output = shellCommand(client, rmCmd);
            if (output.empty()) return "ok";
            return "fail: " + output;
        }
        if (cmd.rfind("shell ", 0) == 0) {
            std::string shellCmd = cmd.substr(6);
            if (shellCmd.empty()) return "fail: Usage: shell <command>";
            return shellCommand(client, shellCmd);
        }
        return "unknown";
    });

    HANDLE hPipeEvent = pipe.start();
    if (!hPipeEvent) {
        printf("FAIL: Cannot create named pipe\n");
        if (forwarder) forwarder->stop();
        CloseHandle(hStopEvent);
        CloseHandle(hDisconnectEvent);
        releaseDaemonLock();
        return 1;
    }

    writePidFile();
    printf("Daemon ready. Use 'molink stop' to stop.\n");

    // 保存串号用于重连（disconnect 会清空 m_serial）
    std::string lastSerial = serial;

    auto transitionToDisconnected = [&]() {
        ResetEvent(hDisconnectEvent);
        client.setState(DaemonState::DISCONNECTED);
        if (forwarder) forwarder->pause();
        lastSerial = client.getSerial(); // 在 disconnect 前保存
        client.disconnect();
        printf("Daemon: Device disconnected (serial=%s), waiting for reconnect...\n",
               lastSerial.c_str());
    };

    auto tryReconnect = [&]() -> bool {
        printf("Daemon: Attempting reconnect with serial=%s...\n",
               lastSerial.empty() ? "(auto)" : lastSerial.c_str());
        if (client.connect(lastSerial)) {
            if (forwarder) forwarder->resume();
            client.setState(DaemonState::CONNECTED);
            printf("Daemon: Reconnected\n");
            return true;
        }
        return false;
    };

    bool running = true;
    DWORD lastReconnectAttempt = 0; // tick count of last attempt

    while (running) {
        DaemonState state = client.getState();
        // Use short timeout so pipe events don't starve reconnect checks
        DWORD timeout = 500; // check every 500ms

        HANDLE handles[3] = { hStopEvent, hPipeEvent, hDisconnectEvent };
        DWORD ret = WaitForMultipleObjects(3, handles, FALSE, timeout);

        if (ret == WAIT_OBJECT_0) {
            // hStopEvent
            client.setState(DaemonState::STOPPING);
            running = false;
        }
        else if (ret == WAIT_OBJECT_0 + 1) {
            // hPipeEvent
            pipe.processConnection();
        }
        else if (ret == WAIT_OBJECT_0 + 2) {
            // hDisconnectEvent — device removed
            transitionToDisconnected();
            lastReconnectAttempt = GetTickCount(); // reset timer on fresh disconnect
        }

        // Try reconnect if disconnected and enough time passed
        if (state == DaemonState::DISCONNECTED) {
            DWORD now = GetTickCount();
            if (now - lastReconnectAttempt >= 1000) {
                lastReconnectAttempt = now;
                tryReconnect();
            }
        }
    }

    printf("Daemon stopping...\n");
    pipe.stop();
    if (forwarder) forwarder->stop();
    client.disconnect();
    CloseHandle(hStopEvent);
    CloseHandle(hDisconnectEvent);
    removePidFile();
    releaseDaemonLock();
    return 0;
}

// ---- molink start 命令 ----
static int cmdStart(uint16_t localPort, uint16_t remotePort,
                    const std::string& serial) {
    // 如果已有 daemon，先停旧再启新
    if (!sendPipeCmd("status").empty()) {
        printf("Daemon is running, restarting...\n");
        auto resp = sendPipeCmd("stop");
        if (resp != "ok") {
            printf("FAIL: Cannot stop existing daemon\n");
            return 1;
        }
        // 等旧进程完全退出
        Sleep(1500);
    }

    // 拼命令行: molink.exe --daemon <args>
    char exePath[MAX_PATH];
    GetModuleFileNameA(nullptr, exePath, sizeof(exePath));

    std::string cmdLine = "\"" + std::string(exePath) + "\" --daemon";
    cmdLine += " -p " + std::to_string(localPort);
    cmdLine += " -r " + std::to_string(remotePort);
    if (!serial.empty()) cmdLine += " -s " + serial;

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};

    if (!CreateProcessA(exePath, &cmdLine[0],
                        nullptr, nullptr, FALSE,
                        DETACHED_PROCESS | CREATE_NO_WINDOW,
                        nullptr, nullptr, &si, &pi)) {
        printf("FAIL: Cannot spawn daemon process: %lu\n", GetLastError());
        return 1;
    }

    CloseHandle(pi.hThread);

    // 等待 daemon 启动完成（最多 8 秒）
    printf("Starting daemon (pid=%lu)...\n", pi.dwProcessId);
    bool ok = false;
    for (int i = 0; i < 16; i++) {
        Sleep(500);
        auto resp = sendPipeCmd("status");
        if (!resp.empty()) {
            printf("%s\n", resp.c_str());
            ok = true;
            break;
        }
        // 进程是否已退出（启动失败）
        DWORD exitCode = STILL_ACTIVE;
        if (GetExitCodeProcess(pi.hProcess, &exitCode) && exitCode != STILL_ACTIVE) {
            printf("FAIL: Daemon exited with code %lu\n", exitCode);
            break;
        }
    }
    if (!ok) {
        printf("FAIL: Daemon did not start. Check molinkd.log\n");
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess);
        return 1;
    }

    CloseHandle(pi.hProcess);
    return 0;
}

// ---- 前台模式 ----
static int foregroundMode(uint16_t localPort, uint16_t remotePort,
                           const std::string& serial) {
    printf("=== MoLink Access ===\n");
    printf("Local: 127.0.0.1:%u -> Device tcp:%u\n", localPort, remotePort);

    AdbClient client;
    HANDLE hDisconnectEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    client.setDisconnectEvent(hDisconnectEvent);

    if (!client.connect(serial)) {
        printf("FAIL: Cannot connect to device\n");
        CloseHandle(hDisconnectEvent);
        return 1;
    }
    printf("Device: %s\n", client.getSerial().c_str());

    Forwarder forwarder(client, localPort, remotePort);
    if (!forwarder.start()) {
        printf("FAIL: Cannot start forwarder\n");
        CloseHandle(hDisconnectEvent);
        return 1;
    }

    g_forwarder = &forwarder;
    SetConsoleCtrlHandler(ctrlHandler, TRUE);
    printf("Forwarding active. Press Ctrl+C to stop.\n");

    while (forwarder.isRunning()) {
        DWORD ret = WaitForSingleObject(hDisconnectEvent, 500);
        if (ret == WAIT_OBJECT_0) {
            // Device disconnected
            ResetEvent(hDisconnectEvent);
            printf("\n[Device disconnected, reconnecting...]\n");
            forwarder.pause();
            client.disconnect();

            bool reconnected = false;
            while (!reconnected && forwarder.isRunning()) {
                Sleep(1000);
                printf("[Reconnect attempt...]\n");
                if (client.connect(serial)) {
                    forwarder.resume();
                    reconnected = true;
                    printf("[Reconnected]\n");
                }
            }
            if (!reconnected) break; // Ctrl+C during reconnect
        }
    }
    CloseHandle(hDisconnectEvent);

    printf("MoLink stopped.\n");
    return 0;
}

// ---- 帮助 ----
static void printUsage() {
    printf("MoLink Access - ADB USB Proxy\n\n"
           "Usage:\n"
           "  molink run      [options]         Run in foreground\n"
           "  molink start    [options]         Start daemon in background\n"
           "  molink stop                       Stop running daemon\n"
           "  molink forward  [options]         Start port forwarding (requires daemon)\n"
           "  molink devices                    List ADB devices\n"
           "  molink status                     Show daemon status\n"
           "  molink push     <local> <remote>  Upload file to device\n"
           "  molink pull     <remote> <local>  Download file from device\n"
           "  molink del      <remote_path>      Delete file on device\n"
           "  molink adel     [options]          Interactive auto delete\n"
           "  molink ls       [remote_path]      List device directory\n"
           "  molink apush    <path> [options]   Auto push (dir->zip, .gitignore)\n"
           "  molink apull    [options]          Interactive auto pull (unzip)\n"
           "  molink --help                     Show this help\n\n"
           "Options:\n"
           "  --port, -p <port>                 Local TCP port (default: 1080)\n"
           "  --rport, -r <port>                Remote device port (default: 1081)\n"
           "  --serial, -s <sn>                 Device serial number\n");
}

// ---- entry ----
int main(int argc, char* argv[]) {
    setvbuf(stdout, nullptr, _IONBF, 0);

    // 无参数 → 帮助
    if (argc < 2) {
        printUsage();
        return 0;
    }

    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        printUsage();
        return 0;
    }

    // 命令模式
    if (strcmp(argv[1], "devices") == 0) return cmdDevices();
    if (strcmp(argv[1], "status") == 0)  return cmdStatus();
    if (strcmp(argv[1], "stop") == 0)    return cmdStop();
    if (strcmp(argv[1], "forward") == 0) return cmdForward(argc, argv);

    // push/pull/ls CLI dispatch
    if (strcmp(argv[1], "push") == 0) {
        if (argc < 4) {
            printf("Usage: molink push <local_file> <remote_path>\n");
            return 1;
        }
        std::string cmd = std::string("push ") + argv[2] + " " + argv[3];
        auto resp = sendPipeCmd(cmd);
        if (resp.empty()) {
            printf("Daemon is not running. Use 'molink start' first.\n");
            return 1;
        }
        printf("%s\n", resp.c_str());
        return (resp == "ok") ? 0 : 1;
    }

    if (strcmp(argv[1], "pull") == 0) {
        if (argc < 4) {
            printf("Usage: molink pull <remote_path> <local_file>\n");
            return 1;
        }
        std::string cmd = std::string("pull ") + argv[2] + " " + argv[3];
        auto resp = sendPipeCmd(cmd);
        if (resp.empty()) {
            printf("Daemon is not running. Use 'molink start' first.\n");
            return 1;
        }
        printf("%s\n", resp.c_str());
        return (resp == "ok") ? 0 : 1;
    }

    if (strcmp(argv[1], "ls") == 0) {
        std::string path = (argc >= 3) ? argv[2] : "/sdcard/";
        std::string cmd = "ls " + path;
        auto resp = sendPipeCmd(cmd);
        if (resp.empty()) {
            printf("Daemon is not running. Use 'molink start' first.\n");
            return 1;
        }
        printf("%s", resp.c_str());
        return 0;
    }

    if (strcmp(argv[1], "del") == 0)   return cmdDel(argc, argv);
    if (strcmp(argv[1], "adel") == 0)  return cmdAdel(argc, argv);
    if (strcmp(argv[1], "apush") == 0) return cmdApush(argc, argv);
    if (strcmp(argv[1], "apull") == 0) return cmdApull(argc, argv);

    // 解析 options
    uint16_t localPort = 1080;
    uint16_t remotePort = 1081;
    std::string serial;
    bool isRun = (strcmp(argv[1], "run") == 0);
    bool isStart = (strcmp(argv[1], "start") == 0);
    bool isDaemon = (strcmp(argv[1], "--daemon") == 0);
    int startIdx = (isRun || isStart || isDaemon) ? 2 : 1;

    for (int i = startIdx; i < argc; i++) {
        if ((strcmp(argv[i], "--port") == 0 || strcmp(argv[i], "-p") == 0) && i + 1 < argc)
            localPort = (uint16_t)atoi(argv[++i]);
        else if ((strcmp(argv[i], "--rport") == 0 || strcmp(argv[i], "-r") == 0) && i + 1 < argc)
            remotePort = (uint16_t)atoi(argv[++i]);
        else if ((strcmp(argv[i], "--serial") == 0 || strcmp(argv[i], "-s") == 0) && i + 1 < argc)
            serial = argv[++i];
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printUsage(); return 0;
        }
    }

    if (isStart)  return cmdStart(localPort, remotePort, serial);
    if (isDaemon) return daemonMain(localPort, remotePort, serial);
    if (isRun)    return foregroundMode(localPort, remotePort, serial);

    printf("Unknown command: %s\n", argv[1]);
    printUsage();
    return 1;
}
