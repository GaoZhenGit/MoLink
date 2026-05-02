#include "adb_rsa.h"
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <shlobj.h>
#include <ncrypt.h>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "ncrypt.lib")
#pragma comment(lib, "crypt32.lib")

AdbRsa::AdbRsa() : m_key(nullptr), m_alg(nullptr) {}

AdbRsa::~AdbRsa() {
    if (m_key) {
        if (m_isNcrypt)
            NCryptFreeObject((NCRYPT_KEY_HANDLE)m_key);
        else
            BCryptDestroyKey(m_key);
        m_key = nullptr;
    }
    if (m_alg) {
        BCryptCloseAlgorithmProvider(m_alg, 0);
        m_alg = nullptr;
    }
}

bool AdbRsa::generateKey() {
    if (m_alg) {
        BCryptCloseAlgorithmProvider(m_alg, 0);
        m_alg = nullptr;
    }
    if (m_key) {
        BCryptDestroyKey(m_key);
        m_key = nullptr;
    }

    NTSTATUS status = BCryptOpenAlgorithmProvider(&m_alg, BCRYPT_RSA_ALGORITHM, nullptr, 0);
    if (status != 0) {
        printf("RSA: BCryptOpenAlgorithmProvider failed: 0x%08X\n", (unsigned)status);
        return false;
    }

    status = BCryptGenerateKeyPair(m_alg, &m_key, 2048, 0);
    if (status != 0) {
        printf("RSA: BCryptGenerateKeyPair failed: 0x%08X\n", (unsigned)status);
        BCryptCloseAlgorithmProvider(m_alg, 0);
        m_alg = nullptr;
        return false;
    }

    status = BCryptFinalizeKeyPair(m_key, 0);
    if (status != 0) {
        printf("RSA: BCryptFinalizeKeyPair failed: 0x%08X\n", (unsigned)status);
        BCryptDestroyKey(m_key);
        BCryptCloseAlgorithmProvider(m_alg, 0);
        m_key = nullptr;
        m_alg = nullptr;
        return false;
    }

    printf("RSA: 2048-bit key pair generated\n");
    return true;
}

std::vector<uint8_t> AdbRsa::exportKeyBlob() {
    if (!m_key) return {};

    ULONG size = 0;
    NTSTATUS status = BCryptExportKey(m_key, nullptr, BCRYPT_RSAFULLPRIVATE_BLOB,
                                       nullptr, 0, &size, 0);
    if (status != 0 || size == 0) {
        printf("RSA: ExportKey size query failed: 0x%08X\n", (unsigned)status);
        return {};
    }

    std::vector<uint8_t> blob(size);
    status = BCryptExportKey(m_key, nullptr, BCRYPT_RSAFULLPRIVATE_BLOB,
                              blob.data(), size, &size, 0);
    if (status != 0) {
        printf("RSA: ExportKey failed: 0x%08X\n", (unsigned)status);
        return {};
    }
    return blob;
}

bool AdbRsa::importKeyBlob(const uint8_t* blob, size_t len) {
    if (!m_alg) {
        NTSTATUS status = BCryptOpenAlgorithmProvider(&m_alg, BCRYPT_RSA_ALGORITHM, nullptr, 0);
        if (status != 0) {
            printf("RSA: BCryptOpenAlgorithmProvider failed: 0x%08X\n", (unsigned)status);
            return false;
        }
    }

    if (m_key) {
        BCryptDestroyKey(m_key);
        m_key = nullptr;
    }

    NTSTATUS status = BCryptImportKeyPair(m_alg, nullptr, BCRYPT_RSAFULLPRIVATE_BLOB,
                                           &m_key, (PUCHAR)blob, (ULONG)len, 0);
    if (status != 0) {
        printf("RSA: ImportKeyPair failed: 0x%08X\n", (unsigned)status);
        return false;
    }
    printf("RSA: Key pair imported (%zu bytes)\n", len);
    return true;
}

bool AdbRsa::saveKey(const std::string& path) {
    auto blob = exportKeyBlob();
    if (blob.empty()) return false;

    FILE* f = fopen(path.c_str(), "wb");
    if (!f) {
        printf("RSA: Cannot open %s for writing\n", path.c_str());
        return false;
    }

    size_t written = fwrite(blob.data(), 1, blob.size(), f);
    fclose(f);

    if (written != blob.size()) {
        printf("RSA: Write failed (%zu/%zu bytes)\n", written, blob.size());
        return false;
    }

    printf("RSA: Key saved to %s (%zu bytes)\n", path.c_str(), blob.size());
    return true;
}

