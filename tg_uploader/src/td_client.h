#pragma once
#include "config.h"
#include <td/telegram/Client.h>
#include <td/telegram/td_api.h>
#include <td/telegram/td_api.hpp>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace td_api = td::td_api;

struct TdAuthStatus {
    bool ok = true;
    bool authorized = false;
    std::string state = "starting";  // starting | wait_code | wait_password | ready | closed | error
    std::string error;
    std::string phone;
};

class TdClient {
public:
    explicit TdClient(const AccountConfig& config);
    ~TdClient();

    // Blocking login (handles auth state machine, reads code/password from stdin)
    bool login();
    bool is_authorized() const { return authorized_; }

    // Non-interactive login for the web UI. start_web_auth() drives TDLib until
    // it either reaches ready or needs user input. submit_* continues the same
    // session after the browser provides the code/password.
    TdAuthStatus start_web_auth(double timeout = 30.0);
    TdAuthStatus submit_auth_code(const std::string& code, double timeout = 30.0);
    TdAuthStatus submit_auth_password(const std::string& password, double timeout = 30.0);

    // Find channel by username or title, returns chat_id
    int64_t find_channel(const std::string& channel_id);

    // Send video with optional thumbnail/cover and caption, returns message_id or 0 on failure
    int64_t send_video(int64_t chat_id, const std::string& video_path,
                       const std::string& caption_markdown,
                       const std::string& thumb_path,
                       const std::string& cover_path,
                       int duration, int width, int height);

    // Send video using pre-uploaded file_id, returns message_id or 0 on failure
    int64_t send_video_by_id(int64_t chat_id, int32_t file_id,
                              const std::string& caption_markdown,
                              const std::string& thumb_path,
                              const std::string& cover_path,
                              int duration, int width, int height);

    // Send photo with caption, returns message_id or 0 on failure
    int64_t send_photo(int64_t chat_id, const std::string& photo_path,
                       const std::string& caption_markdown);

    // Search chat messages with filter, for channel scanning
    td_api::object_ptr<td_api::foundChatMessages> search_chat_messages(
        int64_t chat_id, int64_t from_message_id, int32_t limit,
        td_api::object_ptr<td_api::SearchMessagesFilter> filter);

    // Download a file synchronously, returns local file path
    std::string download_file_sync(int32_t file_id);

    // Get a single message by chat_id and message_id
    td_api::object_ptr<td_api::message> get_message(int64_t chat_id, int64_t message_id);

    // Pre-upload a local file to cloud, returns TDLib file_id (-1 on error)
    int32_t start_pre_upload(const std::string& file_path);

    // Wait for multiple pre-uploads to finish transferring data
    void wait_pre_uploads(const std::vector<int32_t>& file_ids);

    // Poll one receive cycle; marks any completed file_ids into `completed`.
    // progress: file_id -> {uploaded_bytes, total_bytes}
    // Returns file_id that just completed this call, or -1 if none yet.
    int32_t poll_pre_uploads(std::set<int32_t>& pending,
                              std::set<int32_t>& completed,
                              std::map<int32_t, std::pair<int64_t,int64_t>>& progress);

    void close();

    // FLOOD_WAIT tracking: caller reads this to adjust upload intervals
    int flood_wait_hits() const { return flood_wait_hits_; }
    int flood_wait_total_secs() const { return flood_wait_total_secs_; }
    void reset_flood_wait_hits() { flood_wait_hits_ = 0; flood_wait_total_secs_ = 0; }

    // Set current file context shown in FLOOD_WAIT logs so operator knows which file is affected
    void set_current_context(const std::string& ctx) { current_context_ = ctx; }

private:
    td::ClientManager client_manager_;
    int32_t client_id_;
    AccountConfig config_;
    bool authorized_ = false;
    bool closed_ = false;
    bool web_auth_mode_ = false;
    std::string auth_state_ = "starting";
    std::string auth_error_;
    uint64_t next_query_id_ = 1;
    int flood_wait_hits_ = 0;  // Incremented each FLOOD_WAIT, read by Uploader
    int flood_wait_total_secs_ = 0;  // Cumulative FLOOD_WAIT seconds
    std::string current_context_;    // Current file/model for FLOOD_WAIT display

    // Send request and synchronously wait for response
    td_api::object_ptr<td_api::Object> send_sync(
        td_api::object_ptr<td_api::Function> request, double timeout = 300.0);

    // Process a single update
    void process_update(td_api::object_ptr<td_api::Object> update);

    // Handle authorization state changes
    void handle_auth_state(td_api::object_ptr<td_api::AuthorizationState> state);
    TdAuthStatus make_auth_status() const;
    TdAuthStatus pump_auth(double timeout);

    // Parse caption markdown into formattedText
    td_api::object_ptr<td_api::formattedText> parse_caption(const std::string& markdown);

    // Handle FLOOD_WAIT: increment counter, sleep with exponential backoff
    void handle_flood_wait(int server_wait, const char* context = "");

    // Internal: send video message and wait for completion (shared by send_video and send_video_by_id)
    int64_t send_video_impl(int64_t chat_id,
                             td_api::object_ptr<td_api::InputFile> input_file,
                             const std::string& caption_markdown,
                             const std::string& thumb_path,
                             const std::string& cover_path,
                             int duration, int width, int height);
};
