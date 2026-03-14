#include "td_client.h"
#include "utils.h"
#include <filesystem>
#include <iostream>
#include <iomanip>
#include <string>
#include <thread>
#include <chrono>
#include <random>
#include <regex>
#include <set>
#include <utility>

using Clock = std::chrono::steady_clock;

// Parse "retry after N" from error message, return seconds (0 if not found)
static int parse_retry_after(const std::string& msg) {
    std::regex re(R"(retry after (\d+))");
    std::smatch m;
    if (std::regex_search(msg, m, re)) {
        return std::stoi(m[1].str());
    }
    return 0;
}

void TdClient::handle_flood_wait(int server_wait, const char* context) {
    flood_wait_hits_++;

    // server_wait + random 3~8s
    static std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> dist(3, 8);
    int extra = dist(rng);
    int actual_wait = server_wait + extra;

    // Singapore time (UTC+8), thread-safe
    auto now = std::time(nullptr);
    std::tm utc{};
    gmtime_r(&now, &utc);
    utc.tm_hour += 8;
    std::mktime(&utc); // normalize overflow
    char timebuf[20];
    std::strftime(timebuf, sizeof(timebuf), "%H:%M:%S", &utc);

    flood_wait_total_secs_ += actual_wait;
    std::cout << "\n\033[33m[FLOOD_WAIT]\033[0m " << timebuf
              << " (#" << flood_wait_hits_ << ") " << context
              << server_wait << "s +" << extra << "s = "
              << actual_wait << "s (cumulative: " << flood_wait_total_secs_ << "s)";
    if (!current_context_.empty()) {
        std::cout << " [" << current_context_ << "]";
    }
    std::cout << "\n";
    std::this_thread::sleep_for(std::chrono::seconds(actual_wait));
}

TdClient::TdClient(const AccountConfig& config) : config_(config) {
    td::ClientManager::execute(
        td_api::make_object<td_api::setLogVerbosityLevel>(1));
    client_id_ = client_manager_.create_client_id();
    // Send a dummy request to trigger auth state updates
    client_manager_.send(client_id_, next_query_id_++,
                         td_api::make_object<td_api::getOption>("version"));
}

TdClient::~TdClient() {
    if (!closed_) close();
}

void TdClient::process_update(td_api::object_ptr<td_api::Object> update) {
    if (!update) return;
    if (update->get_id() == td_api::updateAuthorizationState::ID) {
        auto& auth_update = static_cast<td_api::updateAuthorizationState&>(*update);
        handle_auth_state(std::move(auth_update.authorization_state_));
    }
}

void TdClient::handle_auth_state(td_api::object_ptr<td_api::AuthorizationState> state) {
    if (!state) return;

    switch (state->get_id()) {
    case td_api::authorizationStateWaitTdlibParameters::ID: {
        auto params = td_api::make_object<td_api::setTdlibParameters>();
        params->database_directory_ = config_.session_dir;
        params->use_message_database_ = true;
        params->use_secret_chats_ = false;
        params->api_id_ = config_.api_id;
        params->api_hash_ = config_.api_hash;
        params->system_language_code_ = "en";
        params->device_model_ = "Desktop";
        params->application_version_ = "1.0";
        send_sync(std::move(params), 10.0);
        break;
    }
    case td_api::authorizationStateWaitPhoneNumber::ID: {
        std::cout << "Setting phone number: " << config_.phone << "\n";
        auto req = td_api::make_object<td_api::setAuthenticationPhoneNumber>(
            config_.phone, nullptr);
        send_sync(std::move(req), 30.0);
        break;
    }
    case td_api::authorizationStateWaitCode::ID: {
        std::cout << "Enter verification code for " << config_.phone << ": ";
        std::string code;
        std::getline(std::cin, code);
        send_sync(td_api::make_object<td_api::checkAuthenticationCode>(code), 30.0);
        break;
    }
    case td_api::authorizationStateWaitPassword::ID: {
        std::cout << "Enter 2FA password for " << config_.phone << ": ";
        std::string password;
        std::getline(std::cin, password);
        send_sync(td_api::make_object<td_api::checkAuthenticationPassword>(password), 30.0);
        break;
    }
    case td_api::authorizationStateReady::ID:
        authorized_ = true;
        std::cout << "Authorization successful.\n";
        // Enable automatic storage cleanup to prevent TDLib cache bloat
        send_sync(td_api::make_object<td_api::setOption>(
            "use_storage_optimizer",
            td_api::make_object<td_api::optionValueBoolean>(true)), 5.0);
        break;
    case td_api::authorizationStateClosed::ID:
        closed_ = true;
        break;
    default:
        break;
    }
}

