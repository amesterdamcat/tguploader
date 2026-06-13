#include "lequ_sign.h"

#include <openssl/sha.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <iostream>
#include <string>

namespace {

// ── Static constants (extracted from libgojni.so via IDA Pro) ───────────────
// 48-byte GatewayKey (3×16 segments)
const unsigned char GATEWAY_KEY[48] = {
    0x29,0xdc,0x8f,0x42,0x6d,0x3f,0xb8,0xf5,0xb2,0x74,0x71,0x48,0x47,0x8a,0x8b,0xf0,
    0xef,0x6c,0xd3,0x5d,0xc9,0xb7,0x8b,0x77,0x00,0x13,0x23,0x3c,0x19,0x99,0xac,0x36,
    0xad,0xdd,0x6f,0xb2,0x3b,0x61,0x62,0xea,0x7b,0xb4,0xd2,0x1b,0xad,0x75,0xa4,0xbd,
};
// 16-byte GatewaySignKey  ("h;1TK!M&A,I6z2s7")
const unsigned char GATEWAY_SIGN_KEY[16] = {
    0x68,0x3b,0x31,0x54,0x4b,0x21,0x4d,0x26,0x41,0x2c,0x49,0x36,0x7a,0x32,0x73,0x37,
};
// 16-byte GatewayEncryptionKey  ("Wr~4a-V4wXM6:v[6")
const unsigned char GATEWAY_ENC_KEY[16] = {
    0x57,0x72,0x7e,0x34,0x61,0x2d,0x56,0x34,0x77,0x58,0x4d,0x36,0x3a,0x76,0x5b,0x36,
};

constexpr int  G_SIGN_BYTES = 24;     // SHA512 truncation length
const char* GATEWAY_SIGN_VERSION = "M1.0";
const char* GATEWAY_ENC_VERSION  = "E1.0";

// SFib(GatewayKey, GatewaySignKey): 16 raw bytes → lowercase hex (32 chars).
//   raw[i] = key[i] ^ key[i+16] ^ signKey[i]
std::string sfib() {
    static const std::string cached = [] {
        static const char* H = "0123456789abcdef";
        std::string s;
        s.reserve(32);
        for (int i = 0; i < 16; ++i) {
            unsigned char b = GATEWAY_KEY[i] ^ GATEWAY_KEY[i + 16] ^ GATEWAY_SIGN_KEY[i];
            s.push_back(H[b >> 4]);
            s.push_back(H[b & 0xF]);
        }
        return s;
    }();
    return cached;
}

// Body AES-CBC key: key[i+16] ^ key[i+32] ^ encKey[i]  (16 bytes)
const std::array<unsigned char, 16>& body_key() {
    static const std::array<unsigned char, 16> k = [] {
        std::array<unsigned char, 16> a{};
        for (int i = 0; i < 16; ++i)
            a[i] = GATEWAY_KEY[i + 16] ^ GATEWAY_KEY[i + 32] ^ GATEWAY_ENC_KEY[i];
        return a;
    }();
    return k;
}

// AES-CBC IV derived from the 16-byte nonce (NOT the raw nonce):
//   IV[0:4]  = nonce[i] ^ nonce[i+8] ^ nonce[i+12]   (i = 0..3)
//   IV[4:16] = nonce[0:12]
std::array<unsigned char, 16> derive_iv(const unsigned char nonce[16]) {
    std::array<unsigned char, 16> iv{};
    for (int i = 0; i < 4; ++i)
        iv[i] = nonce[i] ^ nonce[i + 8] ^ nonce[i + 12];
    std::memcpy(iv.data() + 4, nonce, 12);
    return iv;
}

// AES-128-CBC encrypt with PKCS#7 padding → base64.
std::string aes_cbc_b64(const std::string& plain,
                        const unsigned char key[16],
                        const unsigned char iv[16]) {
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return "";
    std::string out;
    if (EVP_EncryptInit_ex(ctx, EVP_aes_128_cbc(), nullptr, key, iv) == 1) {
        std::string buf;
        buf.resize(plain.size() + EVP_MAX_BLOCK_LENGTH);
        int len = 0, total = 0;
        if (EVP_EncryptUpdate(ctx, reinterpret_cast<unsigned char*>(&buf[0]), &len,
                              reinterpret_cast<const unsigned char*>(plain.data()),
                              static_cast<int>(plain.size())) == 1) {
            total = len;
            if (EVP_EncryptFinal_ex(ctx, reinterpret_cast<unsigned char*>(&buf[0]) + total,
                                    &len) == 1) {
                total += len;
                out = lequ_b64_encode(reinterpret_cast<const unsigned char*>(buf.data()),
                                      static_cast<size_t>(total));
            }
        }
    }
    EVP_CIPHER_CTX_free(ctx);
    return out;
}

// 16-byte nonce: 12 random || 4 (random[i] ^ big_endian_uint32(unix_seconds)[i])
void make_nonce(unsigned char out[16]) {
    RAND_bytes(out, 12);
    uint32_t t = static_cast<uint32_t>(std::time(nullptr));
    unsigned char tb[4] = {
        static_cast<unsigned char>((t >> 24) & 0xFF),
        static_cast<unsigned char>((t >> 16) & 0xFF),
        static_cast<unsigned char>((t >> 8) & 0xFF),
        static_cast<unsigned char>(t & 0xFF),
    };
    for (int i = 0; i < 4; ++i) out[12 + i] = out[i] ^ tb[i];
}

}  // namespace

