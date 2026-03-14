#include "config.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <cstdlib>
#include <algorithm>

using json = nlohmann::json;

static std::string g_base_dir;

void set_base_dir(const std::string& dir) { g_base_dir = dir; }
std::string get_base_dir() { return g_base_dir; }

static std::string configs_dir() {
    return g_base_dir + "/.account_configs";
}

// Read a JSON file, return parsed json or throw
static json read_json(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        throw std::runtime_error("Cannot open: " + path);
    }
    return json::parse(f);
}

// Parse .env file into a map
static std::unordered_map<std::string, std::string> parse_env(const std::string& path) {
    std::unordered_map<std::string, std::string> env;
    std::ifstream f(path);
    if (!f.is_open()) return env;

    std::string line;
    while (std::getline(f, line)) {
        // skip comments and empty lines
        if (line.empty() || line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        // trim whitespace
        while (!key.empty() && key.back() == ' ') key.pop_back();
        while (!val.empty() && val.front() == ' ') val.erase(val.begin());
        // strip surrounding quotes (single or double)
        if (val.size() >= 2 &&
            ((val.front() == '"' && val.back() == '"') ||
             (val.front() == '\'' && val.back() == '\''))) {
            val = val.substr(1, val.size() - 2);
        }
        env[key] = val;
    }
    return env;
}

AccountConfig load_account(const std::string& name) {
    std::string mapping_path = configs_dir() + "/accounts.json";
    json mapping = read_json(mapping_path);

    if (!mapping.contains(name)) {
        std::string available;
        for (auto& [k, v] : mapping.items()) {
            if (!available.empty()) available += ", ";
            available += k;
        }
        throw std::runtime_error("Account '" + name + "' not found. Available: " + available);
    }

    std::string phone_digits = mapping[name].get<std::string>();
    std::string config_path = configs_dir() + "/" + phone_digits + ".json";
    json data = read_json(config_path);

    AccountConfig cfg;
    cfg.name = name;
    cfg.phone = data["phone"].get<std::string>();
    cfg.api_id = data["api_id"].is_number()
                 ? data["api_id"].get<int>()
                 : std::stoi(data["api_id"].get<std::string>());
    cfg.api_hash = data["api_hash"].get<std::string>();
    cfg.channel_id = data.value("channel_id", "me");
    cfg.session_dir = g_base_dir + "/tdlib_" + name;
    return cfg;
}

std::vector<AccountConfig> list_accounts() {
    std::string mapping_path = configs_dir() + "/accounts.json";
    std::vector<AccountConfig> accounts;

    json mapping;
    try {
        mapping = read_json(mapping_path);
    } catch (...) {
        return accounts;
    }

    for (auto& [name, phone_val] : mapping.items()) {
        std::string phone_digits = phone_val.get<std::string>();
        std::string config_path = configs_dir() + "/" + phone_digits + ".json";
        try {
            json data = read_json(config_path);
            AccountConfig cfg;
            cfg.name = name;
            cfg.phone = data["phone"].get<std::string>();
            cfg.api_id = data["api_id"].is_number()
                 ? data["api_id"].get<int>()
                 : std::stoi(data["api_id"].get<std::string>());
            cfg.api_hash = data["api_hash"].get<std::string>();
            cfg.channel_id = data.value("channel_id", "me");
            cfg.session_dir = g_base_dir + "/tdlib_" + name;
            accounts.push_back(std::move(cfg));
        } catch (const std::exception& e) {
            std::cerr << "Warning: config missing for " << name << ": " << e.what() << "\n";
        }
    }
    return accounts;
}

SharedSettings load_settings() {
    SharedSettings s;
    std::string env_path = g_base_dir + "/.env";
    auto env = parse_env(env_path);

    auto get = [&](const std::string& key, const std::string& def) -> std::string {
        auto it = env.find(key);
        return (it != env.end()) ? it->second : def;
    };

    auto to_lower = [](std::string v) {
        std::transform(v.begin(), v.end(), v.begin(), ::tolower);
        return v;
    };

    s.delete_after_upload = to_lower(get("DELETE_AFTER_UPLOAD", "false")) == "true";
    s.mark_uploaded_files = to_lower(get("MARK_UPLOADED_FILES", "true")) == "true";
    s.uploaded_suffix = get("UPLOADED_SUFFIX", ".uploaded");
    s.default_upload_dir = get("DEFAULT_UPLOAD_DIR", "");

    std::string exempt_str = get("EXEMPT_FOLDERS", "");
    if (!exempt_str.empty()) {
        std::string token;
        for (char c : exempt_str) {
            if (c == ',') {
                // trim
                while (!token.empty() && token.front() == ' ') token.erase(token.begin());
                while (!token.empty() && token.back() == ' ') token.pop_back();
                if (!token.empty()) s.exempt_folders.push_back(token);
                token.clear();
            } else {
                token += c;
            }
        }
        while (!token.empty() && token.front() == ' ') token.erase(token.begin());
        while (!token.empty() && token.back() == ' ') token.pop_back();
        if (!token.empty()) s.exempt_folders.push_back(token);
    }

    return s;
}