td_api::object_ptr<td_api::Object> TdClient::send_sync(
    td_api::object_ptr<td_api::Function> request, double timeout) {

    uint64_t qid = next_query_id_++;
    client_manager_.send(client_id_, qid, std::move(request));

    while (true) {
        auto response = client_manager_.receive(timeout);
        if (!response.object) {
            return nullptr;
        }

        if (response.request_id == 0) {
            process_update(std::move(response.object));
        } else if (response.request_id == qid) {
            if (response.object->get_id() == td_api::error::ID) {
                auto& err = static_cast<td_api::error&>(*response.object);
                std::cerr << "\033[31m[TDLib error " << err.code_ << "]\033[0m " << err.message_ << "\n";
            }
            return std::move(response.object);
        } else {
            // Discard responses for other queries (not our current qid)
            // to avoid unbounded memory growth
        }
    }
}

bool TdClient::login() {
    for (int i = 0; i < 100 && !authorized_ && !closed_; i++) {
        auto response = client_manager_.receive(10.0);
        if (!response.object) continue;
        if (response.request_id == 0) {
            process_update(std::move(response.object));
        } else {
            if (response.object->get_id() == td_api::error::ID) {
                auto& err = static_cast<td_api::error&>(*response.object);
                std::cerr << "\033[31m[TDLib error " << err.code_ << "]\033[0m " << err.message_ << "\n";
                if (err.code_ == 401) return false;
            }
        }
    }
    return authorized_;
}

int64_t TdClient::find_channel(const std::string& channel_id) {
    // Always load the user's chat list first so TDLib knows our permissions
    send_sync(td_api::make_object<td_api::loadChats>(
        td_api::make_object<td_api::chatListMain>(), 200), 15.0);

    // Search locally cached chats by title (fast, no network calls per chat)
    auto search_result = send_sync(td_api::make_object<td_api::searchChats>(
        channel_id, 20), 15.0);

    if (search_result && search_result->get_id() == td_api::chats::ID) {
        auto& chats = static_cast<td_api::chats&>(*search_result);
        for (auto chat_id_val : chats.chat_ids_) {
            auto chat_result = send_sync(
                td_api::make_object<td_api::getChat>(chat_id_val), 5.0);
            if (chat_result && chat_result->get_id() == td_api::chat::ID) {
                auto& chat = static_cast<td_api::chat&>(*chat_result);
                if (chat.title_ == channel_id) {
                    std::cout << "[INFO] Found channel: " << chat.title_
                              << " (ID: " << chat.id_ << ")\n";
                    return chat.id_;
                }
            }
        }
    }

    // Fallback: try searchPublicChat (for @username style)
    std::string search_name = channel_id;
    if (!search_name.empty() && search_name[0] == '@') {
        search_name = search_name.substr(1);
    }

    auto result = send_sync(
        td_api::make_object<td_api::searchPublicChat>(search_name), 15.0);

    if (result && result->get_id() == td_api::chat::ID) {
        auto& chat = static_cast<td_api::chat&>(*result);
        std::cout << "[INFO] Found channel: " << chat.title_
                  << " (ID: " << chat.id_ << ")\n";
        return chat.id_;
    }

    std::cerr << "\033[31m[ERROR]\033[0m Channel not found: " << channel_id << "\n";
    return 0;
}

