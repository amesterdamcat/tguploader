#pragma once
#include <string>
#include <cstdint>

struct BotApiResponse {
    bool ok = false;
    int64_t message_id = 0;
    int retry_after = 0;  // seconds to wait (429 FLOOD_WAIT)
    std::string error;
};

// HTTP client for a self-hosted Telegram Bot API local server.
// Requires the server to be started with the --local flag, which allows
// passing absolute local file paths as parameter values instead of uploading
// file content over HTTP. The server reads files from disk directly.
class BotClient {
public:
    BotClient(const std::string& api_url, const std::string& token);
    ~BotClient();

    BotClient(const BotClient&) = delete;
    BotClient& operator=(const BotClient&) = delete;

    void set_progress_prefix(const std::string& pfx) { log_prefix_ = pfx; }

    // Send a video. video_path and thumb_path are absolute local file paths.
    // The local bot API server reads these files from disk (--local mode).
    BotApiResponse send_video(const std::string& chat_id,
                               const std::string& video_path,
                               const std::string& caption,
                               int duration, int width, int height,
                               const std::string& thumb_path = "");

    // Send a photo. photo_path is an absolute local file path.
    BotApiResponse send_photo(const std::string& chat_id,
                               const std::string& photo_path,
                               const std::string& caption);

private:
    std::string api_url_;
    std::string token_;
    void* curl_;  // CURL* opaque handle (avoids including curl.h here)

    std::string make_url(const std::string& method) const;
    std::string log_prefix_;  // colored bot prefix for progress output
    BotApiResponse do_post(const std::string& url, void* form,
                           const std::string& progress_prefix = "") const;
    BotApiResponse parse_response(const std::string& body) const;
};
