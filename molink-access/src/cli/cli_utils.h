#ifndef CLI_UTILS_H
#define CLI_UTILS_H

#include <string>
#include <vector>
#include <cstdio>

// ---- 公共常量 ----
const char kRemoteDir[] = "/sdcard/tmp";
const char kEncodePrefix[] = "b64_";

// ---- Named Pipe 客户端（由 main.cpp 实现） ----
std::string sendPipeCmd(const std::string& cmd);

// ---- 远程文件列表解析（apull / adel 共用） ----
struct RemoteFile {
    std::string rawName;      // 远程文件名（含 b64_ 前缀）
    std::string displayName;  // 解码后的展示名
    bool isZip = false;
};

// 从 ls 输出解析 b64_ 文件列表
std::vector<RemoteFile> listRemoteFiles(const std::string& lsOutput);

// ---- CLI 命令 ----
int cmdApush(int argc, char* argv[]);
int cmdApull(int argc, char* argv[]);
int cmdAdel(int argc, char* argv[]);
int cmdDevices();
int cmdDel(int argc, char* argv[]);
int cmdForward(int argc, char* argv[]);
int cmdAuth(int argc, char* argv[]);
int cmdInstall(int argc, char* argv[]);

#endif