td_api::object_ptr<td_api::formattedText> TdClient::parse_caption(
    const std::string& markdown) {
    auto result = send_sync(
        td_api::make_object<td_api::parseMarkdown>(
            td_api::make_object<td_api::formattedText>(markdown,
                std::vector<td_api::object_ptr<td_api::textEntity>>{})),
        5.0);

    if (result && result->get_id() == td_api::formattedText::ID) {
        return td_api::move_object_as<td_api::formattedText>(result);
    }

    // Fallback: plain text
    return td_api::make_object<td_api::formattedText>(
        markdown, std::vector<td_api::object_ptr<td_api::textEntity>>{});
}

// ---- Search & Download (for scanner) ----

td_api::object_ptr<td_api::foundChatMessages> TdClient::search_chat_messages(
    int64_t chat_id, int64_t from_message_id, int32_t limit,
    td_api::object_ptr<td_api::SearchMessagesFilter> filter) {

    auto request = td_api::make_object<td_api::searchChatMessages>(
        chat_id,
        nullptr,        // topic_id (MessageTopic)
        "",             // query (empty = all)
        nullptr,        // sender_id
        from_message_id,
        0,              // offset
        limit,
        std::move(filter)
    );

    auto result = send_sync(std::move(request), 30.0);
    if (result && result->get_id() == td_api::foundChatMessages::ID) {
        return td_api::move_object_as<td_api::foundChatMessages>(result);
    }
    return nullptr;
}

td_api::object_ptr<td_api::message> TdClient::get_message(int64_t chat_id, int64_t message_id) {
    auto result = send_sync(
        td_api::make_object<td_api::getMessage>(chat_id, message_id), 10.0);
    if (result && result->get_id() == td_api::message::ID) {
        return td_api::move_object_as<td_api::message>(result);
    }
    return nullptr;
}

std::string TdClient::download_file_sync(int32_t file_id) {
    if (file_id <= 0) return "";

    auto result = send_sync(
        td_api::make_object<td_api::downloadFile>(
            file_id,
            32,     // priority
            0,      // offset
            0,      // limit (0 = entire file)
            true    // synchronous
        ), 60.0);

    if (result && result->get_id() == td_api::file::ID) {
        auto& file = static_cast<td_api::file&>(*result);
        if (file.local_ && file.local_->is_downloading_completed_) {
            return file.local_->path_;
        }
    }
    return "";
}

// ---- Pre-upload ----

int32_t TdClient::start_pre_upload(const std::string& file_path) {
    auto result = send_sync(
        td_api::make_object<td_api::preliminaryUploadFile>(
            td_api::make_object<td_api::inputFileLocal>(file_path),
            td_api::make_object<td_api::fileTypeVideo>(),
            32  // highest priority
        ), 15.0);

    if (result && result->get_id() == td_api::file::ID) {
        auto& file = static_cast<td_api::file&>(*result);
        std::cout << "\033[35m[PRE-UPLOAD]\033[0m Started: file_id=" << file.id_
                  << " (" << file_path.substr(file_path.rfind('/') + 1) << ")\n";
        return file.id_;
    }

    std::cerr << "\033[31m[ERROR]\033[0m Failed to start pre-upload: " << file_path << "\n";
    return -1;
}

