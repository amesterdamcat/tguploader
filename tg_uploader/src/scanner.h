#pragma once
#include "td_client.h"
#include <sqlite3.h>
#include <string>
#include <cstdint>

struct VideoRecord {
    int64_t message_id = 0;
    int64_t url_id = 0;
    std::string file_name;
    std::string model_name;
    std::string platform;
    std::string record_date;   // YYYY-MM-DD
    std::string record_time;   // HH:MM:SS
    int duration = 0;
    int width = 0;
    int height = 0;
    int64_t file_size = 0;
    std::string thumb_path;
    std::string caption_text;
    int64_t uploaded_at = 0;   // message.date_ unix timestamp
};

// Standalone: record a just-uploaded video into scanner.db (no TdClient needed)
// thumb_src_path: path to original full-res JPG (will be copied to thumb_dir/{url_id}.jpg)
// url_id_override: when >= 0, use directly as url_id (Bot API); when -1, derive via message_id>>20 (TDLib)
void record_uploaded_video(const std::string& db_path, const std::string& thumb_dir,
                           int64_t message_id, const std::string& file_name,
                           int duration, int width, int height, int64_t file_size,
                           const std::string& caption_text, const std::string& thumb_src_path,
                           int64_t url_id_override = -1);

class ChannelScanner {
public:
    ChannelScanner(TdClient& client, const std::string& db_path, const std::string& thumb_dir);
    ~ChannelScanner();

    // Run scan: returns count of new records inserted
    // resume=true: continue from checkpoint; full=true: full scan from newest
    int run(int64_t chat_id, bool resume, bool full);

    // Re-download thumbnails. force=true: re-download ALL (replace video covers with photos)
    // range: search ±N messages around each video for matching photo
    int fix_thumbs(int64_t chat_id, bool force = false, int range = 30);

private:
    TdClient& client_;
    std::string db_path_;
    std::string thumb_dir_;
    sqlite3* db_ = nullptr;

    void init_db();
    void close_db();

    // Parse filename stem → model, platform, date, time
    void parse_filename(const std::string& stem, std::string& model,
                        std::string& platform, std::string& date, std::string& time);

    // Download thumbnail → thumbs/{url_id}.jpg, returns relative path
    std::string download_thumb(int32_t file_id, int64_t url_id);

    // Try to download the photo message right after a video (same filename stem)
    // Falls back to empty string if no matching photo found
    std::string download_photo_thumb(int64_t chat_id, int64_t video_msg_id,
                                      const std::string& video_stem, int64_t url_id,
                                      bool force = false, int range = 30);

    // Check if message_id already exists in DB
    bool record_exists(int64_t message_id);

    // Insert a video record
    bool insert_record(const VideoRecord& rec);

    // Read/write checkpoint
    int64_t get_checkpoint_oldest_id();
    int get_checkpoint_total();
    bool is_scan_complete();
    void update_checkpoint(int64_t oldest_id, int total, bool complete);
    void reset_checkpoint();
};
