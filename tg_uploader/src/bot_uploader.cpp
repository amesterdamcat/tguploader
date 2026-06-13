#include "bot_uploader.h"
#include "bot_client.h"
#include "caption.h"
#include "scanner.h"
#include "utils.h"
#include "video_info.h"
#include "log_tee.h"     // set_log_channel
#include "web_server.h"  // g_cancel_botupload, fixbig_is_active()
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <nlohmann/json.hpp>
#include <queue>
#include <random>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace fs = std::filesystem;
using json = nlohmann::json;

// Bot API (via TDLib) hard limit: 4000 parts × 512 KB = 2,097,152,000 bytes
static constexpr int64_t BOT_FILE_SIZE_LIMIT = 4000LL * 512 * 1024;

// ---------------------------------------------------------------------------
// HTML caption helpers
// ---------------------------------------------------------------------------

// Escape characters special to HTML: &, <, >
static std::string html_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if      (c == '&')  out += "&amp;";
        else if (c == '<')  out += "&lt;";
        else if (c == '>')  out += "&gt;";
        else                out += c;
    }
    return out;
}

// Bot API uses parse_mode=HTML. Use <pre> for the code block so we don't need
// to escape markdown special chars in filenames and hashtags.
static std::string make_bot_video_caption(const std::string& file_path, const VideoInfo& info) {
    std::string file_name = fs::path(file_path).filename().string();
    std::string hashtag   = extract_hashtags(file_path);

    int64_t file_size = 0;
    struct stat st;
    if (::stat(file_path.c_str(), &st) == 0) file_size = st.st_size;

    int w = info.width  > 0 ? info.width  : 1920;
    int h = info.height > 0 ? info.height : 1080;

    std::string cap = "<pre>" + html_escape(file_name) + "\n"
                    + "Resolution: " + std::to_string(w) + "x" + std::to_string(h) + "\n"
                    + "Duration: "   + format_duration(info.duration) + "\n"
                    + "Size: "       + format_file_size(file_size) + "\n"
                    + "</pre>";
    if (!hashtag.empty()) cap += "\n" + hashtag;

    // Bot API caption limit is 1024 chars; truncate if needed
    if (cap.size() > 1024) cap.resize(1024);
    return cap;
}

static std::string make_bot_image_caption(const std::string& file_path, int width, int height) {
    std::string file_name = fs::path(file_path).filename().string();
    std::string hashtag   = extract_hashtags(file_path);

    int64_t file_size = 0;
    struct stat st;
    if (::stat(file_path.c_str(), &st) == 0) file_size = st.st_size;

    std::string cap = "<pre>" + html_escape(file_name) + "\n";
    if (width > 0 && height > 0) {
        cap += "Resolution: " + std::to_string(width) + "x" + std::to_string(height) + "\n";
    }
    cap += "Size: " + format_file_size(file_size) + "\n</pre>";
    if (!hashtag.empty()) cap += "\n" + hashtag;
    if (cap.size() > 1024) cap.resize(1024);
    return cap;
}

// ---------------------------------------------------------------------------
// BotConfig loading
// ---------------------------------------------------------------------------

BotConfig load_bot_config() {
    std::string base = get_base_dir();
    std::string path = base + "/.account_configs/bots.json";

    std::ifstream f(path);
    if (!f.is_open()) {
        throw std::runtime_error(
            "Cannot open: " + path + "\n"
            "Create .account_configs/bots.json:\n"
            "{\n"
            "  \"api_url\": \"http://127.0.0.1:8081\",\n"
            "  \"channel_id\": \"@yourchannel\",\n"
            "  \"bots\": {\n"
            "    \"bot1\": \"1234:TOKEN1\",\n"
            "    \"bot2\": \"5678:TOKEN2\"\n"
            "  }\n"
            "}\n"
            "Then start the local server:\n"
            "  telegram-bot-api --api-id=ID --api-hash=HASH --local");
    }

    json data = json::parse(f);
    BotConfig cfg;
    cfg.api_url    = data.value("api_url", "http://127.0.0.1:8081");
    cfg.channel_id = data["channel_id"].get<std::string>();

    if (data.contains("bots") && data["bots"].is_object()) {
        for (auto& [name, token_val] : data["bots"].items()) {
            BotEntry e;
            e.name  = name;
            e.token = token_val.get<std::string>();
            cfg.bots.push_back(std::move(e));
        }
    }

    if (cfg.bots.empty())      throw std::runtime_error("No bots configured in bots.json");
    if (cfg.channel_id.empty()) throw std::runtime_error("channel_id missing in bots.json");

    return cfg;
}