void TdClient::wait_pre_uploads(const std::vector<int32_t>& file_ids) {
    if (file_ids.empty()) return;

    std::set<int32_t> pending(file_ids.begin(), file_ids.end());
    // Track uploaded bytes per file for progress display
    std::map<int32_t, std::pair<int64_t, int64_t>> progress; // file_id -> {uploaded, total}

    std::cout << "\033[35m[PRE-UPLOAD]\033[0m Waiting for " << pending.size() << " files...\n";

    while (!pending.empty()) {
        auto response = client_manager_.receive(1.0);
        if (!response.object) continue;

        if (response.request_id == 0) {
            if (response.object->get_id() == td_api::updateFile::ID) {
                auto& upd = static_cast<td_api::updateFile&>(*response.object);
                if (upd.file_ && pending.count(upd.file_->id_)) {
                    int32_t fid = upd.file_->id_;
                    int64_t expected = upd.file_->expected_size_;
                    if (expected <= 0) expected = upd.file_->size_;

                    if (upd.file_->remote_) {
                        auto& remote = upd.file_->remote_;
                        int64_t uploaded = remote->uploaded_size_;
                        progress[fid] = {uploaded, expected};

                        // Display overall progress
                        int64_t total_uploaded = 0, total_expected = 0;
                        for (auto& [id, p] : progress) {
                            total_uploaded += p.first;
                            total_expected += p.second;
                        }
                        if (total_expected > 0) {
                            int pct = static_cast<int>(100.0 * total_uploaded / total_expected);
                            std::printf("\r\033[35m[PRE-UPLOAD]\033[0m %d%% (%.1f / %.1f MB, %zu/%zu files)   ",
                                        pct,
                                        total_uploaded / 1048576.0,
                                        total_expected / 1048576.0,
                                        file_ids.size() - pending.size(),
                                        file_ids.size());
                            std::fflush(stdout);
                        }

                        // Check if this file is done uploading data
                        if (remote->is_uploading_completed_) {
                            pending.erase(fid);
                        } else if (expected > 0 && uploaded >= expected) {
                            pending.erase(fid);
                        } else if (!remote->is_uploading_active_ && uploaded > 0 && expected > 0
                                   && uploaded >= expected * 99 / 100) {
                            // Upload paused at near 100% — TDLib waiting for sendMessage
                            pending.erase(fid);
                        }
                    }
                }
            }
            process_update(std::move(response.object));
        }
    }

    std::printf("\r\033[35m[PRE-UPLOAD]\033[0m All %zu files uploaded to cloud.                    \n",
                file_ids.size());
}

int32_t TdClient::poll_pre_uploads(std::set<int32_t>& pending,
                                     std::set<int32_t>& completed,
                                     std::map<int32_t, std::pair<int64_t,int64_t>>& progress) {
    auto response = client_manager_.receive(0.5);
    if (!response.object) return -1;

    if (response.request_id == 0) {
        if (response.object->get_id() == td_api::updateFile::ID) {
            auto& upd = static_cast<td_api::updateFile&>(*response.object);
            if (upd.file_ && upd.file_->remote_) {
                int32_t fid = upd.file_->id_;
                if (pending.count(fid)) {
                    int64_t uploaded = upd.file_->remote_->uploaded_size_;
                    int64_t expected = upd.file_->expected_size_ > 0
                                       ? upd.file_->expected_size_
                                       : upd.file_->size_;
                    progress[fid] = {uploaded, expected};

                    bool done = upd.file_->remote_->is_uploading_completed_
                        || (expected > 0 && uploaded >= expected)
                        || (!upd.file_->remote_->is_uploading_active_
                            && uploaded > 0 && expected > 0
                            && uploaded >= expected * 99 / 100);
                    if (done) {
                        pending.erase(fid);
                        completed.insert(fid);
                        return fid;
                    }
                }
            }
        }
        process_update(std::move(response.object));
    }
    return -1;
}

// ---- Send video (internal implementation) ----

