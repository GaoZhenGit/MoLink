#ifndef NAMED_PIPE_H
#define NAMED_PIPE_H

#include <windows.h>
#include <string>
#include <functional>
#include <atomic>

class NamedPipeServer {
public:
    using CommandHandler = std::function<std::string(const std::string&)>;

    NamedPipeServer(const std::string& pipeName);
    ~NamedPipeServer();

    void setHandler(CommandHandler handler) { m_handler = handler; }

    // 返回 overlapped event handle 供 WaitForMultipleObjects
    HANDLE start();

    // 处理一个连接（非阻塞，需在 event 触发后调用）
    void processConnection();

    void stop();

private:
    std::string m_pipeName;
    HANDLE m_pipe = INVALID_HANDLE_VALUE;
    OVERLAPPED m_overlapped;
    CommandHandler m_handler;
};

#endif
