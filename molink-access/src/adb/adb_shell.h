#ifndef ADB_SHELL_H
#define ADB_SHELL_H

#include <string>

class AdbClient;

// Execute a shell command on the device via "shell:<cmd>" channel.
// Returns the stdout output as a string, or empty on failure.
std::string shellCommand(AdbClient& client, const std::string& command);

#endif