int64_t TdClient::send_video_impl(int64_t chat_id,
                                    td_api::object_ptr<td_api::InputFile> input_file,
                                    const std::string& caption_markdown,
                                    const std::string& thumb_path,
                                    const std::string& cover_path,
                                    int duration, int width, int height) {
    auto upload_speed_time = Clock::now();
    int64_t upload_speed_bytes = 0;
    double upload_speed_mbs = 0.0;

    {
        auto video = td_api::make_object<td_api::inputMessageVideo>();
        video->video_ = std::move(input_file);
        video->duration_ = duration;
        video->width_ = width > 0 ? width : 1920;
        video->height_ = height > 0 ? height : 1080;
        video->supports_streaming_ = true;

        if (!thumb_path.empty()) {
            auto sz = get_jpeg_size(thumb_path);
            int tw = sz.width  > 0 ? sz.width  : 320;
            int th = sz.height > 0 ? sz.height : 320;
            video->thumbnail_ = td_api::make_object<td_api::inputThumbnail>(
                td_api::make_object<td_api::inputFileLocal>(thumb_path), tw, th);
        }

        // Cover: full-res image shown as video preview in Telegram client
        if (!cover_path.empty()) {
            video->cover_ = td_api::make_object<td_api::inputFileLocal>(cover_path);
            auto csz = get_jpeg_size(cover_path);
            std::cout << "[INFO] Cover: " << csz.width << "x" << csz.height
                      << " (" << format_file_size(std::filesystem::file_size(cover_path)) << ")\n";
        }

        video->caption_ = parse_caption(caption_markdown);

        auto msg = td_api::make_object<td_api::sendMessage>();
        msg->chat_id_ = chat_id;
        msg->input_message_content_ = std::move(video);
        // Silent send: don't push notifications to channel subscribers during batch upload
        auto opts = td_api::make_object<td_api::messageSendOptions>();
        opts->disable_notification_ = true;
        msg->options_ = std::move(opts);

        auto result = send_sync(std::move(msg), 30.0);

        if (!result) return 0;

        if (result->get_id() == td_api::error::ID) {
            auto& err = static_cast<td_api::error&>(*result);
            int wait = parse_retry_after(err.message_);
            if (wait > 0) {
                handle_flood_wait(wait);
                return -1; // signal: need retry from caller
            }
            std::cerr << "\n\033[31m[ERROR]\033[0m sendMessage failed: " << err.message_ << "\n";
            return 0;
        }

        // Capture the temporary message ID assigned by TDLib for this sendMessage.
        // TDLib may fire updateMessageSendFailed for *other* messages (e.g. a
        // previously-sent photo whose local file was deleted by handle_post_upload)
        // while we wait here. Without this guard we'd mistake those events for
        // the current video send failing.
        int64_t pending_msg_id = 0;
        if (result->get_id() == td_api::message::ID) {
            pending_msg_id = static_cast<td_api::message&>(*result).id_;
        }

        // Got preliminary message, wait for upload + send to complete
        for (int i = 0; i < 36000; i++) { // up to ~1 hour
            auto response = client_manager_.receive(0.1);
            if (!response.object) continue;
            if (response.request_id == 0) {
                if (response.object->get_id() == td_api::updateMessageSendSucceeded::ID) {
                    auto& upd = static_cast<td_api::updateMessageSendSucceeded&>(*response.object);
                    if (pending_msg_id != 0 && upd.old_message_id_ != pending_msg_id) {
                        process_update(std::move(response.object));
                        continue;
                    }
                    std::cout << "\n[INFO] Video sent: message ID "
                              << upd.message_->id_ << "\n";
                    return upd.message_->id_;
                }
                if (response.object->get_id() == td_api::updateMessageSendFailed::ID) {
                    auto& upd = static_cast<td_api::updateMessageSendFailed&>(*response.object);
                    // Not our message — ignore, keep waiting
                    if (pending_msg_id != 0 && upd.old_message_id_ != pending_msg_id) {
                        process_update(std::move(response.object));
                        continue;
                    }
                    std::string err_msg = upd.error_ ? upd.error_->message_ : "unknown error";
                    int wait = parse_retry_after(err_msg);
                    if (wait > 0) {
                        handle_flood_wait(wait);
                        return -1; // signal: need retry from caller
                    }
                    // FILE_PARTS_INVALID: file too large for account tier, or stale cache.
                    // Return -2 so caller can retry once; if it fails again, give up.
                    if (err_msg.find("FILE_PARTS_INVALID") != std::string::npos) {
                        if (upd.message_ && upd.message_->content_ &&
                            upd.message_->content_->get_id() == td_api::messageVideo::ID) {
                            auto& mv = static_cast<td_api::messageVideo&>(*upd.message_->content_);
                            if (mv.video_ && mv.video_->video_) {
                                send_sync(td_api::make_object<td_api::deleteFile>(
                                    mv.video_->video_->id_), 5.0);
                            }
                        }
                        return -2; // signal: FILE_PARTS_INVALID, one retry allowed
                    }
                    std::cerr << "\n\033[31m[ERROR]\033[0m Video send failed: " << err_msg << "\n";
                    return 0;
                }
                // Track file upload progress
                if (response.object->get_id() == td_api::updateFile::ID) {
                    auto& upd = static_cast<td_api::updateFile&>(*response.object);
                    if (upd.file_ && upd.file_->remote_) {
                        auto& remote = upd.file_->remote_;
                        if (remote->is_uploading_active_ && upd.file_->expected_size_ > 0) {
                            int64_t uploaded = remote->uploaded_size_;
                            int64_t total = upd.file_->expected_size_;
                            int pct = static_cast<int>(100.0 * uploaded / total);

                            auto now = std::chrono::steady_clock::now();
                            double elapsed = std::chrono::duration<double>(
                                now - upload_speed_time).count();
                            if (elapsed >= 0.5) {
                                upload_speed_mbs = (uploaded - upload_speed_bytes)
                                                   / elapsed / 1048576.0;
                                upload_speed_bytes = uploaded;
                                upload_speed_time = now;
                            }

                            std::printf("\rUploading: %d%% (%.1f / %.1f MB) %.1f MB/s   ",
                                        pct, uploaded / 1048576.0, total / 1048576.0,
                                        upload_speed_mbs);
                            std::fflush(stdout);
                        }
                        if (remote->is_uploading_completed_) {
                            std::printf("\rUpload complete.                              \n");
                        }
                    }
                }
                process_update(std::move(response.object));
            }
        }

        std::cerr << "\n\033[31m[ERROR]\033[0m send_video timed out\n";
        return 0;
    }
    // unreachable — inner loop always returns
}

