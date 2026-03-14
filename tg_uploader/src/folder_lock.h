#pragma once
#include <string>

bool create_folder_lock(const std::string& folder, const std::string& account_name);
bool is_folder_locked(const std::string& folder);
void remove_folder_lock(const std::string& folder);
