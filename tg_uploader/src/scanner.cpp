#include "scanner.h"
#include "config.h"
#include <iostream>
#include <regex>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <algorithm>

namespace fs = std::filesystem;

static const char* SCHEMA_SQL = R"(
CREATE TABLE IF NOT EXISTS videos (
    message_id    INTEGER PRIMARY KEY,
    url_id        INTEGER NOT NULL,
    file_name     TEXT NOT NULL,
    model_name    TEXT NOT NULL,
    platform      TEXT DEFAULT '',
    record_date   TEXT DEFAULT '',
    record_time   TEXT DEFAULT '',
    duration      INTEGER DEFAULT 0,
    width         INTEGER DEFAULT 0,
    height        INTEGER DEFAULT 0,
    file_size     INTEGER DEFAULT 0,
    thumb_path    TEXT DEFAULT '',
    caption_text  TEXT DEFAULT '',
    uploaded_at   INTEGER DEFAULT 0
);

CREATE INDEX IF NOT EXISTS idx_model ON videos(model_name COLLATE NOCASE);
CREATE INDEX IF NOT EXISTS idx_date ON videos(record_date);
CREATE INDEX IF NOT EXISTS idx_platform ON videos(platform);

CREATE VIRTUAL TABLE IF NOT EXISTS videos_fts USING fts5(
    model_name, file_name, content=videos, content_rowid=message_id
);

CREATE TRIGGER IF NOT EXISTS videos_ai AFTER INSERT ON videos BEGIN
    INSERT INTO videos_fts(rowid, model_name, file_name)
    VALUES (new.message_id, new.model_name, new.file_name);
END;

CREATE TABLE IF NOT EXISTS scan_checkpoint (
    id INTEGER PRIMARY KEY CHECK(id = 1),
    oldest_message_id INTEGER DEFAULT 0,
    total_scanned     INTEGER DEFAULT 0,
    scan_complete     INTEGER DEFAULT 0
);
INSERT OR IGNORE INTO scan_checkpoint(id) VALUES (1);
)";

static const std::vector<std::string> platform_keywords = {
    "Chaturbate", "StripChat", "OnlyFans", "ManyVids",
    "Cam4", "Streamate", "LiveJasmin"
};

static bool same_channel_id(const std::string& a, const std::string& b) {
    return !a.empty() && !b.empty() && a == b;
}

static std::vector<std::string> split_string(const std::string& s, char delim) {
    std::vector<std::string> parts;
    std::string token;
    for (char c : s) {
        if (c == delim) {
            parts.push_back(token);
            token.clear();
        } else {
            token += c;
        }
    }
    parts.push_back(token);
    return parts;
}

// ---------- ChannelScanner ----------

ChannelScanner::ChannelScanner(TdClient& client, const std::string& db_path,
                               const std::string& thumb_dir)
    : client_(client), db_path_(db_path), thumb_dir_(thumb_dir) {
    fs::create_directories(thumb_dir_);
}

ChannelScanner::~ChannelScanner() {
    close_db();
}

void ChannelScanner::init_db() {
    int rc = sqlite3_open(db_path_.c_str(), &db_);
    if (rc != SQLITE_OK) {
        std::cerr << "\033[34m[SCAN]\033[0m Failed to open DB: " << sqlite3_errmsg(db_) << "\n";
        db_ = nullptr;
        return;
    }
    // WAL mode for better concurrent access
    sqlite3_exec(db_, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);

    char* err = nullptr;
    rc = sqlite3_exec(db_, SCHEMA_SQL, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        std::cerr << "\033[34m[SCAN]\033[0m Schema error: " << (err ? err : "unknown") << "\n";
        sqlite3_free(err);
    }
}