// ---------------------------------------------------------------------------
// BotUploader
// ---------------------------------------------------------------------------

BotUploader::BotUploader(const BotConfig& config, const SharedSettings& settings)
    : config_(config), settings_(settings),
      bot_flood_until_(config.bots.size(), 0) {}

int BotUploader::find_available_bot() {
    int n = static_cast<int>(config_.bots.size());
    while (true) {
        auto now = std::time(nullptr);
        for (int i = 0; i < n; i++) {
            int idx = (current_bot_idx_ + i) % n;
            if (now >= bot_flood_until_[idx]) {
                current_bot_idx_ = idx;
                return idx;
            }
        }
        // All bots on cooldown — wait for the one with the shortest remaining wait
        std::time_t earliest = bot_flood_until_[0];
        int earliest_idx = 0;
        for (int i = 1; i < n; i++) {
            if (bot_flood_until_[i] < earliest) {
                earliest = bot_flood_until_[i];
                earliest_idx = i;
            }
        }
        int wait_secs = static_cast<int>(earliest - now);
        if (wait_secs > 0) {
            std::cout << "\033[33m[BOT]\033[0m All bots on FLOOD_WAIT. "
                      << "Waiting " << wait_secs << "s for "
                      << config_.bots[earliest_idx].name << "...\n";
            std::this_thread::sleep_for(std::chrono::seconds(wait_secs));
        }
        current_bot_idx_ = earliest_idx;
    }
}

void BotUploader::mark_bot_flood(int idx, int retry_after_secs) {
    bot_flood_until_[idx] = std::time(nullptr) + retry_after_secs + 1;
    flood_wait_hits_++;
    flood_wait_total_secs_ += retry_after_secs;
    std::cout << "\033[33m[WARNING]\033[0m " << config_.bots[idx].name
              << " FLOOD_WAIT " << retry_after_secs << "s, rotating to next bot\n";
    // Advance index so the next call to find_available_bot starts from here+1
    current_bot_idx_ = (idx + 1) % static_cast<int>(config_.bots.size());
}

std::vector<std::string> BotUploader::scan_videos(const std::string& dir, bool recursive) {
    std::vector<std::string> videos;

    auto is_available = [](const std::string& path) {
        if (path.size() >= 9 && path.substr(path.size() - 9) == ".uploaded") return false;
        if (path.size() >= 4 && path.substr(path.size() - 4) == ".big")      return false;
        struct stat st;
        if (::stat((path + ".uploaded").c_str(), &st) == 0) return false;
        if (::stat((path + ".big").c_str(),      &st) == 0) return false;
        return true;
    };

    try {
        if (recursive) {
            for (auto& entry : fs::recursive_directory_iterator(
                     dir, fs::directory_options::skip_permission_denied)) {
                if (entry.is_regular_file()) {
                    std::string path = entry.path().string();
                    if (is_video_file(path) && is_available(path))
                        videos.push_back(path);
                }
            }
        } else {
            for (auto& entry : fs::directory_iterator(dir)) {
                if (entry.is_regular_file()) {
                    std::string path = entry.path().string();
                    if (is_video_file(path) && is_available(path))
                        videos.push_back(path);
                }
            }
        }
    } catch (const fs::filesystem_error& e) {
        std::cerr << "\033[31m[ERROR]\033[0m Directory scan failed: " << e.what() << "\n";
    }

    return videos;
}

std::string BotUploader::find_thumbnail(const std::string& video_path) {
    fs::path p(video_path);
    fs::path thumb = p.parent_path() / (p.stem().string() + ".jpg");
    return fs::exists(thumb) ? thumb.string() : "";
}

bool BotUploader::is_in_exempt_folder(const std::string& file_path) {
    if (settings_.exempt_folders.empty()) return false;
    std::string abs_path = fs::absolute(file_path).string();
    for (const auto& exempt : settings_.exempt_folders) {
        std::string exempt_abs = fs::absolute(exempt).string();
        if (abs_path.find(exempt_abs) == 0) {
            std::cout << "[INFO] File in exempt folder, won't delete: " << file_path << "\n";
            return true;
        }
    }
    return false;
}

