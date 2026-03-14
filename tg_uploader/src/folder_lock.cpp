#include "folder_lock.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <ctime>
#include <chrono>
#include <csignal>
#include <unistd.h>
#include <fcntl.h>

using json = nlohmann::json;

static std::string lock_path(const std::string& folder) {
    return folder + "/.uploading.lock";
}

static std::string now_iso() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", std::localtime(&t));
    return buf;
}

static bool pid_alive(int pid) {
    return kill(static_cast<pid_t>(pid), 0) == 0;
}

bool create_folder_lock(const std::string& folder, const std::string& account_name) {
    try {
        std::string lp = lock_path(folder);

        // Atomic creation: O_CREAT | O_EXCL fails if file already exists
        int fd = ::open(lp.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (fd < 0) {
            // File already exists — check if it's a stale lock
            if (is_folder_locked(folder)) {
                return false;  // genuinely locked by another process
            }
            // Stale lock was cleaned by is_folder_locked, retry atomic create
            fd = ::open(lp.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
            if (fd < 0) return false;
        }

        json lock_info = {
            {"pid", static_cast<int>(getpid())},
            {"account", account_name},
            {"start_time", now_iso()},
            {"folder_path", folder}
        };
        std::string data = lock_info.dump(2);
        ::write(fd, data.c_str(), data.size());
        ::close(fd);

        std::cout << "[INFO] Locked folder: " << folder << "\n";
        return true;
    } catch (const std::exception& e) {
        std::cerr << "\033[31m[ERROR]\033[0m Failed to lock folder: " << e.what() << "\n";
        return false;
    }
}

bool is_folder_locked(const std::string& folder) {
    try {
        std::string lp = lock_path(folder);
        std::ifstream f(lp);
        if (!f.is_open()) return false;

        json lock_info = json::parse(f);
        f.close();

        int pid = lock_info.value("pid", 0);
        if (pid > 0) {
            if (pid_alive(pid)) {
                std::string account = lock_info.value("account", "unknown");
                std::cout << "[INFO] Folder locked: " << folder
                          << " (pid:" << pid << ", account:" << account << ")\n";
                return true;
            }
            // Process dead, clean stale lock
            std::cout << "[INFO] Cleaning stale lock: " << lp << "\n";
            std::remove(lp.c_str());
            return false;
        }

        // Check time-based expiry (6 hours)
        std::string start_time = lock_info.value("start_time", "");
        if (!start_time.empty()) {
            struct std::tm tm = {};
            if (strptime(start_time.c_str(), "%Y-%m-%dT%H:%M:%S", &tm)) {
                auto lock_time = std::chrono::system_clock::from_time_t(std::mktime(&tm));
                auto now = std::chrono::system_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - lock_time).count();
                if (elapsed > 6 * 3600) {
                    std::remove(lp.c_str());
                    return false;
                }
            }
        }

        return true;
    } catch (...) {
        return false;
    }
}

void remove_folder_lock(const std::string& folder) {
    std::string lp = lock_path(folder);
    if (std::remove(lp.c_str()) == 0) {
        std::cout << "[INFO] Unlocked folder: " << folder << "\n";
    }
}
