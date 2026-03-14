#pragma once
#include "config.h"
#include <atomic>
#include <mutex>
#include <string>
#include <vector>
#include <ctime>

struct BotEntry {
    std::string name;
    std::string token;
};

struct BotConfig {
    std::string api_url;    // e.g. "http://127.0.0.1:8081"
    std::string channel_id; // e.g. "@mychannel" or "-100123456789"
    std::vector<BotEntry> bots;
};

// Load bot config from .account_configs/bots.json
BotConfig load_bot_config();

// Uploads videos to Telegram via a self-hosted Bot API local server.
// Single bot: sequential upload with flood-wait rotation.
// Multiple bots: each bot runs in its own thread (parallel uploads).
class BotUploader {
public:
    BotUploader(const BotConfig& config, const SharedSettings& settings);

    void upload_directory(const std::string& dir_path, bool recursive);

private:
    BotConfig config_;
    SharedSettings settings_;

    // --- Sequential (single-bot / rotation) ---
    std::vector<std::time_t> bot_flood_until_;
    int current_bot_idx_ = 0;
    int find_available_bot();
    void mark_bot_flood(int idx, int retry_after_secs);
    bool upload_single(const std::string& video_path);

    // --- Parallel (multi-bot, one thread per bot) ---
    std::mutex log_mutex_;
    std::atomic<int> flood_wait_hits_{0};
    std::atomic<int> flood_wait_total_secs_{0};
    bool upload_with_bot(int bot_idx, const std::string& video_path);

    // --- Shared helpers ---
    std::vector<std::string> scan_videos(const std::string& dir, bool recursive);
    std::string find_thumbnail(const std::string& video_path);
    bool is_in_exempt_folder(const std::string& file_path);
    void handle_post_upload(const std::string& path);
};
