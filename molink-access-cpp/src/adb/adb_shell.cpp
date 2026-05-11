#include "adb_shell.h"
#include "adb_client.h"
#include "adb_reader.h"
#include <cstdio>
#include <chrono>

std::string shellCommand(AdbClient& client, const std::string& command) {
    std::string dest = "shell:" + command;
    auto ch = client.openChannel(dest);
    if (!ch) {
        printf("SHELL: Failed to open channel: %s\n", dest.c_str());
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