void BotUploader::handle_post_upload(const std::string& path) {
    if (settings_.delete_after_upload) {
        if (is_in_exempt_folder(path)) {
            // Exempt folder: mark instead of delete (same behaviour as TDLib uploader)
            if (settings_.mark_uploaded_files) {
                try {
                    fs::rename(path, path + settings_.uploaded_suffix);
                    std::cout << "[INFO] Marked: " << fs::path(path).filename().string() << "\n";
                } catch (const std::exception& e) {
                    std::cerr << "\033[31m[ERROR]\033[0m Rename failed: " << e.what() << "\n";
                }
            }
        } else {
            try {
                fs::remove(path);
                std::cout << "[INFO] Deleted: " << path << "\n";
            } catch (const std::exception& e) {
                std::cerr << "\033[31m[ERROR]\033[0m Delete failed: " << e.what() << "\n";
            }
        }
    } else if (settings_.mark_uploaded_files) {
        try {
            fs::rename(path, path + settings_.uploaded_suffix);
            std::cout << "[INFO] Marked: " << fs::path(path).filename().string() << "\n";
        } catch (const std::exception& e) {
            std::cerr << "\033[31m[ERROR]\033[0m Rename failed: " << e.what() << "\n";
        }
    }
}

bool BotUploader::upload_single(const std::string& video_path) {
    // Defer to a concurrently running fix-big — don't race on the file.
    if (fixbig_is_active(video_path)) {
        std::cout << "[INFO] Skipping (fix-big active): "
                  << fs::path(video_path).filename().string() << "\n";
        return false;
    }
    struct stat st;
    if (::stat(video_path.c_str(), &st) != 0) {
        std::cerr << "\033[31m[ERROR]\033[0m File not found: " << video_path << "\n";
        return false;
    }
    int64_t file_size = st.st_size;

    if (file_size > BOT_FILE_SIZE_LIMIT) {
        std::string fname = fs::path(video_path).filename().string();
        try {
            fs::rename(video_path, video_path + ".big");
        } catch (const std::exception& e) {
            std::cerr << "\033[31m[ERROR]\033[0m Rename to .big failed: " << e.what() << "\n";
        }
        std::cerr << "\033[33m[SKIP]\033[0m " << fname
                  << " exceeds Bot API limit (" << format_file_size(file_size)
                  << " > 2.0 GiB), renamed to .big\n";
        return false;
    }

    VideoInfo info         = get_video_info(video_path);
    std::string thumb_path = find_thumbnail(video_path);  // original full-res jpg
    std::string caption    = make_bot_video_caption(video_path, info);
    std::string fname      = fs::path(video_path).filename().string();
    std::string model      = fs::path(video_path).parent_path().filename().string();

    // Matches uploader.cpp send_prepared() header exactly
    std::cout << "\n[INFO] Sending: " << fname
              << " (" << format_file_size(file_size) << ") [" << model << "]\n";

    // Resize thumbnail to ≤320×320 — same as TDLib thumbnail_ requirement
    std::string thumb_320;
    if (!thumb_path.empty()) {
        std::cout << "[INFO] Thumbnail: " << fs::path(thumb_path).filename().string() << "\n";
        thumb_320 = create_cover_thumb(thumb_path);
    }
    const std::string& send_thumb = thumb_320.empty() ? thumb_path : thumb_320;

    auto send_start = std::chrono::steady_clock::now();

    // Retry loop: rotate through bots on FLOOD_WAIT
    int max_attempts = static_cast<int>(config_.bots.size()) * 3;
    for (int attempt = 0; attempt < max_attempts; attempt++) {
        int bot_idx = find_available_bot();
        const auto& bot = config_.bots[bot_idx];
        if (attempt > 0 || config_.bots.size() > 1) {
            std::cout << "[INFO] Bot: " << bot.name << "\n";
        }

        BotClient client(config_.api_url, bot.token);
        auto resp = client.send_video(
            config_.channel_id, video_path, caption,
            static_cast<int>(info.duration),
            info.width  > 0 ? info.width  : 1920,
            info.height > 0 ? info.height : 1080,
            send_thumb);

        if (resp.ok) {
            double send_secs = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - send_start).count();
            std::cout << "[INFO] Success: " << fname
                      << " (" << fmt_elapsed(send_secs) << ")\n";

            if (!thumb_320.empty()) {
                try { fs::remove(thumb_320); } catch (...) {}
            }

            // Record to scanner.db — identical to uploader.cpp send_prepared()
            {
                std::string base = get_base_dir();
                std::string db_path = base + "/data/scanner.db";
                std::string thumb_dir = base + "/data/thumbs";
                fs::create_directories(base + "/data/thumbs");
                record_uploaded_video(db_path, thumb_dir, resp.message_id,
                                      fname,
                                      static_cast<int>(info.duration),
                                      info.width  > 0 ? info.width  : 1920,
                                      info.height > 0 ? info.height : 1080,
                                      file_size, caption, thumb_path,
                                      resp.message_id);  // Bot API: use message_id directly as url_id
            }

            // Send original full-res jpg as a separate photo — same as TDLib uploader
            if (!thumb_path.empty()) {
                auto thumb_start = std::chrono::steady_clock::now();
                std::this_thread::sleep_for(std::chrono::seconds(2));
                auto sz = get_jpeg_size(thumb_path);
                std::string img_cap = make_bot_image_caption(thumb_path, sz.width, sz.height);

                int bot_idx2 = find_available_bot();
                BotClient client2(config_.api_url, config_.bots[bot_idx2].token);
                auto photo_resp = client2.send_photo(config_.channel_id, thumb_path, img_cap);
                double thumb_secs = std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - thumb_start).count();

                if (photo_resp.ok) {
                    std::cout << "[INFO] Thumbnail sent: message ID " << photo_resp.message_id
                              << " (" << fmt_elapsed(thumb_secs) << ")\n";
                    handle_post_upload(thumb_path);
                } else if (photo_resp.retry_after > 0) {
                    mark_bot_flood(bot_idx2, photo_resp.retry_after);
                    std::cerr << "\033[33m[WARNING]\033[0m Thumbnail upload failed (video OK)"
                              << " (" << fmt_elapsed(thumb_secs) << ")\n";
                } else {
                    std::cerr << "\033[33m[WARNING]\033[0m Thumbnail upload failed (video OK)"
                              << " (" << fmt_elapsed(thumb_secs) << ")\n";
                }
            }

            return true;

        } else if (resp.retry_after > 0) {
            mark_bot_flood(bot_idx, resp.retry_after);
            // Loop: find_available_bot() picks the next available bot

        } else {
            if (!thumb_320.empty()) {
                try { fs::remove(thumb_320); } catch (...) {}
            }
            double send_secs = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - send_start).count();
            std::cerr << "\033[31m[ERROR]\033[0m Upload failed: " << fname
                      << " (" << fmt_elapsed(send_secs) << ")\n";
            return false;
        }
    }

    if (!thumb_320.empty()) {
        try { fs::remove(thumb_320); } catch (...) {}
    }
    std::cerr << "\033[31m[ERROR]\033[0m Upload failed: " << fname
              << " (all bots exhausted)\n";
    return false;
}

