#include "named_pipe.h"
#include <cstdio>
#include <cstring>

// 重新挂起连接等待。m_overlapped.hEvent 是 manual-reset 事件，必须手动复位，
// 否则事件一直保持 signaled，daemon 主循环会空转烧掉一个核。
static void RearmConnect(HANDLE pipe, OVERLAPPED& ov) {
    ResetEvent(ov.hEvent);
    for (int attempt = 0; attempt < 10; attempt++) {
        if (ConnectNamedPipe(pipe, &ov)) {
            SetEvent(ov.hEvent);   // 同步连接成功（罕见），视为已连接
            return;
        }
        DWORD err = GetLastError();
        if (err == ERROR_IO_PENDING) {
            return;                // 正常：等待客户端连接
        }
        if (err == ERROR_PIPE_CONNECTED) {
            SetEvent(ov.hEvent);   // 客户端在 Disconnect/Connect 窗口期连上了
            return;
        }
        if (err == ERROR_NO_DATA) {
            // 客户端在窗口期连上又立刻断开（如 CLI 被 Ctrl+C 杀掉）：
            // 断开残留状态后重试，否则事件无人置位，daemon 管道永久失联
            DisconnectNamedPipe(pipe);
            continue;
        }
        printf("PIPE: ConnectNamedPipe failed: %lu\n", err);
        return;
    }
    printf("PIPE: ConnectNamedPipe keeps failing, giving up\n");
}

NamedPipeServer::NamedPipeServer(const std::string& pipeName)
    : m_pipeName(pipeName) {
    memset(&m_overlapped, 0, sizeof(m_overlapped));
}

NamedPipeServer::~NamedPipeServer() {
    stop();
}

HANDLE NamedPipeServer::start() {
    m_overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    m_pipe = CreateNamedPipeA(
        m_pipeName.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
        1,
        4096,
        4096,
        0,
        nullptr);

    if (m_pipe == INVALID_HANDLE_VALUE) {
        printf("PIPE: CreateNamedPipe failed: %lu\n", GetLastError());
        CloseHandle(m_overlapped.hEvent);
        m_overlapped.hEvent = nullptr;
        return nullptr;
    }

    // 异步等待连接
    if (!ConnectNamedPipe(m_pipe, &m_overlapped)) {
        DWORD err = GetLastError();
        if (err == ERROR_IO_PENDING) {
            // 正常：等待中
        } else if (err == ERROR_PIPE_CONNECTED) {
            SetEvent(m_overlapped.hEvent);
        } else {
            printf("PIPE: ConnectNamedPipe failed: %lu\n", err);
            CloseHandle(m_pipe);
            CloseHandle(m_overlapped.hEvent);
            m_pipe = INVALID_HANDLE_VALUE;
            m_overlapped.hEvent = nullptr;
            return nullptr;
        }
    }

    return m_overlapped.hEvent;
}

void NamedPipeServer::processConnection() {
    if (!m_pipe || m_pipe == INVALID_HANDLE_VALUE) return;

    DWORD bytes = 0;
    if (!GetOverlappedResult(m_pipe, &m_overlapped, &bytes, FALSE)) {
        DWORD err = GetLastError();
        if (err != ERROR_IO_INCOMPLETE) {
            // 连接失败，重建
            DisconnectNamedPipe(m_pipe);
            RearmConnect(m_pipe, m_overlapped);
        }
        return;
    }

    // 读取命令
    char buf[4096] = {};
    DWORD read = 0;
    if (ReadFile(m_pipe, buf, sizeof(buf) - 1, &read, nullptr)) {
        std::string cmd(buf, read);
        while (!cmd.empty() && (cmd.back() == '\n' || cmd.back() == '\r'))
            cmd.pop_back();

        std::string response = "unknown\n";
        if (m_handler) {
            response = m_handler(cmd);
        }
        if (response.empty() || response.back() != '\n')
            response += '\n';

        DWORD written = 0;
        WriteFile(m_pipe, response.c_str(), (DWORD)response.size(), &written, nullptr);
    }

    FlushFileBuffers(m_pipe);
    DisconnectNamedPipe(m_pipe);
    RearmConnect(m_pipe, m_overlapped);
}

void NamedPipeServer::stop() {
    if (m_pipe && m_pipe != INVALID_HANDLE_VALUE) {
        DisconnectNamedPipe(m_pipe);
        CloseHandle(m_pipe);
        m_pipe = INVALID_HANDLE_VALUE;
    }
    if (m_overlapped.hEvent) {
        CloseHandle(m_overlapped.hEvent);
        m_overlapped.hEvent = nullptr;
    }
}
