#pragma once
#include <string>
#include <vector>

struct AccountConfig {
    std::string name;
    std::string phone;
    int api_id = 0;
    std::string api_hash;
    std::string channel_id;
    std::string session_dir;  // TDLib database directory, e.g. "tdlib_wang"
};

struct SharedSettings {
    bool delete_after_upload = false;
    bool mark_uploaded_files = true;
    std::string uploaded_suffix = ".uploaded";
    std::vector<std::string> exempt_folders;
    std::string default_upload_dir;
};

// base_dir = directory containing the executable (or where .account_configs lives)
void set_base_dir(const std::string& dir);
std::string get_base_dir();

AccountConfig load_account(const std::string& name);
std::vector<AccountConfig> list_accounts();
SharedSettings load_settings();
