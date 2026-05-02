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
    m_isNcrypt = false;
    m_cachedModulus.clear();
    m_cachedExp.clear();

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

    // 导出公钥参数到缓存，供 buildRsaPublicKey() 使用
    ULONG blobSize = 0;
    status = BCryptExportKey(m_key, nullptr, BCRYPT_RSAPUBLIC_BLOB,
                              nullptr, 0, &blobSize, 0);
    if (status == 0 && blobSize > 0) {
        std::vector<uint8_t> blob(blobSize);
        status = BCryptExportKey(m_key, nullptr, BCRYPT_RSAPUBLIC_BLOB,
                                  blob.data(), blobSize, &blobSize, 0);
        if (status == 0) {
            BCRYPT_RSAKEY_BLOB* header = (BCRYPT_RSAKEY_BLOB*)blob.data();
            uint8_t* keyData = blob.data() + sizeof(BCRYPT_RSAKEY_BLOB);
            m_cachedExp.assign(keyData, keyData + header->cbPublicExp);
            m_cachedModulus.assign(keyData + header->cbPublicExp,
                                    keyData + header->cbPublicExp + header->cbModulus);
            // 补齐 exponent 到 4 字节
            while (m_cachedExp.size() < 4) m_cachedExp.insert(m_cachedExp.begin(), 0);
            // 去掉 modulus 前导零
            while (!m_cachedModulus.empty() && m_cachedModulus[0] == 0)
                m_cachedModulus.erase(m_cachedModulus.begin());
            printf("RSA: Cached n=%zu bytes, e=%zu bytes from BCrypt\n",
                   m_cachedModulus.size(), m_cachedExp.size());
        }
    }

    printf("RSA: 2048-bit key pair generated (BCrypt)\n");
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
    b64.erase(std::remove_if(b64.begin(), b64.end(),
              [](char c) { return c == '\n' || c == '\r' || c == ' ' || c == '\t'; }),
              b64.end());

    // Base64 解码
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

    // 解析 PKCS#8 DER，提取全部 RSA 参数
    std::vector<uint8_t> n, e, d, p, q, dp, dq, qinv;
    {
        const uint8_t* pkcs8_data = der.data();
        size_t offset = 0;
        auto read_tag = [&]() -> uint8_t {
            return pkcs8_data[offset++];
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
            read_tag(); // should be 0x02 (INTEGER)
            size_t len = read_len();
            out.assign(pkcs8_data + offset, pkcs8_data + offset + len);
            offset += len;
        };
        auto strip_leading_zero = [](std::vector<uint8_t>& v) {
            while (!v.empty() && v[0] == 0) v.erase(v.begin());
            if (v.empty()) v.push_back(0);
        };

        // 外层 PKCS#8: SEQUENCE { INTEGER(version), SEQUENCE{alg}, OCTET STRING{inner} }
        read_tag(); read_len(); // outer SEQUENCE
        { std::vector<uint8_t> tmp; read_int(tmp); } // version
        read_tag(); size_t algLen = read_len(); offset += algLen; // skip algorithm
        read_tag(); read_len(); // OCTET STRING

        // 内层 PKCS#1 RSAPrivateKey: SEQUENCE { version, n, e, d, p, q, dp, dq, qinv }
        read_tag(); read_len(); // SEQUENCE
        { std::vector<uint8_t> tmp; read_int(tmp); } // version
        read_int(n);  read_int(e);  read_int(d);
        read_int(p);  read_int(q);  read_int(dp);
        read_int(dq); read_int(qinv);

        strip_leading_zero(n); strip_leading_zero(e);
        strip_leading_zero(d); strip_leading_zero(p);
        strip_leading_zero(q); strip_leading_zero(dp);
        strip_leading_zero(dq); strip_leading_zero(qinv);

        // 缓存 n 和 e（供 buildRsaPublicKey 使用）
        m_cachedModulus = n;
        m_cachedExp = e;
        while (m_cachedExp.size() < 4) m_cachedExp.insert(m_cachedExp.begin(), 0);
    }

    // 构建 BCRYPT_RSAFULLPRIVATE_BLOB
    size_t blobSize = sizeof(BCRYPT_RSAKEY_BLOB) +
                      e.size() + n.size() + p.size() + q.size() +
                      dp.size() + dq.size() + qinv.size() + d.size();
    std::vector<uint8_t> blob(blobSize);
    BCRYPT_RSAKEY_BLOB* header = (BCRYPT_RSAKEY_BLOB*)blob.data();
    header->Magic       = BCRYPT_RSAPRIVATE_MAGIC;  // "RSA2"
    header->BitLength   = (ULONG)n.size() * 8;
    header->cbPublicExp = (ULONG)e.size();
    header->cbModulus   = (ULONG)n.size();
    header->cbPrime1    = (ULONG)p.size();
    header->cbPrime2    = (ULONG)q.size();

    uint8_t* dst = blob.data() + sizeof(BCRYPT_RSAKEY_BLOB);
    auto copy_be = [&](const std::vector<uint8_t>& src) {
        memcpy(dst, src.data(), src.size());
        dst += src.size();
    };
    copy_be(e); copy_be(n); copy_be(p); copy_be(q);
    copy_be(dp); copy_be(dq); copy_be(qinv); copy_be(d);

    // 清理旧密钥
    if (m_key) { BCryptDestroyKey(m_key); m_key = nullptr; }
    if (m_alg) { BCryptCloseAlgorithmProvider(m_alg, 0); m_alg = nullptr; }
    m_isNcrypt = false;

    NTSTATUS status = BCryptOpenAlgorithmProvider(&m_alg, BCRYPT_RSA_ALGORITHM, nullptr, 0);
    if (status != 0) {
        printf("RSA: BCryptOpenAlgorithmProvider failed: 0x%08X\n", (unsigned)status);
        return false;
    }

    status = BCryptImportKeyPair(m_alg, nullptr, BCRYPT_RSAPRIVATE_BLOB,
                                   &m_key, blob.data(), (ULONG)blob.size(), 0);
    if (status != 0) {
        printf("RSA: BCryptImportKeyPair failed: 0x%08X\n", (unsigned)status);
        BCryptCloseAlgorithmProvider(m_alg, 0);
        m_alg = nullptr;
        return false;
    }

    printf("RSA: Adb key imported via BCrypt (n=%zu e=%zu d=%zu p=%zu q=%zu)\n",
           n.size(), e.size(), d.size(), p.size(), q.size());
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

    auto hash = sha1(token, token_len);

    if (m_isNcrypt) {
        // NCrypt 导入的密钥：使用 NCryptSignHash
        BCRYPT_PKCS1_PADDING_INFO paddingInfo;
        paddingInfo.pszAlgId = NCRYPT_SHA1_ALGORITHM;

        ULONG sigSize = 0;
        SECURITY_STATUS status = NCryptSignHash((NCRYPT_KEY_HANDLE)m_key, &paddingInfo,
                                                 hash.data(), (ULONG)hash.size(),
                                                 nullptr, 0, &sigSize,
                                                 BCRYPT_PAD_PKCS1);
        if (status != 0) {
            printf("RSA: NCryptSignHash size query failed: 0x%08X\n", (unsigned)status);
            return {};
        }

        std::vector<uint8_t> sig(sigSize);
        status = NCryptSignHash((NCRYPT_KEY_HANDLE)m_key, &paddingInfo,
                                 hash.data(), (ULONG)hash.size(),
                                 sig.data(), sigSize, &sigSize,
                                 BCRYPT_PAD_PKCS1);
        if (status != 0) {
            printf("RSA: NCryptSignHash failed: 0x%08X\n", (unsigned)status);
            return {};
        }
        printf("RSA: NCrypt signed token -> signature (%lu bytes)\n", sigSize);
        return sig;
    }

    // BCrypt 密钥：使用标准 BCryptSignHash (SHA-1)
    BCRYPT_PKCS1_PADDING_INFO paddingInfo;
    paddingInfo.pszAlgId = BCRYPT_SHA1_ALGORITHM;

    ULONG sigSize = 0;
    NTSTATUS status = BCryptSignHash(m_key, &paddingInfo,
                                      hash.data(), (ULONG)hash.size(),
                                      nullptr, 0, &sigSize,
                                      BCRYPT_PAD_PKCS1);
    if (status != 0) {
        printf("RSA: BCryptSignHash size query failed: 0x%08X\n", (unsigned)status);
        return {};
    }

    std::vector<uint8_t> sig(sigSize);
    status = BCryptSignHash(m_key, &paddingInfo,
                             hash.data(), (ULONG)hash.size(),
                             sig.data(), sigSize, &sigSize,
                             BCRYPT_PAD_PKCS1);
    if (status != 0) {
        printf("RSA: BCryptSignHash failed: 0x%08X\n", (unsigned)status);
        return {};
    }
    printf("RSA: BCryptSignHash -> signature (%lu bytes)\n", sigSize);
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