bool AdbRsa::loadPkcs8(const std::string& path) {
    // 读取 PEM 文件
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::string pem(size, 0);
    fread(&pem[0], 1, size, f);
    fclose(f);

    // 去掉 PEM 头尾
    auto begin = pem.find("-----BEGIN");
    auto end = pem.find("-----END");
    if (begin == std::string::npos || end == std::string::npos) return false;
    begin = pem.find('\n', begin) + 1;
    std::string b64 = pem.substr(begin, end - begin);
    // 去除换行符和空白
    b64.erase(std::remove_if(b64.begin(), b64.end(),
              [](char c) { return c == '\n' || c == '\r' || c == ' ' || c == '\t'; }),
              b64.end());

    // Base64 解码
    // Windows 自带 CryptStringToBinaryA
    DWORD derLen = 0;
    if (!CryptStringToBinaryA(b64.c_str(), (DWORD)b64.size(),
                               CRYPT_STRING_BASE64, nullptr, &derLen, nullptr, nullptr)) {
        printf("RSA: Base64 decode size failed: %lu\n", GetLastError());
        return false;
    }
    std::vector<uint8_t> der(derLen);
    if (!CryptStringToBinaryA(b64.c_str(), (DWORD)b64.size(),
                               CRYPT_STRING_BASE64, der.data(), &derLen, nullptr, nullptr)) {
        printf("RSA: Base64 decode failed: %lu\n", GetLastError());
        return false;
    }

    // NCrypt PKCS#8 导入
    NCRYPT_PROV_HANDLE prov = 0;
    SECURITY_STATUS ss = NCryptOpenStorageProvider(&prov, MS_KEY_STORAGE_PROVIDER, 0);
    if (ss != 0) {
        printf("RSA: NCryptOpenStorageProvider failed: 0x%08X\n", (unsigned)ss);
        return false;
    }

    NCRYPT_KEY_HANDLE ncKey = 0;
    ss = NCryptImportKey(prov, 0, NCRYPT_PKCS8_PRIVATE_KEY_BLOB, nullptr,
                          &ncKey, der.data(), derLen, NCRYPT_DO_NOT_FINALIZE_FLAG);
    if (ss != 0) {
        printf("RSA: NCryptImportKey failed: 0x%08X\n", (unsigned)ss);
        NCryptFreeObject(prov);
        return false;
    }

    // 设置 SHA1 签名算法（使用已知常量值）
    // NCRYPT_SIGNATURE_HASH_ALGORITHM_PROPERTY = L"Signature Hash Algorithm"
    #ifndef NCRYPT_SIGNATURE_HASH_ALGORITHM_PROPERTY
    #define NCRYPT_SIGNATURE_HASH_ALGORITHM_PROPERTY L"Signature Hash Algorithm"
    #endif
    ss = NCryptSetProperty(ncKey, NCRYPT_SIGNATURE_HASH_ALGORITHM_PROPERTY,
                            (PBYTE)NCRYPT_SHA1_ALGORITHM,
                            (DWORD)(wcslen(NCRYPT_SHA1_ALGORITHM) + 1) * sizeof(WCHAR), 0);

    // Finalize key
    ss = NCryptFinalizeKey(ncKey, 0);
    if (ss != 0) {
        printf("RSA: NCryptFinalizeKey failed: 0x%08X\n", (unsigned)ss);
        NCryptFreeObject(ncKey);
        NCryptFreeObject(prov);
        return false;
    }

    // 缓存公钥参数（NCrypt 导入的密钥不支持 BCryptExportKey）
    // 从已解析的 DER 中提取 n 和 e
    {
        const uint8_t* pkcs8_data = der.data();
        size_t offset = 0;
        auto read_tag = [&]() -> uint8_t {
            uint8_t t = pkcs8_data[offset]; offset++;
            return t;
        };
        auto read_len = [&]() -> size_t {
            if (pkcs8_data[offset] & 0x80) {
                int nl = pkcs8_data[offset] & 0x7F; offset++;
                size_t l = 0;
                for (int i = 0; i < nl; i++) l = (l << 8) | pkcs8_data[offset++];
                return l;
            }
            return pkcs8_data[offset++];
        };
        auto read_int = [&](std::vector<uint8_t>& out) {
            uint8_t tag = read_tag();
            (void)tag;
            size_t len = read_len();
            out.assign(pkcs8_data + offset, pkcs8_data + offset + len);
            offset += len;
        };

        // Skip top SEQUENCE, version, algorithm SEQUENCE (skip its content), OCTET STRING
        read_tag(); read_len(); // outer SEQUENCE
        { std::vector<uint8_t> tmp; read_int(tmp); } // version
        read_tag(); // algorithm SEQUENCE tag
        size_t algLen = read_len(); // algorithm SEQUENCE length
        offset += algLen; // SKIP algorithm content (OID + NULL)
        read_tag(); // OCTET STRING tag (should be 0x04)
        read_len(); // OCTET STRING length
        // Now inside PKCS#1 RSAPrivateKey
        read_tag(); read_len(); // SEQUENCE
        { std::vector<uint8_t> tmp; read_int(tmp); } // version
        read_int(m_cachedModulus);  // n
        read_int(m_cachedExp);      // e
        // Strip leading zero byte from modulus (DER sign byte)
        while (!m_cachedModulus.empty() && m_cachedModulus[0] == 0)
            m_cachedModulus.erase(m_cachedModulus.begin());
        while (!m_cachedExp.empty() && m_cachedExp[0] == 0)
            m_cachedExp.erase(m_cachedExp.begin());
        printf("RSA: Cached n=%zu bytes, e=%zu bytes\n", m_cachedModulus.size(), m_cachedExp.size());
        // Pad exponent to 4 bytes BE
        while (m_cachedExp.size() < 4) m_cachedExp.insert(m_cachedExp.begin(), 0);
    }

    // 将 NCRYPT_KEY_HANDLE 转为 BCRYPT_KEY_HANDLE
    if (m_key) { BCryptDestroyKey(m_key); m_key = nullptr; }
    if (m_alg) { BCryptCloseAlgorithmProvider(m_alg, 0); m_alg = nullptr; }

    m_key = (BCRYPT_KEY_HANDLE)ncKey;
    m_isNcrypt = true;
    printf("RSA: Key imported from PKCS#8 (%lu bytes DER)\n", derLen);
    return true;
}

