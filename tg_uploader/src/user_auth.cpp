#include "user_auth.h"
#include "config.h"
#include <crypt.h>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {
std::mutex                          g_mu;
std::map<std::string, std::string>  g_users;     // username → bcrypt hash
std::string                         g_secret;

// base64url (no padding) encode
std::string b64url_encode(const std::string& in) {
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    int val = 0, valb = -6;
    for (unsigned char c : in) {
        val = (val << 8) | c;
        valb += 8;
        while (valb >= 0) {
            out.push_back(T[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6) out.push_back(T[((val << 8) >> (valb + 8)) & 0x3F]);
    return out;
}

std::string read_or_create_secret() {
    const char* env = std::getenv("CTG_WEB_JWT_SECRET");
    if (env && *env) return std::string(env);

    std::string path = get_base_dir() + "/.account_configs/.web_jwt_secret";
    std::ifstream f(path);
    if (f.is_open()) {
        std::string s;
        std::getline(f, s);
        if (!s.empty()) return s;
    }
    // Generate a fresh 32-byte hex secret and persist it.
    unsigned char buf[32];
    if (RAND_bytes(buf, sizeof(buf)) != 1) {
        std::cerr << "[AUTH] RAND_bytes failed — using fallback\n";
        for (auto& c : buf) c = static_cast<unsigned char>(std::rand() & 0xFF);
    }
    char hex[65] = {0};
    for (int i = 0; i < 32; i++) std::sprintf(hex + i * 2, "%02x", buf[i]);
    std::ofstream out(path);
    out << hex << "\n";
    out.close();
    fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write);
    std::cout << "[AUTH] wrote fresh web JWT secret to " << path << "\n";
    return std::string(hex);
}
}  // namespace

bool user_auth_init() {
    std::lock_guard<std::mutex> lk(g_mu);
    g_secret = read_or_create_secret();

    std::string path = get_base_dir() + "/web/users.json";
    std::ifstream f(path);
    if (!f.is_open()) {
        std::cerr << "[AUTH] " << path << " not found — login will fail\n";
        return false;
    }
    try {
        json j; f >> j;
        g_users.clear();
        for (auto& [k, v] : j.items()) {
            if (v.is_string()) g_users[k] = v.get<std::string>();
        }
        std::cout << "[AUTH] loaded " << g_users.size()
                  << " user(s) from " << path << "\n";
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[AUTH] failed to parse users.json: " << e.what() << "\n";
        return false;
    }
}

bool user_auth_verify(const std::string& username, const std::string& password) {
    std::string hash;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        auto it = g_users.find(username);
        if (it == g_users.end()) return false;
        hash = it->second;
    }
    if (hash.empty()) return false;

    struct crypt_data data;
    data.initialized = 0;
    char* res = crypt_r(password.c_str(), hash.c_str(), &data);
    if (!res) return false;
    // Constant-time compare — crypt_r returns a pointer into `data`
    if (std::strlen(res) != hash.size()) return false;
    unsigned char acc = 0;
    for (size_t i = 0; i < hash.size(); i++) {
        acc |= static_cast<unsigned char>(res[i]) ^ static_cast<unsigned char>(hash[i]);
    }
    return acc == 0;
}

std::string user_auth_sign_token(const std::string& username, int ttl_hours) {
    std::string secret;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        secret = g_secret;
    }
    if (secret.empty()) return "";

    std::time_t exp = std::time(nullptr) + static_cast<std::time_t>(ttl_hours) * 3600;
    json header   = { {"alg", "HS256"}, {"typ", "JWT"} };
    json payload  = { {"sub", username}, {"exp", exp}, {"iat", std::time(nullptr)} };

    std::string h_b64 = b64url_encode(header.dump());
    std::string p_b64 = b64url_encode(payload.dump());
    std::string signing_input = h_b64 + "." + p_b64;

    unsigned char mac[EVP_MAX_MD_SIZE];
    unsigned int mac_len = 0;
    HMAC(EVP_sha256(),
         secret.data(), static_cast<int>(secret.size()),
         reinterpret_cast<const unsigned char*>(signing_input.data()),
         signing_input.size(),
         mac, &mac_len);
    if (mac_len != 32) return "";

    std::string sig_b64 = b64url_encode(std::string(reinterpret_cast<char*>(mac), mac_len));
    return signing_input + "." + sig_b64;
}

const std::string& web_jwt_secret() {
    // Lock-free read after init() — secret is set once at startup
    return g_secret;
}

namespace {
// Generate a bcrypt salt for $2b$ cost=12. Format: $2b$12$<22 base64 chars>
std::string bcrypt_gensalt(int cost = 12) {
    static const char* B64 = "./ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    unsigned char raw[16];
    if (RAND_bytes(raw, sizeof(raw)) != 1) {
        for (auto& c : raw) c = static_cast<unsigned char>(std::rand() & 0xFF);
    }
    // bcrypt's salt encoding uses a custom 22-char base64 of 16 bytes.
    std::string s;
    s.reserve(22);
    int bitbuf = 0, bitcnt = 0;
    for (int i = 0; i < 16; i++) {
        bitbuf = (bitbuf << 8) | raw[i];
        bitcnt += 8;
        while (bitcnt >= 6) {
            bitcnt -= 6;
            s.push_back(B64[(bitbuf >> bitcnt) & 0x3F]);
        }
    }
    if (bitcnt > 0) s.push_back(B64[(bitbuf << (6 - bitcnt)) & 0x3F]);
    while (s.size() < 22) s.push_back('.');
    s = s.substr(0, 22);

    char prefix[16];
    std::snprintf(prefix, sizeof(prefix), "$2b$%02d$", cost);
    return std::string(prefix) + s;
}
}  // namespace

std::string user_auth_set_password(const std::string& username,
                                   const std::string& new_password) {
    if (username.empty()) return "username required";
    if (new_password.size() < 4)  return "password too short";
    if (new_password.size() > 72) return "password too long (max 72 chars)";

    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_users.find(username) == g_users.end()) return "unknown user";
    }

    // Generate bcrypt hash with crypt_r() using a freshly generated $2b$12$ salt.
    std::string salt = bcrypt_gensalt(12);
    struct crypt_data data;
    data.initialized = 0;
    char* hashed = crypt_r(new_password.c_str(), salt.c_str(), &data);
    if (!hashed || std::strlen(hashed) < 50) return "hash generation failed";
    std::string new_hash(hashed);

    // Atomic write of users.json with all keys preserved.
    std::string path = get_base_dir() + "/web/users.json";
    json users;
    {
        std::ifstream f(path);
        if (!f.is_open()) return "users.json not readable";
        try { users = json::parse(f); }
        catch (const std::exception& e) { return std::string("users.json parse error: ") + e.what(); }
    }
    users[username] = new_hash;

    std::string tmp = path + ".tmp";
    try {
        std::ofstream out(tmp);
        if (!out) return "cannot open tmp file for write";
        out << users.dump(2) << "\n";
        out.close();
        if (out.fail()) return "tmp write failed";
        fs::rename(tmp, path);
    } catch (const std::exception& e) {
        try { fs::remove(tmp); } catch (...) {}
        return std::string("rename failed: ") + e.what();
    }

    // Update in-memory map.
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_users[username] = new_hash;
    }
    return "";   // empty == success
}
