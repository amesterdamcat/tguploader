#include "bot_client.h"
#include "utils.h"
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <atomic>
#include <iostream>
#include <stdexcept>

using json = nlohmann::json;

struct UploadProgress {
    const std::string& prefix;
    int last_pct = -1;
    bool forwarding = false;   // emitted the "buffered to relay" marker yet?
    const std::atomic<bool>* cancel = nullptr;
};

// Forward decl — extern global from web_server.cpp. We avoid pulling in
// web_server.h here to keep bot_client decoupled from the web stack.
extern std::atomic<bool> g_cancel_botupload;
extern std::atomic<bool> g_cancel_backup;

static int progress_xfer_cb(void* clientp,
                             curl_off_t /*dltotal*/, curl_off_t /*dlnow*/,
                             curl_off_t ultotal, curl_off_t ulnow) {
    // Non-zero return aborts the in-flight curl transfer. This is what
    // makes "Force Stop" actually drop the current upload instead of
    // waiting for it to finish.
    auto* ctx = static_cast<UploadProgress*>(clientp);
    if (g_cancel_botupload.load() || (ctx && ctx->cancel && ctx->cancel->load())) return 1;
    if (ultotal <= 0 || ulnow <= 0) return 0;
    if (!ctx) return 0;
    int pct = static_cast<int>(ulnow * 100 / ultotal);
    int rounded = (pct / 10) * 10;
    if (rounded > ctx->last_pct) {
        ctx->last_pct = rounded;
        std::cout << "[INFO] " << ctx->prefix
                  << "↑ " << format_file_size(ulnow) << " / " << format_file_size(ultotal)
                  << " (" << rounded << "%)\n";
        std::cout.flush();
    }
    // The bytes above only reach the LOCAL telegram-bot-api relay (127.0.0.1).
    // Once they're all buffered, curl sits here at 100% while the relay does the
    // real, slow upload to Telegram's DC — that's the egress traffic the user
    // sees. We can't observe the relay→Telegram leg, so announce the handoff and
    // let the UI switch to an indeterminate "forwarding" state + live NIC speed.
    if (ulnow >= ultotal && !ctx->forwarding) {
        ctx->forwarding = true;
        std::cout << "[INFO] " << ctx->prefix
                  << "⤴ Buffered " << format_file_size(ultotal)
                  << " to relay; forwarding to Telegram…\n";
        std::cout.flush();
    }
    return 0;
}

static size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* s = static_cast<std::string*>(userdata);
    s->append(ptr, size * nmemb);
    return size * nmemb;
}

BotClient::BotClient(const std::string& api_url, const std::string& token)
    : api_url_(api_url), token_(token), curl_(nullptr) {
    curl_ = curl_easy_init();
    if (!curl_) {
        throw std::runtime_error("Failed to initialize libcurl");
    }
}

BotClient::~BotClient() {
    if (curl_) {
        curl_easy_cleanup(reinterpret_cast<CURL*>(curl_));
    }
}

std::string BotClient::make_url(const std::string& method) const {
    return api_url_ + "/bot" + token_ + "/" + method;
}

BotApiResponse BotClient::parse_response(const std::string& body) const {
    BotApiResponse resp;
    try {
        json j = json::parse(body);
        resp.ok = j.value("ok", false);
        if (resp.ok) {
            if (j.contains("result") && j["result"].is_object()) {
                resp.message_id = j["result"].value("message_id", int64_t(0));
            }
        } else {
            resp.error = j.value("description", "unknown error");
            int error_code = j.value("error_code", 0);
            if (error_code == 429) {
                // Extract retry_after from parameters or description
                if (j.contains("parameters") && j["parameters"].is_object()) {
                    resp.retry_after = j["parameters"].value("retry_after", 60);
                } else {
                    // Fallback: parse "Too Many Requests: retry after N" from description
                    const std::string marker = "retry after ";
                    auto pos = resp.error.rfind(marker);
                    if (pos != std::string::npos) {
                        try {
                            resp.retry_after = std::stoi(resp.error.substr(pos + marker.size()));
                        } catch (...) {
                            resp.retry_after = 60;
                        }
                    } else {
                        resp.retry_after = 60;
                    }
                }
            }
        }
    } catch (const std::exception& e) {
        resp.ok = false;
        resp.error = std::string("JSON parse error: ") + e.what();
        if (body.size() <= 300) resp.error += " | body: " + body;
    }
    return resp;
}

BotApiResponse BotClient::do_post(const std::string& url, void* form,
                                   const std::string& progress_prefix,
                                   bool with_progress,
                                   long timeout_seconds,
                                   const std::atomic<bool>* cancel) const {
    CURL* curl = reinterpret_cast<CURL*>(curl_);
    std::string response_body;

    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, reinterpret_cast<curl_mime*>(form));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_seconds); // 0 = no timeout for large uploads
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L); // 30s connect timeout

    // Progress reporting — prefix is purely cosmetic (bot name tag in parallel
    // mode). Without this, single-bot uploads emitted no `↑ N MB / M MB (P%)`
    // lines, breaking the Now Playing progress bar. Tiny control calls
    // (copyMessage etc.) pass with_progress=false to avoid spurious 100% /
    // "forwarding" lines; they may still pass a cancel flag.
    UploadProgress prog{progress_prefix, -1, false, cancel};
    if (with_progress || cancel) {
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_xfer_cb);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &prog);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    } else {
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
    }

    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        BotApiResponse resp;
        resp.ok = false;
        resp.error = std::string("curl error: ") + curl_easy_strerror(rc);
        return resp;
    }

    return parse_response(response_body);
}