bool AdbRsa::loadKey(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        printf("RSA: Key file not found: %s\n", path.c_str());
        return false;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0 || size > 8192) {
        printf("RSA: Invalid key file size: %ld\n", size);
        fclose(f);
        return false;
    }

    std::vector<uint8_t> blob(size);
    size_t read = fread(blob.data(), 1, size, f);
    fclose(f);

    if (read != (size_t)size) {
        printf("RSA: Read failed (%zu/%ld bytes)\n", read, size);
        return false;
    }

    return importKeyBlob(blob.data(), blob.size());
}

std::vector<uint8_t> AdbRsa::signToken(const uint8_t* token, size_t token_len) {
    if (!m_key) {
        printf("RSA: No key for signing\n");
        return {};
    }

    // SHA1(token)
    auto hash = sha1(token, token_len);

    // Try NCryptSignHash first (for PKCS#8 imported keys), fall back to BCryptSignHash
    BCRYPT_PKCS1_PADDING_INFO paddingInfo;
    paddingInfo.pszAlgId = NCRYPT_SHA1_ALGORITHM;

    ULONG sigSize = 0;
    // 注意: NCryptSignHash 在 Win10+ 上接受 BCRYPT_PAD_PKCS1 即使对 finalized keys
    SECURITY_STATUS status = NCryptSignHash((NCRYPT_KEY_HANDLE)m_key, &paddingInfo,
                                             hash.data(), (ULONG)hash.size(),
                                             nullptr, 0, &sigSize,
                                             BCRYPT_PAD_PKCS1);
    if (status != 0) {
        // Fall back to BCryptSignHash
        paddingInfo.pszAlgId = BCRYPT_SHA1_ALGORITHM;
        status = BCryptSignHash(m_key, &paddingInfo,
                                 hash.data(), (ULONG)hash.size(),
                                 nullptr, 0, &sigSize,
                                 BCRYPT_PAD_PKCS1);
        if (status != 0) {
            printf("RSA: SignHash size query failed: 0x%08X\n", (unsigned)status);
            return {};
        }
    }

    std::vector<uint8_t> sig(sigSize);
    status = NCryptSignHash((NCRYPT_KEY_HANDLE)m_key, &paddingInfo,
                             hash.data(), (ULONG)hash.size(),
                             sig.data(), sigSize, &sigSize,
                             BCRYPT_PAD_PKCS1);
    if (status != 0) {
        // Fall back to BCryptSignHash
        paddingInfo.pszAlgId = BCRYPT_SHA1_ALGORITHM;
        status = BCryptSignHash(m_key, &paddingInfo,
                                 hash.data(), (ULONG)hash.size(),
                                 sig.data(), sigSize, &sigSize,
                                 BCRYPT_PAD_PKCS1);
        if (status != 0) {
            printf("RSA: SignHash failed: 0x%08X\n", (unsigned)status);
            return {};
        }
    }

    printf("RSA: Signed token (%zu bytes) -> signature (%lu bytes)\n", token_len, sigSize);
    return sig;
}

