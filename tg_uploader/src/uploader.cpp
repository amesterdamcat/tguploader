#include "uploader.h"
#include "caption.h"
#include "folder_lock.h"
#include "scanner.h"
#include "utils.h"
#include "video_info.h"
#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <map>
#include <random>
#include <set>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;
using SteadyClock = std::chrono::steady_clock;

// Defined in main.cpp — signal handler uses this to clean up on SIGINT/SIGTERM
extern std::string g_locked_folder;


// TDLib free account limit: 4000 parts × 512KB = 2,097,152,000 bytes (~1.953 GiB)
// Premium: 8000 parts × 512KB = 4,194,304,000 bytes (~3.906 GiB)
// Use free limit conservatively; files over this get renamed to .big
static constexpr int64_t BIG_THRESHOLD = 4000LL * 512 * 1024; // ~1.953 GiB

Uploader::Uploader(TdClient& client, const AccountConfig& account,
                   const SharedSettings& settings, int64_t chat_id,
                   int concurrent)
    : client_(client), account_(account), settings_(settings),
      chat_id_(chat_id), concurrent_(concurrent) {}

std::vector<std::string> Uploader::scan_videos(const std::string& dir, bool recursive) {
    std::vector<std::string> videos;

    auto is_available = [](const std::string& path) {
        if (path.size() >= 9 && path.substr(path.size() - 9) == ".uploaded")
            return false;
        if (path.size() >= 4 && path.substr(path.size() - 4) == ".big")
            return false;
        struct stat st;
        if (::stat((path + ".uploaded").c_str(), &st) == 0)
            return false;
        if (::stat((path + ".big").c_str(), &st) == 0)
            return false;
        return true;
    };

    try {
        if (recursive) {
            for (auto& entry : fs::recursive_directory_iterator(
                     dir, fs::directory_options::skip_permission_denied)) {
                if (entry.is_regular_file()) {
                    std::string path = entry.path().string();
                    if (is_video_file(path) && is_available(path)) {
                        videos.push_back(path);
                    }
                }
            }
        } else {
            for (auto& entry : fs::directory_iterator(dir)) {
                if (entry.is_regular_file()) {
                    std::string path = entry.path().string();
                    if (is_video_file(path) && is_available(path)) {
                        videos.push_back(path);
                    }
                }
            }
        }
    } catch (const fs::filesystem_error& e) {
        std::cerr << "\033[31m[ERROR]\033[0m Directory scan failed: " << e.what() << "\n";
    }

    return videos;
}

std::string Uploader::find_thumbnail(const std::string& video_path) {
    fs::path p(video_path);
    fs::path thumb = p.parent_path() / (p.stem().string() + ".jpg");
    if (fs::exists(thumb)) {
        return thumb.string();
    }
    return "";
}

