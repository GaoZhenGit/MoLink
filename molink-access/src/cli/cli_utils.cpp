#include "cli_utils.h"
#include "base64.h"

#include <cstring>
#include <sstream>

// ---- 从 ls 输出解析 b64_ 文件列表 ----
std::vector<RemoteFile> listRemoteFiles(const std::string& lsOutput) {
    std::vector<RemoteFile> files;

    std::istringstream ss(lsOutput);
    std::string line;
    while (std::getline(ss, line)) {
        if (line.empty() || line.find("total ") == 0) continue;
        if (!line.empty() && (line[0] == 'd' || line[0] == 'c' || line[0] == 'l' || line[0] == 't'))
            continue;

        std::istringstream ls(line);
        std::string token, lastToken;
        while (ls >> token) lastToken = token;
        if (lastToken.empty()) continue;

        if (lastToken.find(kEncodePrefix) != 0) continue;

        RemoteFile rf;
        rf.rawName = lastToken;
        std::string encoded = lastToken.substr(strlen(kEncodePrefix));
        rf.displayName = base64Decode(encoded);
        rf.isZip = (rf.displayName.size() >= 11 &&
                    rf.displayName.substr(rf.displayName.size() - 11) == ".molink.zip");

        files.push_back(rf);
    }

    return files;
}