// ---------------------------------------------------------------------------
// Parallel upload: fixed bot index, sleeps on FLOOD_WAIT (no rotation)
// ---------------------------------------------------------------------------

// ANSI colors cycled per bot index
static const char* BOT_COLORS[] = {
    "\033[32m",  // green
    "\033[33m",  // yellow
    "\033[34m",  // blue
    "\033[35m",  // magenta
    "\033[36m",  // cyan
    "\033[31m",  // red
};
static constexpr int N_BOT_COLORS = 6;

bool BotUploader::upload_with_bot(int bot_idx, const std::string& video_path) {
    const auto& bot = config_.bots[bot_idx];
    const char* color = BOT_COLORS[bot_idx % N_BOT_COLORS];
    // colored prefix for log lines, e.g. "\033[32m[PhoUpload_bot]\033[0m "
    const std::string pfx = std::string(color) + "[" + bot.name + "]\033[0m ";

    // Defer to a concurrently running fix-big.
    if (fixbig_is_active(video_path)) {
        std::lock_guard<std::mutex> lk(log_mutex_);
        std::cout << "[INFO] " << pfx << "Skipping (fix-big active): "
                  << fs::path(video_path).filename().string() << "\n";
        return false;
    }

    struct stat st;
    if (::stat(video_path.c_str(), &st) != 0) {
        std::lock_guard<std::mutex> lk(log_mutex_);
        std::cerr << "\033[31m[ERROR]\033[0m " << pfx << "File not found: " << video_path << "\n";
        return false;
    }
    int64_t file_size = st.st_size;

    if (file_size > BOT_FILE_SIZE_LIMIT) {
        std::string fname = fs::path(video_path).filename().string();
        std::string rename_err;
        try {
            fs::rename(video_path, video_path + ".big");
        } catch (const std::exception& e) {
            rename_err = e.what();
        }
        std::lock_guard<std::mutex> lk(log_mutex_);
        if (!rename_err.empty()) {
            std::cerr << "\033[31m[ERROR]\033[0m " << pfx
                      << "Rename to .big failed: " << rename_err << "\n";
        }
        std::cerr << "\033[33m[SKIP]\033[0m " << pfx << fname
                  << " exceeds Bot API limit (" << format_file_size(file_size)
                  << " > 2.0 GiB), renamed to .big\n";
        return false;
    }

    VideoInfo info         = get_video_info(video_path);
    std::string thumb_path = find_thumbnail(video_path);
    std::string caption    = make_bot_video_caption(video_path, info);
    std::string fname      = fs::path(video_path).filename().string();
    std::string model      = fs::path(video_path).parent_path().filename().string();

    {
        std::lock_guard<std::mutex> lk(log_mutex_);
        std::cout << "\n[INFO] " << pfx << "Sending: " << fname
                  << " (" << format_file_size(file_size) << ") [" << model << "]\n";
    }

    std::string thumb_320;
    if (!thumb_path.empty()) {
        {
            std::lock_guard<std::mutex> lk(log_mutex_);
            std::cout << "[INFO] " << pfx << "Thumbnail: "
                      << fs::path(thumb_path).filename().string() << "\n";
        }
        thumb_320 = create_cover_thumb(thumb_path, pfx);
    }
    const std::string& send_thumb = thumb_320.empty() ? thumb_path : thumb_320;

    auto send_start = std::chrono::steady_clock::now();
    int max_flood_retries = 3;

    for (int attempt = 0; attempt <= max_flood_retries; attempt++) {
        BotClient client(config_.api_url, bot.token);
        client.set_progress_prefix(pfx);
        auto resp = client.send_video(
            config_.channel_id, video_path, caption,
            static_cast<int>(info.duration),
            info.width  > 0 ? info.width  : 1920,
            info.height > 0 ? info.height : 1080,
            send_thumb);

        if (resp.ok) {
            double send_secs = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - send_start).count();
            {
                std::lock_guard<std::mutex> lk(log_mutex_);
                std::cout << "[INFO] " << pfx << "Success: " << fname
                          << " (" << fmt_elapsed(send_secs) << ")\n";
            }
            if (!thumb_320.empty()) {
                try { fs::remove(thumb_320); } catch (...) {}
            }

            // Record to scanner.db
            {
                std::string base      = get_base_dir();
                std::string db_path   = base + "/data/scanner.db";
                std::string thumb_dir = base + "/data/thumbs";
                fs::create_directories(thumb_dir);
                record_uploaded_video(db_path, thumb_dir, resp.message_id,
                                      fname,
                                      static_cast<int>(info.duration),
                                      info.width  > 0 ? info.width  : 1920,
                                      info.height > 0 ? info.height : 1080,
                                      file_size, caption, thumb_path,
                                      resp.message_id);
            }

            // Send original full-res jpg as a separate photo
            if (!thumb_path.empty()) {
                auto thumb_start = std::chrono::steady_clock::now();
                std::this_thread::sleep_for(std::chrono::seconds(2));
                auto sz = get_jpeg_size(thumb_path);
                std::string img_cap = make_bot_image_caption(thumb_path, sz.width, sz.height);
                BotClient client2(config_.api_url, bot.token);
                auto photo_resp = client2.send_photo(config_.channel_id, thumb_path, img_cap);
                double thumb_secs = std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - thumb_start).count();
                bool photo_ok = photo_resp.ok;
                {
                    std::lock_guard<std::mutex> lk(log_mutex_);
                    if (photo_ok) {
                        std::cout << "[INFO] " << pfx << "Thumbnail sent: message ID "
                                  << photo_resp.message_id
                                  << " (" << fmt_elapsed(thumb_secs) << ")\n";
                    } else {
                        std::cerr << "\033[33m[WARNING]\033[0m " << pfx
                                  << "Thumbnail upload failed (video OK)"
                                  << " (" << fmt_elapsed(thumb_secs) << ")\n";
                    }
                }
                if (photo_ok) handle_post_upload(thumb_path);
            }

            return true;

        } else if (resp.retry_after > 0) {
            flood_wait_hits_++;
            flood_wait_total_secs_ += resp.retry_after;
            {
                std::lock_guard<std::mutex> lk(log_mutex_);
                std::cout << "\033[33m[WARNING]\033[0m " << pfx
                          << "FLOOD_WAIT " << resp.retry_after << "s, waiting...\n";
            }
            std::this_thread::sleep_for(std::chrono::seconds(resp.retry_after + 1));

        } else {
            if (!thumb_320.empty()) {
                try { fs::remove(thumb_320); } catch (...) {}
            }
            double send_secs = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - send_start).count();
            {
                std::lock_guard<std::mutex> lk(log_mutex_);
                std::cerr << "\033[31m[ERROR]\033[0m " << pfx << "Upload failed: " << fname
                          << " (" << fmt_elapsed(send_secs) << ")"
                          << (resp.error.empty() ? "" : " — " + resp.error) << "\n";
            }
            return false;
        }
    }

    if (!thumb_320.empty()) {
        try { fs::remove(thumb_320); } catch (...) {}
    }
    {
        std::lock_guard<std::mutex> lk(log_mutex_);
        std::cerr << "\033[31m[ERROR]\033[0m " << pfx << "Upload failed: " << fname
                  << " (max flood retries exceeded)\n";
    }
    return false;
}

