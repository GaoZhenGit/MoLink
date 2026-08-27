#ifndef ADB_SYNC_H
#define ADB_SYNC_H

#include <cstdint>
#include <string>
#include <vector>
#include <memory>

// ADB Sync protocol message IDs
constexpr uint32_t SYNC_SEND = 0x444e4553;  // "SEND"
constexpr uint32_t SYNC_RECV = 0x56434552;  // "RECV"
constexpr uint32_t SYNC_DATA = 0x41544144;  // "DATA"
constexpr uint32_t SYNC_DONE = 0x454e4f44;  // "DONE"
constexpr uint32_t SYNC_OKAY = 0x59414b4f;  // "OKAY"
constexpr uint32_t SYNC_FAIL = 0x4c494146;  // "FAIL"
constexpr uint32_t SYNC_QUIT = 0x54495551;  // "QUIT"
constexpr uint32_t SYNC_STAT = 0x54415453;  // "STAT" (response carries mode/size/mtime)

#pragma pack(push, 1)
struct SyncMsg {
    uint32_t id;
    uint32_t data_length;
};
#pragma pack(pop)

class AdbClient;
struct Channel;
using ChannelPtr = std::shared_ptr<Channel>;

// Low-level sync protocol helpers (used by FilePush / FilePull)
bool syncSend(AdbClient& client, ChannelPtr ch,
              const std::string& remotePath, uint32_t mode);
bool syncRecv(AdbClient& client, ChannelPtr ch,
              const std::string& remotePath);
bool syncQuit(AdbClient& client, ChannelPtr ch);

// Query remote file metadata. On success populates *mode, *size, *mtime (Unix seconds).
bool syncStat(AdbClient& client, ChannelPtr ch,
              const std::string& remotePath,
              uint32_t* mode, uint32_t* size, uint32_t* mtime);

// Read a sync response from the channel's dataQueue.
bool syncReadResponse(ChannelPtr ch, SyncMsg& msg,
                      std::vector<uint8_t>& payload, int timeoutMs);

#endif
