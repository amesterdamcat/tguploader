#pragma once
#include "td_client.h"
#include "config.h"
#include "video_info.h"
#include <string>
#include <vector>

struct PreparedVideo {
    std::string video_path;
    std::string thumb_path;
    std::string caption;
    VideoInfo info;
    int64_t file_size = 0;
    int32_t pre_upload_id = -1;  // TDLib file_id after pre-upload
};

class Uploader {
public:
    Uploader(TdClient& client, const AccountConfig& account,
             const SharedSettings& settings, int64_t chat_id,
             int concurrent = 1);

    // Upload all videos in a directory
    void upload_directory(const std::string& dir_path, bool recursive);

private:
    TdClient& client_;
    const AccountConfig& account_;
    SharedSettings settings_;
    int64_t chat_id_;
    int concurrent_;

    // Scan for video files
    std::vector<std::string> scan_videos(const std::string& dir, bool recursive);

    // Prepare a video (metadata, caption, thumbnail) without uploading
    bool prepare_video(const std::string& video_path, PreparedVideo& out);

    // Upload a single video directly (concurrent=1 mode)
    bool upload_single(const std::string& video_path);

    // Upload a batch of videos with pre-upload + ordered send
    void upload_batch(std::vector<PreparedVideo>& batch);

    // Send one prepared video (using file_id or local path)
    bool send_prepared(PreparedVideo& pv);

    // Post-upload file handling (delete or mark)
    void handle_post_upload(const std::string& path);

    // Find same-name .jpg thumbnail
    std::string find_thumbnail(const std::string& video_path);

    // Check if file is in an exempt folder
    bool is_in_exempt_folder(const std::string& file_path);
};
