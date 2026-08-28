#ifndef ADB_SHELL_H
#define ADB_SHELL_H

#include <cstdint>
#include <string>

class AdbClient;

// Execute a shell command on the device via "shell:<cmd>" channel.
// Returns the stdout output as a string, or empty on failure.
std::string shellCommand(AdbClient& client, const std::string& command);

// Query remote file mtime (Unix seconds) via `stat -c '%Y' <path>`.
// Returns 0 on failure (file not found, timeout, parse error).
// Use shell: channel — independent of sync: protocol, so safe to call
// from file_pull without polluting the sync channel state.
uint32_t getRemoteMtime(AdbClient& client, const std::string& remotePath);

#endif