void BotUploader::upload_directory(const std::string& dir_path, bool recursive) {
    if (!fs::is_directory(dir_path)) {
        std::cerr << "\033[31m[ERROR]\033[0m Directory not found: " << dir_path << "\n";
        return;
    }

    auto videos = scan_videos(dir_path, recursive);
    int total_files = static_cast<int>(videos.size());
    std::cout << "[INFO] Found " << total_files << " video files to upload\n";
    std::cout << "[INFO] Bot API: " << config_.api_url
              << "  Channel: " << config_.channel_id
              << "  Bots: " << config_.bots.size() << " (";
    for (size_t i = 0; i < config_.bots.size(); i++) {
        if (i) std::cout << ", ";
        std::cout << config_.bots[i].name;
    }
    std::cout << ")\n";

    if (videos.empty()) return;

    // Mode message — matches uploader.cpp
    if (settings_.delete_after_upload) {
        if (!settings_.exempt_folders.empty()) {
            std::cout << "\033[33m[WARNING]\033[0m Mode: delete after upload ("
                      << settings_.exempt_folders.size() << " exempt folders)\n";
        } else {
            std::cout << "\033[33m[WARNING]\033[0m Mode: delete after upload (no exemptions)\n";
        }
    } else if (settings_.mark_uploaded_files) {
        std::cout << "[INFO] Mode: mark uploaded with '"
                  << settings_.uploaded_suffix << "'\n";
    }

    int n_bots = static_cast<int>(config_.bots.size());

    if (n_bots > 1) {
        // Parallel mode: one thread per bot, shared work queue
        std::cout << "[INFO] Parallel mode: " << n_bots << " bots uploading simultaneously\n";

        std::sort(videos.begin(), videos.end());
        std::queue<std::string> work_queue;
        for (const auto& v : videos) work_queue.push(v);
        std::mutex queue_mutex;

        std::atomic<int> par_success{0}, par_skipped{0};
        std::vector<int> bot_uploads(n_bots, 0);  // thread i is the only writer of bot_uploads[i]

        auto total_start = std::chrono::steady_clock::now();

        std::vector<std::thread> threads;
        for (int i = 0; i < n_bots; i++) {
            threads.emplace_back([&, i]() {
                set_log_channel("bot-upload");
                while (true) {
                    if (g_cancel_botupload.load()) break;
                    std::string vp;
                    {
                        std::lock_guard<std::mutex> lk(queue_mutex);
                        if (work_queue.empty()) break;
                        vp = work_queue.front();
                        work_queue.pop();
                    }
                    if (upload_with_bot(i, vp)) {
                        handle_post_upload(vp);
                        par_success++;
                        bot_uploads[i]++;
                    } else {
                        par_skipped++;
                    }
                    // Brief pause before grabbing next file
                    {
                        std::lock_guard<std::mutex> lk(queue_mutex);
                        if (work_queue.empty()) break;
                    }
                    if (g_cancel_botupload.load()) break;
                    std::this_thread::sleep_for(std::chrono::seconds(3));
                }
            });
        }
        for (auto& t : threads) t.join();

        double total_secs = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - total_start).count();
        std::cout << "\n[INFO] Batch complete: " << par_success.load() << "/"
                  << total_files << " uploaded, " << par_skipped.load() << " skipped"
                  << " (total " << fmt_elapsed(total_secs);
        if (flood_wait_hits_.load() > 0) {
            std::cout << ", flood_wait " << flood_wait_hits_.load() << "x "
                      << fmt_elapsed(static_cast<double>(flood_wait_total_secs_.load()));
        }
        std::cout << ")\n";
        std::cout << "[INFO] Per-bot: ";
        for (int i = 0; i < n_bots; i++) {
            if (i) std::cout << ", ";
            std::cout << config_.bots[i].name << "=" << bot_uploads[i];
        }
        std::cout << "\n";
        return;
    }

    // Sequential mode (single bot): folder grouping with ordered processing
    std::map<std::string, std::vector<std::string>> folder_groups;
    for (const auto& vp : videos) {
        folder_groups[fs::path(vp).parent_path().string()].push_back(vp);
    }
    std::cout << "[INFO] Available folders: " << folder_groups.size() << "\n";

    int success = 0, skipped = 0;
    auto total_start = std::chrono::steady_clock::now();

    for (auto& [folder_path, folder_videos] : folder_groups) {
        if (g_cancel_botupload.load()) { std::cout << "[INFO] Canceled, stopping.\n"; break; }
        std::sort(folder_videos.begin(), folder_videos.end());
        auto folder_start = std::chrono::steady_clock::now();
        std::cout << "[INFO] Processing folder: " << folder_path
                  << " (" << folder_videos.size() << " files)\n";

        for (size_t i = 0; i < folder_videos.size(); i++) {
            if (g_cancel_botupload.load()) { std::cout << "[INFO] Canceled, stopping.\n"; break; }
            const auto& vp = folder_videos[i];

            if (upload_single(vp)) {
                handle_post_upload(vp);
                success++;
            } else {
                skipped++;
            }

            if (i + 1 < folder_videos.size()) {
                static std::mt19937 rng(std::random_device{}());
                std::uniform_int_distribution<int> dist(3, 9);
                int delay = dist(rng);
                std::cout << "[INFO] Waiting " << delay << "s...\n";
                std::this_thread::sleep_for(std::chrono::seconds(delay));
            }
        }

        double folder_secs = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - folder_start).count();
        std::cout << "[INFO] Folder done: " << folder_path
                  << " (" << fmt_elapsed(folder_secs) << ")\n";

        if (&folder_path != &folder_groups.rbegin()->first) {
            static std::mt19937 rng2(std::random_device{}());
            std::uniform_int_distribution<int> dist2(3, 9);
            int delay = dist2(rng2);
            std::cout << "[INFO] Waiting " << delay << "s...\n";
            std::this_thread::sleep_for(std::chrono::seconds(delay));
        }
    }

    double total_secs = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - total_start).count();
    std::cout << "\n[INFO] Batch complete: " << success << "/"
              << total_files << " uploaded, " << skipped << " skipped"
              << " (total " << fmt_elapsed(total_secs);
    if (flood_wait_hits_.load() > 0) {
        std::cout << ", flood_wait " << flood_wait_hits_.load() << "x "
                  << fmt_elapsed(static_cast<double>(flood_wait_total_secs_.load()));
    }
    std::cout << ")\n";
}