// ── base64 (standard alphabet, with padding) ────────────────────────────────
std::string lequ_b64_encode(const unsigned char* data, size_t len) {
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    size_t i = 0;
    for (; i + 3 <= len; i += 3) {
        uint32_t n = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out.push_back(T[(n >> 18) & 63]);
        out.push_back(T[(n >> 12) & 63]);
        out.push_back(T[(n >> 6) & 63]);
        out.push_back(T[n & 63]);
    }
    if (len - i == 1) {
        uint32_t n = data[i] << 16;
        out.push_back(T[(n >> 18) & 63]);
        out.push_back(T[(n >> 12) & 63]);
        out.push_back('=');
        out.push_back('=');
    } else if (len - i == 2) {
        uint32_t n = (data[i] << 16) | (data[i + 1] << 8);
        out.push_back(T[(n >> 18) & 63]);
        out.push_back(T[(n >> 12) & 63]);
        out.push_back(T[(n >> 6) & 63]);
        out.push_back('=');
    }
    return out;
}

std::string lequ_b64_decode(const std::string& in) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::string out;
    int buf = 0, bits = 0;
    for (char c : in) {
        if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
        int v = val(c);
        if (v < 0) return "";
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((buf >> bits) & 0xFF));
        }
    }
    return out;
}

// ── core signing ────────────────────────────────────────────────────────────
LequSigned lequ_sign_with_nonce(const std::string& path, const std::string& auth,
                                const std::string& body, int ect, bool encrypt,
                                const unsigned char nonce[16]) {
    LequSigned r;
    r.el_ns  = lequ_b64_encode(nonce, 16);
    r.el_ect = std::to_string(ect);

    std::string body_sign;
    if (encrypt && !body.empty()) {
        auto iv = derive_iv(nonce);
        std::string enc = aes_cbc_b64(body, body_key().data(), iv.data());
        r.body    = enc;
        body_sign = enc;
        r.el_ver  = GATEWAY_ENC_VERSION;   // E1.0
    } else {
        r.body    = body;
        body_sign = body;
        r.el_ver  = GATEWAY_SIGN_VERSION;  // M1.0
    }

    std::string sha_in = sfib() + auth + r.el_ect + r.el_ns + r.el_ver + path + body_sign;
    unsigned char digest[SHA512_DIGEST_LENGTH];
    SHA512(reinterpret_cast<const unsigned char*>(sha_in.data()), sha_in.size(), digest);
    r.el_sign = lequ_b64_encode(digest, G_SIGN_BYTES);
    return r;
}

LequSigned lequ_sign(const std::string& path, const std::string& auth,
                     const std::string& body, int ect, bool encrypt) {
    unsigned char nonce[16];
    make_nonce(nonce);
    return lequ_sign_with_nonce(path, auth, body, ect, encrypt, nonce);
}

// ── self-test against captured Frida vectors (2026-06-06) ───────────────────
bool lequ_sign_selftest() {
    const std::string AUTH = "dI29g4x9fBImmzKkM2gfUQKGFDCMrfU9";
    struct Case {
        const char* label; const char* path; const char* body; const char* el_ns;
        int ect; bool encrypt; const char* exp_sign; const char* exp_enc;
    };
    const Case cases[] = {
        {"Test1 empty", "/app/video/topic/list", "", "Q5voeK8+QweXcMqmKbgLZw==",
         1, false, "vF8O1/hE6jmIAR8xhJG8OiVOusx4MSMQ", ""},
        {"Test2 enc", "/app/video/topicvideolist",
         "count=20&start=0&type=1&tid=134&live=2", "xGZ6UZU71osnXwACrkWZcQ==",
         1, true, "Q0hSTHeq22HlWQvamoKRxvjlB9DFXyxG",
         "AAT7oMalwmXNu35kFV2oPRyknSGumR79Us6wGSj0kmZ0ROGJpNXHhNt/AsPJKebZ"},
        {"Test3 empty", "/app/user/init/info", "", "+SWvkfOtSZqUcbpxkwZMtg==",
         1, false, "knoJonI6bSR/YfYdKFjONsIp+vQ9WV+g", ""},
        {"Test4 enc", "/app/user/userInfo",
         "field=all&name=43417392&extendField=percent%2Cpersonal",
         "umj8rOmI7yaJcM8c0Esfiw==", 1, true,
         "WzvAclJ0kZM3Dul/oCl6K5yS8pV9MyxD",
         "ZaO8qrFx838QWM2MvdqPNF9P/yb8ZucqBRgqXfzGIs+Wgr/iF3/xk7Jz+hOcquk5oBeNVPKukjtb92FV4EML8g=="},
    };
    bool all = true;
    for (const auto& c : cases) {
        std::string nonce = lequ_b64_decode(c.el_ns);
        if (nonce.size() != 16) { std::cout << "  FAIL " << c.label << " (bad nonce)\n"; all = false; continue; }
        LequSigned s = lequ_sign_with_nonce(c.path, AUTH, c.body, c.ect, c.encrypt,
                                            reinterpret_cast<const unsigned char*>(nonce.data()));
        bool ok = (s.el_sign == c.exp_sign);
        if (c.encrypt && c.body[0]) ok = ok && (s.body == c.exp_enc);
        std::cout << (ok ? "  PASS " : "  FAIL ") << c.label << "\n";
        if (!ok) {
            all = false;
            std::cout << "        sign got: " << s.el_sign << "\n        sign exp: " << c.exp_sign << "\n";
            if (c.encrypt && c.body[0])
                std::cout << "        enc  got: " << s.body << "\n        enc  exp: " << c.exp_enc << "\n";
        }
    }
    std::cout << (all ? "  Zeus sign self-test: ALL PASS\n" : "  Zeus sign self-test: FAILED\n");
    return all;
}
