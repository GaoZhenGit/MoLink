#include "adb_shell.h"
#include "adb_client.h"
#include "adb_reader.h"
#include "log.h"
#include <cstdio>
#include <cstdlib>
#include <chrono>

std::string shellCommand(AdbClient& client, const std::string& command) {
    std::string dest = "shell:" + command;
    auto ch = client.openChannel(dest);
    if (!ch) {
        LOG_ERROR("SHELL", "failed to open channel: %s", dest.c_str());
        return "fail: Could not open shell channel";
    }

    std::string output;
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::seconds(30);

    while (std::chrono::steady_clock::now() < deadline) {
        std::unique_lock<std::mutex> lock(ch->mtx);
        if (!ch->dataQueue.empty()) {
            auto data = std::move(ch->dataQueue.front());
            ch->dataQueue.pop();
            lock.unlock();
            output.append((const char*)data.data(), data.size());
            continue;
        }
        if (ch->closed) {
            break;
        }
        ch->cv.wait_for(lock, std::chrono::milliseconds(100));
    }

    client.closeChannel(ch);
    return output;
}

uint32_t getRemoteMtime(AdbClient& client, const std::string& remotePath) {
    // 用单引号包路径，避免空格/中文路径被 shell 拆分或转义
    std::string cmd = "stat -c '%Y' '" + remotePath + "' 2>/dev/null";
    std::string out = shellCommand(client, cmd);

    // 截尾：shell 输出可能带 \n / \r
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' ||
                            out.back() == ' ' || out.back() == '\t')) {
        out.pop_back();
    }

    if (out.empty()) return 0;

    char* endp = nullptr;
    long long v = strtoll(out.c_str(), &endp, 10);
    if (endp == out.c_str() || v <= 0) return 0;  // stat 失败/解析失败
    return (uint32_t)v;
}