bool Uploader::is_in_exempt_folder(const std::string& file_path) {
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

void Uploader::handle_post_upload(const std::string& path) {
    if (settings_.delete_after_upload) {
        if (is_in_exempt_folder(path)) {
            if (settings_.mark_uploaded_files) {
                try {
                    fs::rename(path, path + settings_.uploaded_suffix);
                    std::cout << "[INFO] Marked uploaded: "
                              << fs::path(path).filename().string() << "\n";
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
            std::cout << "[INFO] Marked uploaded: "
                      << fs::path(path).filename().string() << "\n";
        } catch (const std::exception& e) {
            std::cerr << "\033[31m[ERROR]\033[0m Rename failed: " << e.what() << "\n";
        }
    }
}

bool Uploader::prepare_video(const std::string& video_path, PreparedVideo& out) {
    struct stat st;
    if (::stat(video_path.c_str(), &st) != 0) {
        std::cerr << "\033[31m[ERROR]\033[0m File not found: " << video_path << "\n";
        return false;
    }

    out.video_path = video_path;
    out.file_size = st.st_size;

    if (out.file_size > BIG_THRESHOLD) {
        std::cerr << "\033[33m[WARNING]\033[0m Skipping oversized file: "
                  << fs::path(video_path).filename().string()
                  << " (" << format_file_size(out.file_size) << ")\n";
        try {
            fs::rename(video_path, video_path + ".big");
        } catch (const std::exception& e) {
            std::cerr << "\033[31m[ERROR]\033[0m Rename failed: " << e.what() << "\n";
        }
        return false;
    }

    out.info = get_video_info(video_path);
    out.thumb_path = find_thumbnail(video_path);
    out.caption = make_video_caption(video_path, out.info);
    return true;
}

// create_cover_thumb is defined in utils.cpp (shared with bot_uploader)

bool Uploader::send_prepared(PreparedVideo& pv) {
    auto send_start = SteadyClock::now();
    std::string fname = fs::path(pv.video_path).filename().string();
    std::string model = fs::path(pv.video_path).parent_path().filename().string();
    client_.set_current_context(model + "/" + fname);
    std::cout << "\n[INFO] Sending: " << fname
              << " (" << format_file_size(pv.file_size) << ") [" << model << "]\n";

    // Resize thumbnail to ≤320×320 for video cover (Telegram requirement).
    // The original full-res .jpg is still sent separately as a photo afterward.
    std::string cover_thumb;
    if (!pv.thumb_path.empty()) {
        std::cout << "[INFO] Thumbnail: " << fs::path(pv.thumb_path).filename().string() << "\n";
        cover_thumb = create_cover_thumb(pv.thumb_path);
    }
    // thumb_320 = resized 320×320 for thumbnail_, original = full-res for cover_
    const std::string& thumb_320 = cover_thumb.empty() ? pv.thumb_path : cover_thumb;

    int64_t msg_id;
    if (pv.pre_upload_id >= 0) {
        // File already uploaded to cloud, just send (fast)
        msg_id = client_.send_video_by_id(
            chat_id_, pv.pre_upload_id, pv.caption, thumb_320, pv.thumb_path,
            static_cast<int>(pv.info.duration),
            pv.info.width > 0 ? pv.info.width : 1920,
            pv.info.height > 0 ? pv.info.height : 1080);
    } else {
        // Direct upload + send
        msg_id = client_.send_video(
            chat_id_, pv.video_path, pv.caption, thumb_320, pv.thumb_path,
            static_cast<int>(pv.info.duration),
            pv.info.width > 0 ? pv.info.width : 1920,
            pv.info.height > 0 ? pv.info.height : 1080);
    }

    // Clean up the temp resized cover thumbnail
    if (!cover_thumb.empty()) {
        try { fs::remove(cover_thumb); } catch (...) {}
    }

    double send_secs = std::chrono::duration<double>(SteadyClock::now() - send_start).count();
    if (msg_id <= 0) {
        std::cerr << "\033[31m[ERROR]\033[0m Upload failed: "
                  << fs::path(pv.video_path).filename().string()
                  << " (" << fmt_elapsed(send_secs) << ")\n";
        return false;
    }

    std::cout << "[INFO] Success: " << fs::path(pv.video_path).filename().string()
              << " (" << fmt_elapsed(send_secs) << ")\n";

    // Record to scanner.db (use original full-res JPG as thumbnail, not the resized cover)
    {
        std::string base = get_base_dir();
        std::string db_path = base + "/data/scanner.db";
        std::string thumb_dir = base + "/data/thumbs";
        record_uploaded_video(db_path, thumb_dir, msg_id,
                              fs::path(pv.video_path).filename().string(),
                              static_cast<int>(pv.info.duration),
                              pv.info.width > 0 ? pv.info.width : 1920,
                              pv.info.height > 0 ? pv.info.height : 1080,
                              pv.file_size, pv.caption, pv.thumb_path);
    }

    // Send thumbnail as separate photo
    if (!pv.thumb_path.empty()) {
        auto thumb_start = SteadyClock::now();
        auto sz = get_jpeg_size(pv.thumb_path);
        std::string img_caption = make_image_caption(pv.thumb_path, sz.width, sz.height);
        int64_t photo_id = client_.send_photo(chat_id_, pv.thumb_path, img_caption);
        double thumb_secs = std::chrono::duration<double>(SteadyClock::now() - thumb_start).count();
        if (photo_id > 0) {
            std::cout << "[INFO] Thumbnail sent: message ID " << photo_id
                      << " (" << fmt_elapsed(thumb_secs) << ")\n";
            handle_post_upload(pv.thumb_path);
        } else {
            std::cerr << "\033[33m[WARNING]\033[0m Thumbnail upload failed (video OK)"
                      << " (" << fmt_elapsed(thumb_secs) << ")\n";
        }
    }

    // Random delay between uploads to avoid FLOOD_WAIT
    {
        static std::mt19937 rng(std::random_device{}());
        std::uniform_int_distribution<int> dist(3, 9);
        int delay = dist(rng);
        std::cout << "[INFO] Waiting " << delay << "s...\n";
        std::this_thread::sleep_for(std::chrono::seconds(delay));
    }

    return true;
}

bool Uploader::upload_single(const std::string& video_path) {
    PreparedVideo pv;
    if (!prepare_video(video_path, pv)) return false;
    return send_prepared(pv);
}

void Uploader::upload_batch(std::vector<PreparedVideo>& batch) {
    if (batch.empty()) return;
    auto batch_start = SteadyClock::now();

    // Phase 1: Start pre-uploading all files concurrently
    std::set<int32_t> pending, completed;
    std::map<int32_t, std::pair<int64_t,int64_t>> progress;

    for (auto& pv : batch) {
        int32_t fid = client_.start_pre_upload(pv.video_path);
        if (fid >= 0) {
            pv.pre_upload_id = fid;
            pending.insert(fid);
        }
    }

    std::cout << "\033[35m[PRE-UPLOAD]\033[0m Waiting for " << pending.size() << " files...\n";

    auto speed_time = std::chrono::steady_clock::now();
    int64_t speed_bytes = 0;
    double speed_mbs = 0.0;

    // Phase 2+3: Pipeline — send each file as soon as it's ready, in order
    for (auto& pv : batch) {
        if (pv.pre_upload_id < 0) {
            // No pre-upload, send directly
            if (send_prepared(pv)) handle_post_upload(pv.video_path);
            continue;
        }

        // Wait until THIS file's upload is done (others may finish too, tracked in completed)
        while (!completed.count(pv.pre_upload_id) && !pending.empty()) {
            client_.poll_pre_uploads(pending, completed, progress);

            // Display overall progress + speed
            int64_t total_up = 0, total_ex = 0;
            for (auto& [fid, p] : progress) total_up += p.first, total_ex += p.second;
            if (total_ex > 0) {
                auto now = std::chrono::steady_clock::now();
                double elapsed = std::chrono::duration<double>(now - speed_time).count();
                if (elapsed >= 0.5) {
                    speed_mbs = (total_up - speed_bytes) / elapsed / 1048576.0;
                    speed_bytes = total_up;
                    speed_time = now;
                }
                int pct = static_cast<int>(100.0 * total_up / total_ex);
                std::printf("\r\033[35m[PRE-UPLOAD]\033[0m %d%% (%.1f / %.1f MB) %.1f MB/s [%zu/%zu done]   ",
                    pct, total_up / 1048576.0, total_ex / 1048576.0,
                    speed_mbs, completed.size(), batch.size());
                std::fflush(stdout);
            }
        }

        std::printf("\r\033[35m[PRE-UPLOAD]\033[0m File ready, sending: %s                    \n",
            fs::path(pv.video_path).filename().string().c_str());

        if (send_prepared(pv)) handle_post_upload(pv.video_path);
    }

    double batch_secs = std::chrono::duration<double>(SteadyClock::now() - batch_start).count();
    std::cout << "[BATCH] Batch done: " << batch.size() << " files, elapsed " << fmt_elapsed(batch_secs) << "\n";
}

void Uploader::upload_directory(const std::string& dir_path, bool recursive) {
    if (!fs::is_directory(dir_path)) {
        std::cerr << "\033[31m[ERROR]\033[0m Directory not found: " << dir_path << "\n";
        return;
    }

    auto video_files = scan_videos(dir_path, recursive);
    std::cout << "[INFO] Found " << video_files.size() << " video files to upload\n";
    if (concurrent_ > 1) {
        std::cout << "[INFO] Concurrent pre-upload: " << concurrent_ << " files at a time\n";
    }

    if (video_files.empty()) return;

    auto total_start = SteadyClock::now();

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

    // Group by folder
    std::map<std::string, std::vector<std::string>> folder_groups;
    for (const auto& vf : video_files) {
        folder_groups[fs::path(vf).parent_path().string()].push_back(vf);
    }

    // Select available (unlocked) folders, shuffle
    std::vector<std::string> available_folders;
    int locked_count = 0;
    for (auto& [folder, files] : folder_groups) {
        if (is_folder_locked(folder)) {
            locked_count++;
        } else {
            available_folders.push_back(folder);
        }
    }

    if (locked_count > 0) {
        std::cout << "[INFO] Skipping " << locked_count << " locked folders\n";
    }

    if (available_folders.empty()) {
        std::cout << "\033[33m[WARNING]\033[0m All folders locked\n";
        return;
    }

    std::random_device rd;
    std::mt19937 rng(rd());
    std::shuffle(available_folders.begin(), available_folders.end(), rng);
    std::cout << "[INFO] Available folders: " << available_folders.size() << "\n";

    int success_count = 0;
    int skipped_count = 0;
    int total_files = static_cast<int>(video_files.size());
    std::string current_locked_folder;

    for (const auto& folder_path : available_folders) {
        if (!create_folder_lock(folder_path, account_.name)) {
            std::cout << "\033[33m[WARNING]\033[0m Cannot lock folder, skipping: " << folder_path << "\n";
            continue;
        }

        current_locked_folder = folder_path;
        g_locked_folder = folder_path;  // for signal handler cleanup
        auto folder_start = SteadyClock::now();
        auto& folder_videos = folder_groups[folder_path];
        std::sort(folder_videos.begin(), folder_videos.end());
        std::cout << "[INFO] Processing folder: " << folder_path
                  << " (" << folder_videos.size() << " files)\n";

        if (concurrent_ <= 1) {
            // Sequential mode: upload one by one
            for (const auto& video_file : folder_videos) {
                try {
                    if (upload_single(video_file)) {
                        success_count++;
                        handle_post_upload(video_file);
                    } else {
                        skipped_count++;
                    }
                } catch (const std::exception& e) {
                    std::cerr << "\033[31m[ERROR]\033[0m Upload failed "
                              << fs::path(video_file).filename().string()
                              << ": " << e.what() << "\n";
                }
            }
        } else {
            // Concurrent mode: batch pre-upload + ordered send
            for (size_t i = 0; i < folder_videos.size(); i += concurrent_) {
                size_t batch_end = std::min(i + concurrent_, folder_videos.size());

                // Prepare batch
                std::vector<PreparedVideo> batch;
                for (size_t j = i; j < batch_end; j++) {
                    PreparedVideo pv;
                    if (prepare_video(folder_videos[j], pv)) {
                        batch.push_back(std::move(pv));
                    } else {
                        skipped_count++;
                    }
                }

                if (batch.empty()) continue;

                std::cout << "\n[BATCH] Pre-uploading " << batch.size()
                          << " files concurrently...\n";

                // Pre-upload + ordered send
                size_t before = success_count;
                upload_batch(batch);

                // Count successes (files that were post-processed already in upload_batch)
                // upload_batch handles post_upload internally for successful sends
                for (auto& pv : batch) {
                    // Check if file was handled (deleted or renamed)
                    if (!fs::exists(pv.video_path)) {
                        success_count++;
                    } else {
                        // Check if marked as uploaded
                        if (fs::exists(pv.video_path + settings_.uploaded_suffix)) {
                            success_count++;
                        }
                    }
                }
            }
        }

        double folder_secs = std::chrono::duration<double>(SteadyClock::now() - folder_start).count();
        std::cout << "[INFO] Folder done: " << folder_path
                  << " (" << fmt_elapsed(folder_secs) << ")\n";
        remove_folder_lock(current_locked_folder);
        current_locked_folder.clear();
        g_locked_folder.clear();
    }

    if (!current_locked_folder.empty()) {
        remove_folder_lock(current_locked_folder);
        g_locked_folder.clear();
    }

    double total_secs = std::chrono::duration<double>(SteadyClock::now() - total_start).count();
    int flood_hits = client_.flood_wait_hits();
    int flood_secs = client_.flood_wait_total_secs();
    std::cout << "\n[INFO] Batch complete: " << success_count << "/"
              << total_files << " uploaded, " << skipped_count << " skipped"
              << " (total " << fmt_elapsed(total_secs);
    if (flood_hits > 0) {
        std::cout << ", flood_wait " << flood_hits << "x " << fmt_elapsed(flood_secs);
    }
    std::cout << ")\n";
}
