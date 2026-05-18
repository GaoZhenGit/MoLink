#ifndef FILE_LIST_H
#define FILE_LIST_H

#include <string>

class AdbClient;

// List a directory on the device.
// Returns directory listing as string, or "fail: <reason>".
std::string listFiles(AdbClient& client, const std::string& remotePath);

#endif