// ---- SHA-256 ----

namespace {
    const uint32_t K256[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
    };

    inline uint32_t rotr32_2(uint32_t x, int n) {
        return (x >> n) | (x << (32 - n));
    }

    void sha256_transform(uint32_t state[8], const uint8_t block[64]) {
        uint32_t w[64];
        for (int i = 0; i < 16; i++) {
            w[i] = ((uint32_t)block[i*4] << 24) | ((uint32_t)block[i*4+1] << 16) |
                   ((uint32_t)block[i*4+2] << 8) | (uint32_t)block[i*4+3];
        }
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = rotr32_2(w[i-15], 7) ^ rotr32_2(w[i-15], 18) ^ (w[i-15] >> 3);
            uint32_t s1 = rotr32_2(w[i-2], 17) ^ rotr32_2(w[i-2], 19) ^ (w[i-2] >> 10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }

        uint32_t a=state[0], b=state[1], c=state[2], d=state[3];
        uint32_t e=state[4], f=state[5], g=state[6], h=state[7];

        for (int i = 0; i < 64; i++) {
            uint32_t S1 = rotr32_2(e, 6) ^ rotr32_2(e, 11) ^ rotr32_2(e, 25);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t temp1 = h + S1 + ch + K256[i] + w[i];
            uint32_t S0 = rotr32_2(a, 2) ^ rotr32_2(a, 13) ^ rotr32_2(a, 22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t temp2 = S0 + maj;

            h = g; g = f; f = e; e = d + temp1;
            d = c; c = b; b = a; a = temp1 + temp2;
        }

        state[0] += a; state[1] += b; state[2] += c; state[3] += d;
        state[4] += e; state[5] += f; state[6] += g; state[7] += h;
    }
}

std::vector<uint8_t> AdbRsa::sha256(const uint8_t* data, size_t len) {
    uint32_t state[8] = {
        0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
        0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19
    };
    uint64_t bitlen = (uint64_t)len * 8;

    size_t i = 0;
    for (; i + 64 <= len; i += 64)
        sha256_transform(state, data + i);

    uint8_t final[64] = {};
    size_t rem = len - i;
    memcpy(final, data + i, rem);
    final[rem] = 0x80;

    if (rem >= 56) {
        sha256_transform(state, final);
        memset(final, 0, 64);
    }

    for (int j = 0; j < 8; j++)
        final[56 + j] = (uint8_t)(bitlen >> (56 - j * 8));
    sha256_transform(state, final);

    std::vector<uint8_t> hash(32);
    for (int j = 0; j < 8; j++) {
        hash[j*4]   = (state[j] >> 24) & 0xFF;
        hash[j*4+1] = (state[j] >> 16) & 0xFF;
        hash[j*4+2] = (state[j] >> 8) & 0xFF;
        hash[j*4+3] = state[j] & 0xFF;
    }
    return hash;
}

// ---- RSAPublicKey 结构构建 ----

// 模逆 n0inv = -modinv(n0, 2^32)
static uint32_t modinv32(uint32_t a) {
    // a 必须是奇数（RSA 模数的低 32 位总是奇数）
    uint32_t x = 1;
    // Newton 迭代: x = x * (2 - a * x) mod 2^32
    for (int i = 0; i < 5; i++)
        x = x * (2 - a * x);
    return (uint32_t)(-(int32_t)x);
}

std::vector<uint8_t> AdbRsa::buildRsaPublicKey() {
    if (m_cachedModulus.size() < 256 || m_cachedExp.size() < 4) {
        printf("RSA: No cached key params for RSAPublicKey\n");
        return {};
    }

    // 1. 从 BE 字节转为 LE uint32 limbs
    std::vector<uint32_t> n_limbs(64, 0);
    for (int i = 0; i < 64; i++) {
        int be_idx = 255 - (i * 4 + 3);  // limb[i] 的 MSB 在 BE 的位置
        n_limbs[i] = ((uint32_t)m_cachedModulus[be_idx + 3]) |
                     ((uint32_t)m_cachedModulus[be_idx + 2] << 8) |
                     ((uint32_t)m_cachedModulus[be_idx + 1] << 16) |
                     ((uint32_t)m_cachedModulus[be_idx] << 24);
    }

    uint32_t n0 = n_limbs[0];
    uint32_t n0inv = modinv32(n0);

    // 2. 计算 rr = 2^4096 mod n
    //    使用重复加倍 + 条件减法
    std::vector<uint32_t> r(66, 0);  // 最多需要 66 个 limb
    r[64] = 1;  // 2^2048 (bit 2048 = limb 64 bit 0)

    // r = 2^2048 - n
    {
        uint64_t borrow = 0;
        for (int i = 0; i < 64; i++) {
            uint64_t sub = (uint64_t)n_limbs[i] + borrow;
            if (r[i] < sub) {
                r[i] = (uint32_t)((r[i] + 0x100000000ULL) - sub);
                borrow = 1;
            } else {
                r[i] = (uint32_t)(r[i] - sub);
                borrow = 0;
            }
        }
        if (borrow && r[64] > 0) r[64]--;
    }

    // 加倍 2048 次，每次 >= n 则减去 n
    for (int iter = 0; iter < 2048; iter++) {
        // r *= 2
        uint32_t carry = 0;
        for (size_t i = 0; i < r.size(); i++) {
            uint64_t v = ((uint64_t)r[i] << 1) | carry;
            r[i] = (uint32_t)v;
            carry = (uint32_t)(v >> 32);
        }

        // 如果 r >= n，r -= n
        // 简化：检查是否有进位，或最高 limb 比较
        bool ge = (carry > 0);
        if (!ge) {
            // 比较 r[63..0] 和 n[63..0]
            for (int i = 63; i >= 0; i--) {
                if (r[i] > n_limbs[i]) { ge = true; break; }
                if (r[i] < n_limbs[i]) break;
                if (i == 0 && r[i] == n_limbs[i]) ge = true;
            }
        }
        if (ge) {
            uint64_t borrow = 0;
            for (int i = 0; i < 64; i++) {
                uint64_t sub = (uint64_t)n_limbs[i] + borrow;
                if (r[i] < sub) {
                    r[i] = (uint32_t)((r[i] + 0x100000000ULL) - sub);
                    borrow = 1;
                } else {
                    r[i] = (uint32_t)(r[i] - sub);
                    borrow = 0;
                }
            }
            if (borrow && r[64] > 0) r[64]--;
        }
    }

    // 3. 组装 RSAPublicKey (524 bytes)
    std::vector<uint8_t> result(4 + 4 + 256 + 256 + 4, 0);
    size_t off = 0;

    // key_size (uint32 LE)
    uint32_t key_size = 64;
    memcpy(result.data() + off, &key_size, 4); off += 4;

    // n0inv (uint32 LE)
    memcpy(result.data() + off, &n0inv, 4); off += 4;

    // modulus (256 bytes, LE from limbs)
    for (int i = 0; i < 64; i++) {
        memcpy(result.data() + off, &n_limbs[i], 4);
        off += 4;
    }

    // rr (256 bytes, LE from limbs)
    for (int i = 0; i < 64; i++) {
        memcpy(result.data() + off, &r[i], 4);
        off += 4;
    }

    // exponent (uint32 LE)
    // m_cachedExp is BE, convert to LE
    uint32_t exp = 0;
    for (size_t i = 0; i < m_cachedExp.size(); i++)
        exp |= ((uint32_t)m_cachedExp[m_cachedExp.size() - 1 - i]) << (i * 8);
    memcpy(result.data() + off, &exp, 4); off += 4;

    printf("RSA: RSAPublicKey built (%zu bytes, n0inv=0x%08X, exp=0x%X)\n",
           result.size(), n0inv, exp);
    return result;
}

std::string AdbRsa::base64Encode(const uint8_t* data, size_t len) {
    DWORD b64Len = 0;
    if (!CryptBinaryToStringA(data, (DWORD)len,
                               CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                               nullptr, &b64Len))
        return {};

    std::string result(b64Len, 0);
    if (!CryptBinaryToStringA(data, (DWORD)len,
                               CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                               &result[0], &b64Len))
        return {};

    // 去除尾部的 null
    while (!result.empty() && result.back() == 0)
        result.pop_back();
    return result;
}

bool AdbRsa::readAdbPubKey(std::string& out) {
    char appdata[MAX_PATH] = {};
    if (SHGetFolderPathA(nullptr, CSIDL_PROFILE, nullptr, 0, appdata) != S_OK)
        return false;

    std::string path = std::string(appdata) + "\\.android\\adbkey.pub";
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0 || size > 2048) {
        fclose(f);
        return false;
    }

