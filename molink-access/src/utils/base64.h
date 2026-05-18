#ifndef BASE64_H
#define BASE64_H

#include <string>
#include <vector>
#include <cstdint>

inline std::string base64Encode(const std::string& input) {
    static const char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string output;
    output.reserve(((input.size() + 2) / 3) * 4);

    const uint8_t* data = (const uint8_t*)input.data();
    size_t i = 0;
    for (; i + 2 < input.size(); i += 3) {
        output.push_back(kAlphabet[(data[i] >> 2) & 0x3F]);
        output.push_back(kAlphabet[((data[i] & 0x03) << 4) | ((data[i + 1] >> 4) & 0x0F)]);
        output.push_back(kAlphabet[((data[i + 1] & 0x0F) << 2) | ((data[i + 2] >> 6) & 0x03)]);
        output.push_back(kAlphabet[data[i + 2] & 0x3F]);
    }

    size_t remaining = input.size() - i;
    if (remaining == 1) {
        output.push_back(kAlphabet[(data[i] >> 2) & 0x3F]);
        output.push_back(kAlphabet[(data[i] & 0x03) << 4]);
        output.push_back('=');
        output.push_back('=');
    } else if (remaining == 2) {
        output.push_back(kAlphabet[(data[i] >> 2) & 0x3F]);
        output.push_back(kAlphabet[((data[i] & 0x03) << 4) | ((data[i + 1] >> 4) & 0x0F)]);
        output.push_back(kAlphabet[(data[i + 1] & 0x0F) << 2]);
        output.push_back('=');
    }

    return output;
}

inline std::string base64Decode(const std::string& input) {
    static const uint8_t kDecode[128] = {
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,62,0,0,0,63,
        52,53,54,55,56,57,58,59,60,61,0,0,0,0,0,0,
        0,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,0,0,0,0,0,
        0,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,0,0,0,0,0
    };

    std::string output;
    output.reserve((input.size() / 4) * 3);

    int val = 0, valb = -8;
    for (unsigned char c : input) {
        if (c == '=') break;  // padding: stop, don't output garbage NUL
        if (c >= 128 || (kDecode[c] == 0 && c != 'A')) continue;
        val = (val << 6) + kDecode[c];
        valb += 6;
        if (valb >= 0) {
            output.push_back(char((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return output;
}

#endif