std::vector<uint8_t> AdbRsa::getPublicKey(const std::string& user) {
    if (!m_key) return {};

    std::vector<uint8_t> pubExp;
    std::vector<uint8_t> modulus;

    if (m_isNcrypt && !m_cachedModulus.empty()) {
        // 使用缓存的公钥参数（NCrypt 不支持 BCryptExportKey）
        pubExp = m_cachedExp;
        modulus = m_cachedModulus;
    } else {
        // Export public key blob from CNG
        ULONG blobSize = 0;
        NTSTATUS status = BCryptExportKey(m_key, nullptr, BCRYPT_RSAPUBLIC_BLOB,
                                           nullptr, 0, &blobSize, 0);
        if (status != 0 || blobSize == 0) {
            printf("RSA: Export public key size failed: 0x%08X\n", (unsigned)status);
            return {};
        }

        std::vector<uint8_t> blob(blobSize);
        status = BCryptExportKey(m_key, nullptr, BCRYPT_RSAPUBLIC_BLOB,
                                  blob.data(), blobSize, &blobSize, 0);
        if (status != 0) {
            printf("RSA: Export public key failed: 0x%08X\n", (unsigned)status);
            return {};
        }

        BCRYPT_RSAKEY_BLOB* header = (BCRYPT_RSAKEY_BLOB*)blob.data();
        uint8_t* keyData = blob.data() + sizeof(BCRYPT_RSAKEY_BLOB);
        pubExp.assign(keyData, keyData + header->cbPublicExp);
        modulus.assign(keyData + header->cbPublicExp,
                       keyData + header->cbPublicExp + header->cbModulus);
    }

    // pubExp and modulus are now in BE byte order (from CNG blob or PKCS#8 DER)
    // Android format: name\0 + 4 bytes LE exponent + N bytes LE modulus
    size_t nameLen = user.size() + 1;
    size_t keyLen = nameLen + 4 + modulus.size();

    std::vector<uint8_t> result(keyLen);
    memcpy(result.data(), user.c_str(), nameLen);

    // Exponent: reverse BE → LE, pad to 4 bytes
    for (size_t i = 0; i < 4; i++) {
        if (i < pubExp.size())
            result[nameLen + i] = pubExp[pubExp.size() - 1 - i];
        else
            result[nameLen + i] = 0;
    }

    // Modulus: reverse BE → LE
    for (size_t i = 0; i < modulus.size(); i++)
        result[nameLen + 4 + i] = modulus[modulus.size() - 1 - i];

    printf("RSA: Public key exported (%zu bytes, exp=%zu bytes, mod=%zu bytes, user=%s)\n",
           result.size(), pubExp.size(), modulus.size(), user.c_str());

    return result;
}

// ---- SHA1 ----

namespace {
    inline uint32_t rotl32(uint32_t x, int n) {
        return (x << n) | (x >> (32 - n));
    }

    void sha1_transform(uint32_t state[5], const uint8_t block[64]) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) {
            w[i] = ((uint32_t)block[i * 4] << 24) |
                   ((uint32_t)block[i * 4 + 1] << 16) |
                   ((uint32_t)block[i * 4 + 2] << 8) |
                   (uint32_t)block[i * 4 + 3];
        }
        for (int i = 16; i < 80; i++) {
            uint32_t x = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
            w[i] = rotl32(x, 1);
        }

        uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) {
                f = (b & c) | (~b & d);
                k = 0x5A827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }
            uint32_t t = rotl32(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rotl32(b, 30);
            b = a;
            a = t;
        }
        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
        state[4] += e;
    }
}

std::vector<uint8_t> AdbRsa::sha1(const uint8_t* data, size_t len) {
    uint32_t state[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    uint64_t bitlen = (uint64_t)len * 8;

    size_t i = 0;
    for (; i + 64 <= len; i += 64)
        sha1_transform(state, data + i);

    uint8_t final[64] = {};
    size_t rem = len - i;
    memcpy(final, data + i, rem);
    final[rem] = 0x80;

    if (rem >= 56) {
        sha1_transform(state, final);
        memset(final, 0, 64);
    }

    for (int j = 0; j < 8; j++)
        final[56 + j] = (uint8_t)(bitlen >> (56 - j * 8));
    sha1_transform(state, final);

    std::vector<uint8_t> hash(20);
    for (int j = 0; j < 5; j++) {
        hash[j * 4]     = (state[j] >> 24) & 0xFF;
        hash[j * 4 + 1] = (state[j] >> 16) & 0xFF;
        hash[j * 4 + 2] = (state[j] >> 8) & 0xFF;
        hash[j * 4 + 3] = state[j] & 0xFF;
    }
    return hash;
}