BotApiResponse BotClient::send_video(const std::string& chat_id,
                                      const std::string& video_path,
                                      const std::string& caption,
                                      int duration, int width, int height,
                                      const std::string& thumb_path) {
    CURL* curl = reinterpret_cast<CURL*>(curl_);
    curl_mime* form = curl_mime_init(curl);

    auto add_str = [&](const char* name, const std::string& val) {
        curl_mimepart* part = curl_mime_addpart(form);
        curl_mime_name(part, name);
        curl_mime_data(part, val.c_str(), CURL_ZERO_TERMINATED);
    };

    add_str("chat_id", chat_id);
    add_str("caption", caption);
    add_str("parse_mode", "HTML");
    add_str("duration", std::to_string(duration));
    add_str("width", std::to_string(width));
    add_str("height", std::to_string(height));
    add_str("supports_streaming", "true");

    // Upload file content via multipart (works with both --local and standard server)
    {
        curl_mimepart* part = curl_mime_addpart(form);
        curl_mime_name(part, "video");
        curl_mime_filedata(part, video_path.c_str());
    }
    if (!thumb_path.empty()) {
        curl_mimepart* part = curl_mime_addpart(form);
        curl_mime_name(part, "thumbnail");
        curl_mime_filedata(part, thumb_path.c_str());
    }

    BotApiResponse resp = do_post(make_url("sendVideo"), form, log_prefix_);
    curl_mime_free(form);
    return resp;
}

BotApiResponse BotClient::send_photo(const std::string& chat_id,
                                      const std::string& photo_path,
                                      const std::string& caption) {
    CURL* curl = reinterpret_cast<CURL*>(curl_);
    curl_mime* form = curl_mime_init(curl);

    auto add_str = [&](const char* name, const std::string& val) {
        curl_mimepart* part = curl_mime_addpart(form);
        curl_mime_name(part, name);
        curl_mime_data(part, val.c_str(), CURL_ZERO_TERMINATED);
    };

    add_str("chat_id", chat_id);
    add_str("caption", caption);
    add_str("parse_mode", "HTML");

    {
        curl_mimepart* part = curl_mime_addpart(form);
        curl_mime_name(part, "photo");
        curl_mime_filedata(part, photo_path.c_str());
    }

    BotApiResponse resp = do_post(make_url("sendPhoto"), form);
    curl_mime_free(form);
    return resp;
}

BotApiResponse BotClient::copy_message(const std::string& from_chat_id,
                                        int64_t message_id,
                                        const std::string& to_chat_id) {
    CURL* curl = reinterpret_cast<CURL*>(curl_);
    curl_mime* form = curl_mime_init(curl);

    auto add_str = [&](const char* name, const std::string& val) {
        curl_mimepart* part = curl_mime_addpart(form);
        curl_mime_name(part, name);
        curl_mime_data(part, val.c_str(), CURL_ZERO_TERMINATED);
    };
    add_str("chat_id", to_chat_id);            // destination (backup channel)
    add_str("from_chat_id", from_chat_id);     // source channel
    add_str("message_id", std::to_string(message_id));

    // No progress callback: this is a tiny server-side-copy request.
    BotApiResponse resp = do_post(make_url("copyMessage"), form, "", false, 120L, &g_cancel_backup);
    curl_mime_free(form);
    return resp;
}

std::string BotClient::raw_get(const std::string& url) const {
    CURL* curl = reinterpret_cast<CURL*>(curl_);
    std::string body;
    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        return std::string("{\"ok\":false,\"description\":\"curl error: ")
               + curl_easy_strerror(rc) + "\"}";
    }
    return body;
}

BotIdentity BotClient::get_me() {
    BotIdentity r;
    try {
        json j = json::parse(raw_get(make_url("getMe")));
        r.ok = j.value("ok", false);
        if (r.ok && j.contains("result")) {
            r.id = j["result"].value("id", int64_t(0));
            r.username = j["result"].value("username", "");
        } else {
            r.error = j.value("description", "unknown error");
        }
    } catch (const std::exception& e) { r.error = std::string("parse: ") + e.what(); }
    return r;
}

ChatAccess BotClient::get_chat(const std::string& chat_id) {
    ChatAccess r;
    try {
        json j = json::parse(raw_get(make_url("getChat") + "?chat_id=" + chat_id));
        r.ok = j.value("ok", false);
        if (r.ok && j.contains("result")) {
            r.title = j["result"].value("title", "");
            r.type  = j["result"].value("type", "");
        } else {
            r.error = j.value("description", "unknown error");
        }
    } catch (const std::exception& e) { r.error = std::string("parse: ") + e.what(); }
    return r;
}

MemberStatus BotClient::get_chat_member(const std::string& chat_id, int64_t user_id) {
    MemberStatus r;
    try {
        json j = json::parse(raw_get(make_url("getChatMember")
                                     + "?chat_id=" + chat_id
                                     + "&user_id=" + std::to_string(user_id)));
        r.ok = j.value("ok", false);
        if (r.ok && j.contains("result")) {
            r.status = j["result"].value("status", "");
            // Channel creators implicitly have all rights; admins carry the flag.
            r.can_post = (r.status == "creator") ||
                         j["result"].value("can_post_messages", false);
        } else {
            r.error = j.value("description", "unknown error");
        }
    } catch (const std::exception& e) { r.error = std::string("parse: ") + e.what(); }
    return r;
}
