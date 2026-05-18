#ifndef FILE_PUSH_H
#define FILE_PUSH_H

#include <string>

class AdbClient;

// Push a local file to the device.
// Returns "ok" on success, or "fail: <reason>" on error.
std::string pushFile(AdbClient& client,
                     const std::string& localPath,
                     const std::string& remotePath);

#endif
