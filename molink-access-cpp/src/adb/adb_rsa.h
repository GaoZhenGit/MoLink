#ifndef ADB_RSA_H
#define ADB_RSA_H

#include <cstdint>
#include <vector>
#include <string>
#include <windows.h>
#include <bcrypt.h>

class AdbRsa {
public:
    AdbRsa();
    ~AdbRsa();

    bool generateKey();
    bool loadKey(const std::string& path);
    bool loadPkcs8(const std::string& path);
    bool saveKey(const std::string& path);

    std::vector<uint8_t> signToken(const uint8_t* token, size_t token_len);
    std::vector<uint8_t> getPublicKey(const std::string& user = "molink@host");
    std::string getPubKeyPayload();

    bool isReady() const { return m_key != nullptr; }

private:
    BCRYPT_KEY_HANDLE m_key;
    BCRYPT_ALG_HANDLE m_alg;
    std::vector<uint8_t> m_cachedModulus;  // 256 bytes BE
    std::vector<uint8_t> m_cachedExp;      // 4 bytes BE

    static std::vector<uint8_t> sha1(const uint8_t* data, size_t len);
    static std::vector<uint8_t> sha256(const uint8_t* data, size_t len);
    static std::string base64Encode(const uint8_t* data, size_t len);
    std::vector<uint8_t> exportKeyBlob();
    bool importKeyBlob(const uint8_t* blob, size_t len);
    std::vector<uint8_t> buildRsaPublicKey();
    bool readAdbPubKey(std::string& out);
};

#endif
