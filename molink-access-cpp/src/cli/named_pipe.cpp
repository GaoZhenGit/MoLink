#include "named_pipe.h"
#include <cstdio>
#include <cstring>

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
            ConnectNamedPipe(m_pipe, &m_overlapped);
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
    ConnectNamedPipe(m_pipe, &m_overlapped);
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
