#include "adb_rsa.h"
#include <cstdio>
#include <cstring>
#include <shlobj.h>

#pragma comment(lib, "bcrypt.lib")

AdbRsa::AdbRsa() : m_key(nullptr), m_alg(nullptr) {}

AdbRsa::~AdbRsa() {
    if (m_key) {
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

    // BCryptSignHash with PKCS#1 v1.5 padding (SHA1)
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

    printf("RSA: Signed token (%zu bytes) -> signature (%lu bytes)\n", token_len, sigSize);
    return sig;
}

std::vector<uint8_t> AdbRsa::getPublicKey(const std::string& user) {
    if (!m_key) return {};

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

    // Parse BCRYPT_RSAKEY_BLOB
    BCRYPT_RSAKEY_BLOB* header = (BCRYPT_RSAKEY_BLOB*)blob.data();
    uint8_t* keyData = blob.data() + sizeof(BCRYPT_RSAKEY_BLOB);
    uint8_t* pubExp = keyData;
    uint8_t* modulus = keyData + header->cbPublicExp;

    // Android format: name\0 + 4 bytes LE exponent + 256 bytes LE modulus
    // CNG blob stores exponent and modulus in BIG-ENDIAN, must reverse to LE
    size_t nameLen = user.size() + 1;
    size_t keyLen = nameLen + 4 + header->cbModulus;

    std::vector<uint8_t> result(keyLen);
    memcpy(result.data(), user.c_str(), nameLen);

    // Exponent: reverse bytes (BE -> LE), pad to 4 bytes
    ULONG expLen = header->cbPublicExp;
    for (ULONG i = 0; i < 4; i++) {
        if (i < expLen)
            result[nameLen + i] = pubExp[expLen - 1 - i];  // reverse
        else
            result[nameLen + i] = 0;
    }

    // Modulus: reverse bytes (BE -> LE)
    for (ULONG i = 0; i < header->cbModulus; i++)
        result[nameLen + 4 + i] = modulus[header->cbModulus - 1 - i];

    printf("RSA: Public key exported (%zu bytes, exp=%u bytes, mod=%u bytes, user=%s)\n",
           result.size(), expLen, header->cbModulus, user.c_str());

    // Debug: print CNG blob bytes and result bytes
    printf("RSA: CNG exp raw (%u bytes): ", expLen);
    for (ULONG i = 0; i < expLen && i < 8; i++) printf("%02X ", pubExp[i]);
    printf("\n");
    printf("RSA: CNG mod first 8 bytes: ");
    for (int i = 0; i < 8; i++) printf("%02X ", modulus[i]);
    printf("\n");
    printf("RSA: Android exp LE (4 bytes): ");
    for (int i = 0; i < 4; i++) printf("%02X ", result[nameLen + i]);
    printf("\n");
    printf("RSA: Android mod first 8 bytes: ");
    for (int i = 0; i < 8; i++) printf("%02X ", result[nameLen + 4 + i]);
    printf("\n");
    printf("RSA: Android mod last 8 bytes: ");
    for (int i = 0; i < 8; i++) printf("%02X ", result[nameLen + 4 + 248 + i]);
    printf("\n");
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
