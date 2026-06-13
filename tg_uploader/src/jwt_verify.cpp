#include "jwt_verify.h"
#include "user_auth.h"
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

using json = nlohmann::json;

namespace {

// base64url decode (RFC 4648 §5) — no padding, '-' and '_' instead of '+' '/'
bool b64url_decode(const std::string& in, std::string& out) {
    static const int8_t T[256] = {
        // 0..255
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,63,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    };
    out.clear();
    out.reserve(in.size() * 3 / 4 + 4);
    int val = 0, valb = -8;
    for (unsigned char c : in) {
        int d = T[c];
        if (d < 0) return false;
        val = (val << 6) | d;
        valb += 6;
        if (valb >= 0) {
            out.push_back(static_cast<char>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return true;
}

// Split string by delimiter
std::vector<std::string> split(const std::string& s, char d) {
    std::vector<std::string> out;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); i++) {
        if (i == s.size() || s[i] == d) {
            out.emplace_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    return out;
}

bool constant_time_eq(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned char acc = 0;
    for (size_t i = 0; i < a.size(); i++) {
        acc |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
    }
    return acc == 0;
}
}  // namespace

bool jwt_verify_hs256(const std::string& token) {
    const std::string& secret = web_jwt_secret();
    if (secret.empty()) return false;
    auto parts = split(token, '.');
    if (parts.size() != 3) return false;

    std::string signing_input = parts[0] + "." + parts[1];
    std::string expected_sig;
    if (!b64url_decode(parts[2], expected_sig)) return false;

    unsigned char mac[EVP_MAX_MD_SIZE];
    unsigned int mac_len = 0;
    HMAC(EVP_sha256(),
         secret.data(), static_cast<int>(secret.size()),
         reinterpret_cast<const unsigned char*>(signing_input.data()),
         signing_input.size(),
         mac, &mac_len);
    if (mac_len != 32) return false;

    std::string actual_sig(reinterpret_cast<char*>(mac), mac_len);
    if (!constant_time_eq(actual_sig, expected_sig)) return false;

    // Decode payload to check exp
    std::string payload_str;
    if (!b64url_decode(parts[1], payload_str)) return false;
    try {
        auto payload = json::parse(payload_str);
        if (payload.contains("exp")) {
            std::time_t exp = payload["exp"].get<std::time_t>();
            if (std::time(nullptr) > exp) return false;
        }
    } catch (...) {
        return false;
    }
    return true;
}

std::string jwt_extract_sub(const std::string& token) {
    if (!jwt_verify_hs256(token)) return "";
    auto parts = split(token, '.');
    if (parts.size() != 3) return "";
    std::string payload_str;
    if (!b64url_decode(parts[1], payload_str)) return "";
    try {
        auto p = json::parse(payload_str);
        if (p.contains("sub") && p["sub"].is_string()) return p["sub"].get<std::string>();
    } catch (...) {}
    return "";
}
