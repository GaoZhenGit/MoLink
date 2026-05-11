#ifndef FILE_PULL_H
#define FILE_PULL_H

#include <string>

class AdbClient;

// Pull a file from the device to the local filesystem.
// Returns "ok" on success, or "fail: <reason>" on error.
std::string pullFile(AdbClient& client,
                     const std::string& remotePath,
                     const std::string& localPath);

#endif