// ---- Public send_video (local file path) ----

int64_t TdClient::send_video(int64_t chat_id, const std::string& video_path,
                              const std::string& caption_markdown,
                              const std::string& thumb_path,
                              const std::string& cover_path,
                              int duration, int width, int height) {
    int parts_invalid_retries = 0;
    while (true) {
        auto input = td_api::make_object<td_api::inputFileLocal>(video_path);
        int64_t result = send_video_impl(chat_id, std::move(input),
                                          caption_markdown, thumb_path, cover_path,
                                          duration, width, height);
        if (result == -1) continue; // FLOOD_WAIT: retry
        if (result == -2) {         // FILE_PARTS_INVALID
            if (parts_invalid_retries++ < 1) {
                std::cout << "[WARN] FILE_PARTS_INVALID - cache cleared, retrying once...\n";
                continue;
            }
            std::cerr << "\033[31m[ERROR]\033[0m FILE_PARTS_INVALID persists - file likely exceeds "
                         "account upload limit (free: ~1.95 GiB). Skipping.\n";
            return 0;
        }
        return result;
    }
}

// ---- Public send_video_by_id (pre-uploaded file) ----

int64_t TdClient::send_video_by_id(int64_t chat_id, int32_t file_id,
                                     const std::string& caption_markdown,
                                     const std::string& thumb_path,
                                     const std::string& cover_path,
                                     int duration, int width, int height) {
    int parts_invalid_retries = 0;
    while (true) {
        auto input = td_api::make_object<td_api::inputFileId>(file_id);
        int64_t result = send_video_impl(chat_id, std::move(input),
                                          caption_markdown, thumb_path, cover_path,
                                          duration, width, height);
        if (result == -1) continue; // FLOOD_WAIT: retry
        if (result == -2) {         // FILE_PARTS_INVALID
            if (parts_invalid_retries++ < 1) {
                std::cout << "[WARN] FILE_PARTS_INVALID - cache cleared, retrying once...\n";
                continue;
            }
            std::cerr << "\033[31m[ERROR]\033[0m FILE_PARTS_INVALID persists - file likely exceeds "
                         "account upload limit (free: ~1.95 GiB). Skipping.\n";
            return 0;
        }
        return result;
    }
}

