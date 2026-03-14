#include "bot_client.h"
#include "utils.h"
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <iostream>
#include <stdexcept>

using json = nlohmann::json;

struct UploadProgress {
    const std::string& prefix;
    int last_pct = -1;
};

static int progress_xfer_cb(void* clientp,
                             curl_off_t /*dltotal*/, curl_off_t /*dlnow*/,
                             curl_off_t ultotal, curl_off_t ulnow) {
    if (ultotal <= 0 || ulnow <= 0) return 0;
    auto* ctx = static_cast<UploadProgress*>(clientp);
    int pct = static_cast<int>(ulnow * 100 / ultotal);
    int rounded = (pct / 10) * 10;
    if (rounded > ctx->last_pct) {
        ctx->last_pct = rounded;
        std::cout << "[INFO] " << ctx->prefix
                  << "↑ " << format_file_size(ulnow) << " / " << format_file_size(ultotal)
                  << " (" << rounded << "%)\n";
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
                                   const std::string& progress_prefix) const {
    CURL* curl = reinterpret_cast<CURL*>(curl_);
    std::string response_body;

    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, reinterpret_cast<curl_mime*>(form));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);         // no timeout for large uploads
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L); // 30s connect timeout

    UploadProgress prog{progress_prefix};
    if (!progress_prefix.empty()) {
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_xfer_cb);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &prog);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
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