    out.resize(size);
    size_t read = fread(&out[0], 1, size, f);
    fclose(f);

    if (read != (size_t)size) return false;

    // 去除末尾的换行符
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
        out.pop_back();

    printf("RSA: Read adbkey.pub (%zu chars)\n", out.size());
    return true;
}

std::string AdbRsa::getPubKeyPayload() {
    // 优先复用 adb 的 .pub 文件
    std::string pubKeyStr;
    if (readAdbPubKey(pubKeyStr)) {
        pubKeyStr += '\0';
        printf("RSA: Using adbkey.pub for AUTH_RSAPUBLICKEY (%zu bytes)\n", pubKeyStr.size());
        return pubKeyStr;
    }

    // 没有 .pub 文件，自己构建 RSAPublicKey
    if (!isReady()) {
        printf("RSA: Key not ready, cannot build RSAPublicKey\n");
        return {};
    }

    // 如果缓存为空（从旧文件加载的 BCrypt key），尝试导出公钥参数
    if (m_cachedModulus.empty() && !m_isNcrypt) {
        ULONG blobSize = 0;
        NTSTATUS status = BCryptExportKey(m_key, nullptr, BCRYPT_RSAPUBLIC_BLOB,
                                           nullptr, 0, &blobSize, 0);
        if (status == 0 && blobSize > 0) {
            std::vector<uint8_t> blob(blobSize);
            status = BCryptExportKey(m_key, nullptr, BCRYPT_RSAPUBLIC_BLOB,
                                      blob.data(), blobSize, &blobSize, 0);
            if (status == 0) {
                BCRYPT_RSAKEY_BLOB* header = (BCRYPT_RSAKEY_BLOB*)blob.data();
                uint8_t* keyData = blob.data() + sizeof(BCRYPT_RSAKEY_BLOB);
                m_cachedExp.assign(keyData, keyData + header->cbPublicExp);
                m_cachedModulus.assign(keyData + header->cbPublicExp,
                                        keyData + header->cbPublicExp + header->cbModulus);
                while (m_cachedExp.size() < 4) m_cachedExp.insert(m_cachedExp.begin(), 0);
                while (!m_cachedModulus.empty() && m_cachedModulus[0] == 0)
                    m_cachedModulus.erase(m_cachedModulus.begin());
                printf("RSA: Exported n=%zu bytes, e=%zu bytes from loaded key\n",
                       m_cachedModulus.size(), m_cachedExp.size());
            }
        }
    }

    auto rsapk = buildRsaPublicKey();
    if (rsapk.empty()) return {};

    std::string b64 = base64Encode(rsapk.data(), rsapk.size());
    if (b64.empty()) return {};

    // 用户名
    char user[256] = {};
    DWORD userLen = sizeof(user);
    GetUserNameA(user, &userLen);
    char computer[256] = {};
    DWORD compLen = sizeof(computer);
    GetComputerNameA(computer, &compLen);

    std::string result = b64 + " " + std::string(user) + "@" + std::string(computer);
    result += '\0';
    printf("RSA: Built AUTH_RSAPUBLICKEY payload (%zu bytes)\n", result.size());
    return result;
}