void ChannelScanner::close_db() {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

// Shared filename parser: extract model, platform, date, time from filename stem.
// Used by both ChannelScanner::parse_filename and record_uploaded_video.
static void parse_filename_impl(const std::string& stem, std::string& model,
                                 std::string& platform, std::string& date,
                                 std::string& time) {
    auto parts = split_string(stem, '_');
    if (parts.empty()) {
        model = stem;
        return;
    }

    // Find platform keyword (case-insensitive)
    int platform_index = -1;
    for (int i = 0; i < static_cast<int>(parts.size()); i++) {
        std::string lower_part = parts[i];
        std::transform(lower_part.begin(), lower_part.end(), lower_part.begin(), ::tolower);
        for (const auto& kw : platform_keywords) {
            std::string lower_kw = kw;
            std::transform(lower_kw.begin(), lower_kw.end(), lower_kw.begin(), ::tolower);
            if (lower_part == lower_kw) {
                platform_index = i;
                platform = kw; // store canonical name
                break;
            }
        }
        if (platform_index >= 0) break;
    }

    std::vector<std::string> remaining_parts;

    if (platform_index > 0) {
        // Join parts before platform keyword as model name
        model.clear();
        for (int i = 0; i < platform_index; i++) {
            if (i > 0) model += '_';
            model += parts[i];
        }
        for (int i = platform_index + 1; i < static_cast<int>(parts.size()); i++) {
            remaining_parts.push_back(parts[i]);
        }
    } else if (platform_index == 0) {
        // Platform is first part, model is second
        if (parts.size() > 1) {
            model = parts[1];
            for (int i = 2; i < static_cast<int>(parts.size()); i++) {
                remaining_parts.push_back(parts[i]);
            }
        }
    } else {
        // No platform found - scan for stop markers: "part", date pattern, or YYYY-MM-DD
        std::regex date8_re(R"(\d{8})");
        std::regex date_dash_re(R"(\d{4}-\d{2}-\d{2})");
        int name_end = static_cast<int>(parts.size());
        for (int i = 0; i < static_cast<int>(parts.size()); i++) {
            std::string lower_part = parts[i];
            std::transform(lower_part.begin(), lower_part.end(), lower_part.begin(), ::tolower);
            if (lower_part == "part") { name_end = i; break; }
            if (std::regex_search(parts[i], date8_re)) { name_end = i; break; }
            if (std::regex_search(parts[i], date_dash_re)) { name_end = i; break; }
        }
        if (name_end == 0) name_end = 1; // at least first part
        model.clear();
        for (int i = 0; i < name_end; i++) {
            if (i > 0) model += '_';
            model += parts[i];
        }
        for (int i = name_end; i < static_cast<int>(parts.size()); i++) {
            remaining_parts.push_back(parts[i]);
        }
    }

    // Default platform
    if (platform.empty()) platform = "Chaturbate";

    // Search for date-time pattern: YYYYMMDD-HHMMSS or YYYYMMDD or YYYY-MM-DD_HH-MM-SS
    std::regex datetime_re(R"((\d{4})(\d{2})(\d{2})-(\d{2})(\d{2})(\d{2}))");
    std::regex date_re(R"((\d{4})(\d{2})(\d{2}))");
    std::regex datetime_dash_re(R"((\d{4})-(\d{2})-(\d{2}))");
    std::regex time_dash_re(R"((\d{2})-(\d{2})-(\d{2}))");

    for (size_t ri = 0; ri < remaining_parts.size(); ri++) {
        const auto& part = remaining_parts[ri];
        std::smatch m;
        if (std::regex_search(part, m, datetime_re)) {
            date = m[1].str() + "-" + m[2].str() + "-" + m[3].str();
            time = m[4].str() + ":" + m[5].str() + ":" + m[6].str();
            return;
        }
        if (std::regex_search(part, m, datetime_dash_re)) {
            date = m[1].str() + "-" + m[2].str() + "-" + m[3].str();
            // Next part might be time: HH-MM-SS
            if (ri + 1 < remaining_parts.size()) {
                std::smatch tm;
                if (std::regex_match(remaining_parts[ri + 1], tm, time_dash_re)) {
                    time = tm[1].str() + ":" + tm[2].str() + ":" + tm[3].str();
                }
            }
            return;
        }
        if (std::regex_search(part, m, date_re)) {
            date = m[1].str() + "-" + m[2].str() + "-" + m[3].str();
            return;
        }
    }
}

void ChannelScanner::parse_filename(const std::string& stem, std::string& model,
                                     std::string& platform, std::string& date,
                                     std::string& time) {
    parse_filename_impl(stem, model, platform, date, time);
}

std::string ChannelScanner::download_thumb(int32_t file_id, int64_t url_id) {
    if (file_id <= 0) return "";

    std::string dest = thumb_dir_ + "/" + std::to_string(url_id) + ".jpg";

    // Skip if already downloaded
    if (fs::exists(dest) && fs::file_size(dest) > 0) {
        return "thumbs/" + std::to_string(url_id) + ".jpg";
    }

    std::string local_path = client_.download_file_sync(file_id);
    if (local_path.empty()) return "";

    try {
        fs::copy_file(local_path, dest, fs::copy_options::overwrite_existing);
        fs::permissions(dest, fs::perms::owner_read | fs::perms::owner_write |
                              fs::perms::group_read | fs::perms::others_read);
        return "thumbs/" + std::to_string(url_id) + ".jpg";
    } catch (const std::exception& e) {
        std::cerr << "\033[34m[SCAN]\033[0m Thumb copy failed: " << e.what() << "\n";
        return "";
    }
}

// Try to find a photo message near the video with matching filename stem.
// Searches up to 20 messages ahead of the video for a photo whose caption
// contains the video's filename stem.
// Returns thumb relative path, or empty string if not found.
std::string ChannelScanner::download_photo_thumb(int64_t chat_id, int64_t video_msg_id,
                                                  const std::string& video_stem, int64_t url_id,
                                                  bool force, int range) {
    std::string dest = thumb_dir_ + "/" + std::to_string(url_id) + ".jpg";

    // Skip if already downloaded (unless force=true for cover upgrade)
    if (!force && fs::exists(dest) && fs::file_size(dest) > 0) {
        return "thumbs/" + std::to_string(url_id) + ".jpg";
    }

    // Search photo messages within ±30 messages of this video
    static constexpr int64_t MSG_ID_STEP = 1 << 20;

    // Helper lambda: scan a batch of photo messages for matching caption
    auto try_find_photo = [&](int64_t from_id, int32_t limit,
                              int64_t min_id, int64_t max_id) -> std::string {
        auto result = client_.search_chat_messages(
            chat_id, from_id, limit,
            td_api::make_object<td_api::searchMessagesFilterPhoto>());
        if (!result) return "";

        for (auto& msg : result->messages_) {
            if (!msg || !msg->content_) continue;
            if (msg->id_ < min_id || msg->id_ > max_id) continue;
            if (msg->content_->get_id() != td_api::messagePhoto::ID) continue;

            auto& photo_msg = static_cast<td_api::messagePhoto&>(*msg->content_);
            if (!photo_msg.caption_ || photo_msg.caption_->text_.empty()) continue;
            if (photo_msg.caption_->text_.find(video_stem) == std::string::npos) continue;

            if (!photo_msg.photo_ || photo_msg.photo_->sizes_.empty()) continue;
            auto& largest = photo_msg.photo_->sizes_.back();
            if (!largest || !largest->photo_) continue;

            std::string local_path = client_.download_file_sync(largest->photo_->id_);
            if (local_path.empty()) continue;

            try {
                fs::copy_file(local_path, dest, fs::copy_options::overwrite_existing);
                fs::permissions(dest, fs::perms::owner_read | fs::perms::owner_write |
                                      fs::perms::group_read | fs::perms::others_read);
                return "thumbs/" + std::to_string(url_id) + ".jpg";
            } catch (const std::exception& e) {
                std::cerr << "\033[34m[SCAN]\033[0m Photo thumb copy failed: " << e.what() << "\n";
            }
        }
        return "";
    };

    // Search after video (newer messages): from +(range+1) msgs ahead, look back range
    std::string found = try_find_photo(
        video_msg_id + MSG_ID_STEP * (range + 1), range,
        video_msg_id + 1, INT64_MAX);
    if (!found.empty()) return found;

    // Search before video (older messages): from video itself, look back range
    found = try_find_photo(
        video_msg_id, range,
        video_msg_id - MSG_ID_STEP * (range + 1), video_msg_id - 1);
    return found;
}

bool ChannelScanner::record_exists(int64_t message_id) {
    const char* sql = "SELECT 1 FROM videos WHERE message_id = ?";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(stmt, 1, message_id);
    bool exists = (sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_finalize(stmt);
    return exists;
}

bool ChannelScanner::insert_record(const VideoRecord& rec) {
    const char* sql =
        "INSERT OR IGNORE INTO videos "
        "(message_id, url_id, file_name, model_name, platform, record_date, record_time, "
        " duration, width, height, file_size, thumb_path, caption_text, uploaded_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "\033[34m[SCAN]\033[0m INSERT prepare error: " << sqlite3_errmsg(db_) << "\n";
        return false;
    }

    sqlite3_bind_int64(stmt, 1, rec.message_id);
    sqlite3_bind_int64(stmt, 2, rec.url_id);
    sqlite3_bind_text(stmt, 3, rec.file_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, rec.model_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, rec.platform.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, rec.record_date.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, rec.record_time.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 8, rec.duration);
    sqlite3_bind_int(stmt, 9, rec.width);
    sqlite3_bind_int(stmt, 10, rec.height);
    sqlite3_bind_int64(stmt, 11, rec.file_size);
    sqlite3_bind_text(stmt, 12, rec.thumb_path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 13, rec.caption_text.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 14, rec.uploaded_at);

    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok;
}

int64_t ChannelScanner::get_checkpoint_oldest_id() {
    const char* sql = "SELECT oldest_message_id FROM scan_checkpoint WHERE id = 1";
    sqlite3_stmt* stmt;
    int64_t val = 0;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            val = sqlite3_column_int64(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
    return val;
}

int ChannelScanner::get_checkpoint_total() {
    const char* sql = "SELECT total_scanned FROM scan_checkpoint WHERE id = 1";
    sqlite3_stmt* stmt;
    int val = 0;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            val = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
    return val;
}

bool ChannelScanner::is_scan_complete() {
    const char* sql = "SELECT scan_complete FROM scan_checkpoint WHERE id = 1";
    sqlite3_stmt* stmt;
    bool val = false;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            val = sqlite3_column_int(stmt, 0) != 0;
        }
        sqlite3_finalize(stmt);
    }
    return val;
}

void ChannelScanner::update_checkpoint(int64_t oldest_id, int total, bool complete) {
    const char* sql =
        "UPDATE scan_checkpoint SET oldest_message_id = ?, total_scanned = ?, scan_complete = ? WHERE id = 1";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, oldest_id);
        sqlite3_bind_int(stmt, 2, total);
        sqlite3_bind_int(stmt, 3, complete ? 1 : 0);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

void ChannelScanner::reset_checkpoint() {
    update_checkpoint(0, 0, false);
}

int ChannelScanner::run(int64_t chat_id, bool resume, bool full, bool fetch_thumbnails) {
    init_db();
    if (!db_) {
        std::cerr << "\033[34m[SCAN]\033[0m Cannot open database, aborting.\n";
        return 0;
    }

    int64_t from_message_id = 0;
    int total_scanned = 0;
    bool incremental = (!resume && !full);

    if (full && !resume) {
        // Full scan from newest
        std::cout << "\033[34m[SCAN]\033[0m Starting full scan from newest messages...\n";
        reset_checkpoint();
        from_message_id = 0; // 0 = start from newest
    } else if (resume) {
        // Resume from checkpoint
        from_message_id = get_checkpoint_oldest_id();
        total_scanned = get_checkpoint_total();
        if (from_message_id == 0) {
            std::cout << "\033[34m[SCAN]\033[0m No checkpoint found, starting from newest.\n";
        } else {
            std::cout << "\033[34m[SCAN]\033[0m Resuming from message_id " << from_message_id
                      << " (" << total_scanned << " previously scanned)\n";
        }
    } else {
        // Incremental: scan from newest, stop when hitting existing record
        std::cout << "\033[34m[SCAN]\033[0m Incremental scan (new messages only)...\n";
        from_message_id = 0;
    }

    int new_records = 0;
    int pages = 0;
    bool done = false;

    while (!done) {
        if (pages == 0) {
            std::cout << "\033[34m[SCAN]\033[0m Fetching messages...\n";
        }
        auto result = client_.search_chat_messages(
            chat_id, from_message_id, 100,
            td_api::make_object<td_api::searchMessagesFilterVideo>());

        if (!result) {
            std::cerr << "\033[34m[SCAN]\033[0m search_chat_messages returned null, stopping.\n";
            break;
        }

        auto& messages = result->messages_;
        if (messages.empty()) {
            std::cout << "\033[34m[SCAN]\033[0m No more messages, scan complete.\n";
            done = true;
            break;
        }

        pages++;
        int64_t oldest_in_page = 0;

        // Use a transaction per page for performance
        sqlite3_exec(db_, "BEGIN TRANSACTION", nullptr, nullptr, nullptr);

        for (auto& msg : messages) {
            if (!msg) continue;

            total_scanned++;
            int64_t msg_id = msg->id_;

            if (oldest_in_page == 0 || msg_id < oldest_in_page) {
                oldest_in_page = msg_id;
            }

            if (total_scanned % 10000 == 0) {
                std::cout << "\033[34m[SCAN]\033[0m Progress: "
                          << total_scanned << " videos scanned, "
                          << new_records << " new, " << pages << " pages...\n";
            }

            // Incremental mode: stop if we already have this record
            if (incremental && record_exists(msg_id)) {
                std::cout << "\033[34m[SCAN]\033[0m Hit existing record at message_id " << msg_id
                          << ", stopping incremental scan.\n";
                done = true;
                break;
            }

            // Skip if already in DB (for full/resume modes)
            if (record_exists(msg_id)) continue;

            // Extract video content
            if (!msg->content_ || msg->content_->get_id() != td_api::messageVideo::ID) {
                continue;
            }

            auto& video_msg = static_cast<td_api::messageVideo&>(*msg->content_);
            if (!video_msg.video_) continue;

            auto& video = *video_msg.video_;

            VideoRecord rec;
            rec.message_id = msg_id;
            rec.url_id = msg_id >> 20;
            rec.file_name = video.file_name_;
            rec.duration = video.duration_;
            rec.width = video.width_;
            rec.height = video.height_;
            rec.uploaded_at = msg->date_;

            if (video.video_) {
                rec.file_size = video.video_->expected_size_;
                if (rec.file_size <= 0) rec.file_size = video.video_->size_;
            }

            // Extract caption text
            if (msg->content_->get_id() == td_api::messageVideo::ID) {
                auto& cap = video_msg.caption_;
                if (cap) {
                    rec.caption_text = cap->text_;
                }
            }

            // Parse filename for metadata
            std::string stem = fs::path(rec.file_name).stem().string();
            parse_filename(stem, rec.model_name, rec.platform, rec.record_date, rec.record_time);

            if (fetch_thumbnails) {
                // Download thumbnail: prefer the uploaded JPG photo after the video
                rec.thumb_path = download_photo_thumb(chat_id, msg_id, stem, rec.url_id);
                // Fallback to video's built-in thumbnail
                if (rec.thumb_path.empty() && video.thumbnail_ && video.thumbnail_->file_) {
                    rec.thumb_path = download_thumb(video.thumbnail_->file_->id_, rec.url_id);
                }
            }

            if (insert_record(rec)) {
                new_records++;
            }
        }

        sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr);

        // Update checkpoint
        if (oldest_in_page > 0) {
            update_checkpoint(oldest_in_page, total_scanned, done);
        }

        // Update from_message_id for next page
        if (result->next_from_message_id_ == 0 || messages.empty()) {
            std::cout << "\033[34m[SCAN]\033[0m Reached end of channel history.\n";
            done = true;
        } else {
            from_message_id = result->next_from_message_id_;
        }

        std::cout << "\033[34m[SCAN]\033[0m Progress: "
                  << total_scanned << " videos scanned, "
                  << new_records << " new, " << pages << " pages...\n";
    }

    // Mark scan as complete
    if (done) {
        update_checkpoint(get_checkpoint_oldest_id(), total_scanned, true);
    }

    std::cout << "\033[34m[SCAN]\033[0m Done. " << total_scanned
              << " total scanned, " << new_records
              << " new records inserted.\n";

    close_db();
    return new_records;
}

int ChannelScanner::fix_thumbs(int64_t chat_id, bool force, int range) {
    init_db();
    if (!db_) {
        std::cerr << "\033[35m[FIX-THUMB]\033[0m Cannot open database.\n";
        return 0;
    }

    // force: re-download all; otherwise only missing
    const char* sql = force
        ? "SELECT message_id, url_id, file_name FROM videos"
        : "SELECT message_id, url_id, file_name FROM videos WHERE thumb_path = ''";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "\033[35m[FIX-THUMB]\033[0m Query failed.\n";
        close_db();
        return 0;
    }

    struct MissingThumb { int64_t message_id; int64_t url_id; std::string file_name; };
    std::vector<MissingThumb> missing;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        missing.push_back({
            sqlite3_column_int64(stmt, 0),
            sqlite3_column_int64(stmt, 1),
            text ? text : ""
        });
    }
    sqlite3_finalize(stmt);

    // In force mode, categorize existing thumbnails
    // - photo (>= 100KB): skip, already best quality
    // - cover (< 100KB): try upgrade to photo, keep cover if upgrade fails
    // - missing: full download (photo → cover fallback)
    struct ThumbTask { int64_t message_id; int64_t url_id; std::string file_name; bool has_cover; };
    std::vector<ThumbTask> tasks;

    if (force) {
        size_t before = missing.size();
        size_t exist_photo = 0, exist_cover = 0, no_file = 0;
        for (auto& m : missing) {
            std::string existing = thumb_dir_ + "/" + std::to_string(m.url_id) + ".jpg";
            if (fs::exists(existing) && fs::file_size(existing) > 0) {
                if (fs::file_size(existing) >= 100000) {
                    exist_photo++;
                    continue; // Already high-quality, skip
                }
                exist_cover++;
                tasks.push_back({m.message_id, m.url_id, std::move(m.file_name), true});
            } else {
                no_file++;
                tasks.push_back({m.message_id, m.url_id, std::move(m.file_name), false});
            }
        }
        std::printf("\033[35m[FIX-THUMB]\033[0m %zu total: \033[32m%zu photo(skip)\033[0m, \033[33m%zu cover(upgrade)\033[0m, \033[31m%zu missing\033[0m\n",
                    before, exist_photo, exist_cover, no_file);
    } else {
        for (auto& m : missing) {
            tasks.push_back({m.message_id, m.url_id, std::move(m.file_name), false});
        }
    }

    if (tasks.empty()) {
        std::cout << "\033[35m[FIX-THUMB]\033[0m Nothing to fix.\n";
        close_db();
        return 0;
    }

    std::printf("\033[35m[FIX-THUMB]\033[0m Processing %zu records (range=±%d)...\n", tasks.size(), range);

    int got_photo = 0;    // Downloaded or upgraded to photo
    int got_cover = 0;    // Downloaded video cover (missing → cover)
    int kept_cover = 0;   // Cover upgrade failed, kept existing
    int failed = 0;       // Missing and got nothing
    for (size_t i = 0; i < tasks.size(); i++) {
        auto& t = tasks[i];
        std::string stem = fs::path(t.file_name).stem().string();

        // Try photo download (force=true for cover upgrade to bypass existing file cache)
        std::string thumb = download_photo_thumb(chat_id, t.message_id, stem, t.url_id, t.has_cover, range);

        if (!thumb.empty()) {
            // Verify: check actual file size to confirm it's a real photo (>= 100KB)
            std::string dest = thumb_dir_ + "/" + std::to_string(t.url_id) + ".jpg";
            bool is_real_photo = fs::exists(dest) && fs::file_size(dest) >= 100000;

            if (t.has_cover && !is_real_photo) {
                // "Photo" found but file still small — not a real upgrade, keep cover
                kept_cover++;
            } else {
                const char* update_sql = "UPDATE videos SET thumb_path = ? WHERE message_id = ?";
                sqlite3_stmt* update_stmt;
                if (sqlite3_prepare_v2(db_, update_sql, -1, &update_stmt, nullptr) == SQLITE_OK) {
                    sqlite3_bind_text(update_stmt, 1, thumb.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_int64(update_stmt, 2, t.message_id);
                    sqlite3_step(update_stmt);
                    sqlite3_finalize(update_stmt);
                }
                got_photo++;
            }
        } else if (t.has_cover) {
            // Had cover, photo upgrade failed — keep existing cover as-is
            kept_cover++;
        } else {
            // Missing file, no photo — fallback to video's built-in thumbnail
            auto video_msg = client_.get_message(chat_id, t.message_id);
            if (video_msg && video_msg->content_ &&
                video_msg->content_->get_id() == td_api::messageVideo::ID) {
                auto& vc = static_cast<td_api::messageVideo&>(*video_msg->content_);
                if (vc.video_ && vc.video_->thumbnail_ && vc.video_->thumbnail_->file_) {
                    thumb = download_thumb(vc.video_->thumbnail_->file_->id_, t.url_id);
                }
            }
            if (!thumb.empty()) {
                const char* update_sql = "UPDATE videos SET thumb_path = ? WHERE message_id = ?";
                sqlite3_stmt* update_stmt;
                if (sqlite3_prepare_v2(db_, update_sql, -1, &update_stmt, nullptr) == SQLITE_OK) {
                    sqlite3_bind_text(update_stmt, 1, thumb.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_int64(update_stmt, 2, t.message_id);
                    sqlite3_step(update_stmt);
                    sqlite3_finalize(update_stmt);
                }
                got_cover++;
            } else {
                failed++;
            }
        }

        std::printf("\r\033[35m[FIX-THUMB]\033[0m %zu/%zu  \033[32m%d photo\033[0m \033[33m%d cover\033[0m \033[36m%d kept\033[0m \033[31m%d fail\033[0m   ",
                    i + 1, tasks.size(), got_photo, got_cover, kept_cover, failed);
        std::fflush(stdout);
    }

    std::printf("\n\033[35m[FIX-THUMB]\033[0m Done. \033[32m%d photo\033[0m + \033[33m%d cover\033[0m + \033[36m%d kept\033[0m, \033[31m%d failed\033[0m\n",
                got_photo, got_cover, kept_cover, failed);
    close_db();
    return got_photo + got_cover;
}

// --- Standalone: record upload into scanner.db ---

void record_uploaded_video(const std::string& db_path, const std::string& thumb_dir,
                           int64_t message_id, const std::string& file_name,
                           int duration, int width, int height, int64_t file_size,
                           const std::string& caption_text, const std::string& thumb_src_path,
                           int64_t url_id_override) {
    sqlite3* db = nullptr;
    if (sqlite3_open(db_path.c_str(), &db) != SQLITE_OK) {
        std::cerr << "\033[36m[DB]\033[0m Failed to open scanner.db for recording upload\n";
        return;
    }

    // Wait up to 10s if another thread is writing (parallel bot uploads)
    sqlite3_busy_timeout(db, 10000);

    // Ensure schema exists (handles fresh install where scan hasn't run yet)
    sqlite3_exec(db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(db, SCHEMA_SQL, nullptr, nullptr, nullptr);

    int64_t url_id = (url_id_override >= 0) ? url_id_override : (message_id >> 20);

    // Parse filename
    std::string stem = fs::path(file_name).stem().string();
    std::string model, platform, date, time;
    parse_filename_impl(stem, model, platform, date, time);

    // Copy thumbnail
    std::string thumb_path;
    if (!thumb_src_path.empty() && fs::exists(thumb_src_path)) {
        fs::create_directories(thumb_dir);
        std::string dest = thumb_dir + "/" + std::to_string(url_id) + ".jpg";
        try {
            fs::copy_file(thumb_src_path, dest, fs::copy_options::overwrite_existing);
            fs::permissions(dest, fs::perms::owner_read | fs::perms::owner_write |
                                  fs::perms::group_read | fs::perms::others_read);
            thumb_path = "thumbs/" + std::to_string(url_id) + ".jpg";
        } catch (const std::exception& e) {
            std::cerr << "\033[36m[DB]\033[0m Thumb copy failed: " << e.what() << "\n";
        }
    }

    int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    const char* sql =
        "INSERT OR IGNORE INTO videos "
        "(message_id, url_id, file_name, model_name, platform, record_date, record_time, "
        " duration, width, height, file_size, thumb_path, caption_text, uploaded_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, message_id);
        sqlite3_bind_int64(stmt, 2, url_id);
        sqlite3_bind_text(stmt, 3, file_name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, model.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 5, platform.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 6, date.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 7, time.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 8, duration);
        sqlite3_bind_int(stmt, 9, width);
        sqlite3_bind_int(stmt, 10, height);
        sqlite3_bind_int64(stmt, 11, file_size);
        sqlite3_bind_text(stmt, 12, thumb_path.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 13, caption_text.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 14, now);
        int rc = sqlite3_step(stmt);
        if (rc == SQLITE_DONE) {
            int changes = sqlite3_changes(db);
            if (changes > 0) {
                std::cout << "\n\033[36m[DB]\033[0m Recorded to scanner.db: " << file_name
                          << " (url_id=" << url_id
                          << ", thumb=" << (thumb_path.empty() ? "none" : "ok") << ")\n";
            } else {
                std::cout << "\n\033[36m[DB]\033[0m Already exists, skipped: " << file_name
                          << " (url_id=" << url_id << ")\n";
            }
        } else {
            std::cerr << "\n\033[36m[DB]\033[0m INSERT failed: " << sqlite3_errmsg(db) << "\n";
        }
        sqlite3_finalize(stmt);
    } else {
        std::cerr << "\n\033[36m[DB]\033[0m INSERT prepare error: " << sqlite3_errmsg(db) << "\n";
    }

    sqlite3_close(db);
}

ChannelScanResult scan_account_channel(const std::string& account_name,
                                       bool resume,
                                       bool full,
                                       bool fetch_thumbnails) {
    ChannelScanResult out;
    out.account_name = account_name;
    try {
        auto config = load_account(account_name);
        out.account_name = config.name;
        std::cout << "[" << config.name << "] Connecting for scan...\n";

        TdClient client(config);
        if (!client.login()) {
            out.error = "not authorized for account '" + config.name + "'";
            std::cerr << "Error: " << out.error << ". Run login first.\n";
            return out;
        }

        std::cout << "[" << config.name << "] Authorized. Finding channel: "
                  << config.channel_id << "\n";

        int64_t chat_id = client.find_channel(config.channel_id);
        if (chat_id == 0) {
            out.error = "channel not found: " + config.channel_id;
            std::cerr << "Error: " << out.error << "\n";
            client.close();
            return out;
        }

        std::string base = get_base_dir();
        std::string db_path = base + "/data/scanner.db";
        std::string thumb_dir = base + "/data/thumbs";
        fs::create_directories(thumb_dir);

        ChannelScanner scanner(client, db_path, thumb_dir);
        out.inserted = scanner.run(chat_id, resume, full, fetch_thumbnails);
        out.ok = true;
        std::cout << "\033[34m[SCAN]\033[0m Inserted " << out.inserted
                  << " new records.\n";
        client.close();
    } catch (const std::exception& e) {
        out.error = e.what();
        std::cerr << "\033[34m[SCAN]\033[0m Failed: " << out.error << "\n";
    }
    return out;
}

ChannelScanResult scan_source_channel_for_backup(const std::string& source_channel_id,
                                                 bool full,
                                                 bool fetch_thumbnails) {
    auto accounts = list_accounts();
    if (accounts.empty()) {
        ChannelScanResult out;
        out.error = "no TDLib accounts configured for pre-backup scan";
        return out;
    }

    for (const auto& acc : accounts) {
        if (same_channel_id(acc.channel_id, source_channel_id)) {
            return scan_account_channel(acc.name, false, full, fetch_thumbnails);
        }
    }

    if (accounts.size() == 1) {
        std::cout << "\033[33m[SCAN]\033[0m Source channel '" << source_channel_id
                  << "' does not exactly match account channel '" << accounts[0].channel_id
                  << "'; using the only configured account: " << accounts[0].name << "\n";
        return scan_account_channel(accounts[0].name, false, full, fetch_thumbnails);
    }

    ChannelScanResult out;
    out.error = "cannot choose scan account for source channel '" + source_channel_id +
                "'; set the matching channel_id in .account_configs";
    return out;
}