// ---- Send photo ----

int64_t TdClient::send_photo(int64_t chat_id, const std::string& photo_path,
                              const std::string& caption_markdown) {
    while (true) {
        auto photo = td_api::make_object<td_api::inputMessagePhoto>();
        photo->photo_ = td_api::make_object<td_api::inputFileLocal>(photo_path);
        photo->caption_ = parse_caption(caption_markdown);

        auto msg = td_api::make_object<td_api::sendMessage>();
        msg->chat_id_ = chat_id;
        msg->input_message_content_ = std::move(photo);
        auto opts = td_api::make_object<td_api::messageSendOptions>();
        opts->disable_notification_ = true;
        msg->options_ = std::move(opts);

        auto result = send_sync(std::move(msg), 60.0);

        if (!result) return 0;

        if (result->get_id() == td_api::error::ID) {
            auto& err = static_cast<td_api::error&>(*result);
            int wait = parse_retry_after(err.message_);
            if (wait > 0) {
                handle_flood_wait(wait, "Photo: ");
                continue;
            }
            std::cerr << "\033[31m[ERROR]\033[0m sendMessage(photo) failed: " << err.message_ << "\n";
            return 0;
        }

        // Track our message ID to avoid matching other messages' events
        int64_t pending_msg_id = 0;
        if (result->get_id() == td_api::message::ID) {
            pending_msg_id = static_cast<td_api::message&>(*result).id_;
        }

        // Wait for send to complete
        bool should_retry = false;
        for (int i = 0; i < 600; i++) {
            auto response = client_manager_.receive(0.1);
            if (!response.object) continue;
            if (response.request_id == 0) {
                if (response.object->get_id() == td_api::updateMessageSendSucceeded::ID) {
                    auto& upd = static_cast<td_api::updateMessageSendSucceeded&>(*response.object);
                    if (pending_msg_id != 0 && upd.old_message_id_ != pending_msg_id) {
                        process_update(std::move(response.object));
                        continue;
                    }
                    std::cout << "[INFO] Photo sent: message ID " << upd.message_->id_ << "\n";
                    return upd.message_->id_;
                }
                if (response.object->get_id() == td_api::updateMessageSendFailed::ID) {
                    auto& upd = static_cast<td_api::updateMessageSendFailed&>(*response.object);
                    if (pending_msg_id != 0 && upd.old_message_id_ != pending_msg_id) {
                        process_update(std::move(response.object));
                        continue;
                    }
                    std::string err_msg = upd.error_ ? upd.error_->message_ : "unknown error";
                    int wait = parse_retry_after(err_msg);
                    if (wait > 0) {
                        handle_flood_wait(wait, "Photo: ");
                        should_retry = true;
                        break;
                    }
                    std::cerr << "\033[31m[ERROR]\033[0m Photo send failed: " << err_msg << "\n";
                    return 0;
                }
                process_update(std::move(response.object));
            }
        }
        if (!should_retry) return 0;
    }
}

// ---- Close ----

void TdClient::close() {
    if (closed_) return;
    send_sync(td_api::make_object<td_api::close>(), 5.0);
    for (int i = 0; i < 100; i++) {
        auto response = client_manager_.receive(0.5);
        if (!response.object) break;
        if (response.request_id == 0) {
            process_update(std::move(response.object));
        }
        if (closed_) break;
    }
    closed_ = true;
}
