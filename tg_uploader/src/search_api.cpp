#include "search_api.h"
#include "config.h"
#include "jwt_verify.h"
#include "user_auth.h"
#include "../include/httplib.h"
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <variant>
#include <vector>

using json = nlohmann::json;
using httplib::Request;
using httplib::Response;

// Private channel: -1001810923743 → t.me/c/1810923743/{url_id}
static const char* CHANNEL_ID_BARE = "1810923743";

static const char* RESOLUTION_CASE =
    "CASE"
    " WHEN height >= 2160 THEN '4K'"
    " WHEN height >= 1080 THEN '1080p'"
    " WHEN height >= 720 THEN '720p'"
    " WHEN height >= 480 THEN '480p'"
    " WHEN height > 0 THEN 'Other'"
    " ELSE 'Unknown' END";

static const std::map<std::string, std::string> SORT_OPTIONS = {
    {"date_desc", "v.record_date DESC, v.record_time DESC"},
    {"date_asc",  "v.record_date ASC, v.record_time ASC"},
    {"duration_desc", "v.duration DESC"},
    {"size_desc", "v.file_size DESC"},
};

// ===================== small helpers =====================

namespace {

std::string db_path()    { return get_base_dir() + "/data/scanner.db"; }
std::string thumbs_dir() { return get_base_dir() + "/data/thumbs"; }

// Read-only scanner.db connection, opened per request (low-concurrency browse
// tool; WAL means readers never block). RAII close.
struct DB {
    sqlite3* db = nullptr;
    DB() {
        if (sqlite3_open_v2(db_path().c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
            if (db) { sqlite3_close(db); db = nullptr; }
        } else {
            sqlite3_busy_timeout(db, 5000);
        }
    }
    ~DB() { if (db) sqlite3_close(db); }
    explicit operator bool() const { return db != nullptr; }
};

using Bind = std::variant<int64_t, std::string>;

void bind_all(sqlite3_stmt* st, const std::vector<Bind>& ps) {
    for (size_t i = 0; i < ps.size(); ++i) {
        if (std::holds_alternative<int64_t>(ps[i]))
            sqlite3_bind_int64(st, static_cast<int>(i + 1), std::get<int64_t>(ps[i]));
        else {
            const std::string& s = std::get<std::string>(ps[i]);
            sqlite3_bind_text(st, static_cast<int>(i + 1), s.c_str(), -1, SQLITE_TRANSIENT);
        }
    }
}

std::string col_text(sqlite3_stmt* st, int i) {
    const unsigned char* t = sqlite3_column_text(st, i);
    return t ? reinterpret_cast<const char*>(t) : "";
}

bool sauth(const Request& req) {
    std::string tok;
    auto h = req.get_header_value("Authorization");
    if (h.rfind("Bearer ", 0) == 0) tok = h.substr(7);
    else if (req.has_param("token")) tok = req.get_param_value("token");
    if (tok.empty()) return false;
    return jwt_verify_hs256(tok);
}
void unauth(Response& res) {
    res.status = 401;
    res.set_content("{\"detail\":\"Unauthorized\"}", "application/json");
}
void send_json(Response& res, const json& j) {
    res.set_content(j.dump(), "application/json");
}

std::string qstr(const Request& r, const char* k) {
    return r.has_param(k) ? r.get_param_value(k) : "";
}
int qint(const Request& r, const char* k, int def) {
    if (!r.has_param(k)) return def;
    try { return std::stoi(r.get_param_value(k)); } catch (...) { return def; }
}

bool serve_thumb(Response& res, const std::string& url_id, const char* cache) {
    // url_id is matched as digits, so no path traversal risk.
    std::string path = thumbs_dir() + "/" + url_id + ".jpg";
    std::ifstream f(path, std::ios::binary);
    if (!f) { res.status = 404; res.set_content("{\"detail\":\"Not found\"}", "application/json"); return false; }
    std::ostringstream ss; ss << f.rdbuf();
    res.set_header("Cache-Control", cache);
    res.set_content(ss.str(), "image/jpeg");
    return true;
}

int64_t file_size_of(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return 0;
    return static_cast<int64_t>(f.tellg());
}

// ===================== libcurl (live status) =====================

size_t curl_wcb(char* p, size_t s, size_t n, void* u) {
    static_cast<std::string*>(u)->append(p, s * n);
    return s * n;
}

std::string http_get(const std::string& url, const std::string& referer, long timeout_s) {
    CURL* c = curl_easy_init();
    if (!c) return "";
    std::string body;
    struct curl_slist* hdr = nullptr;
    hdr = curl_slist_append(hdr, "User-Agent: Mozilla/5.0 (X11; Linux x86_64) "
                                 "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36");
    hdr = curl_slist_append(hdr, "Accept: application/json, text/javascript, */*; q=0.01");
    hdr = curl_slist_append(hdr, "X-Requested-With: XMLHttpRequest");
    std::string ref = "Referer: " + referer;
    hdr = curl_slist_append(hdr, ref.c_str());

    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdr);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_wcb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, timeout_s);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, timeout_s);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);   // safe timeouts in worker threads

    CURLcode rc = curl_easy_perform(c);
    curl_slist_free_all(hdr);
    curl_easy_cleanup(c);
    return rc == CURLE_OK ? body : std::string();
}

// On-demand live cache. Background-refreshed only while /api/live is being hit,
// at most once per 90s. Idle (page closed) ⇒ zero external requests ⇒ zero CPU.
std::mutex          g_live_mu;
json                g_live_models = json::array();
std::atomic<bool>   g_live_refreshing{false};
std::atomic<int64_t> g_live_last{0};

std::optional<json> check_model_live(const std::string& model) {
    std::string body = http_get("https://chaturbate.com/api/biocontext/" + model + "/",
                                "https://chaturbate.com/" + model + "/", 4);
    if (body.empty()) return std::nullopt;
    try {
        json d = json::parse(body);
        if (!d.is_object()) return std::nullopt;
        if (d.contains("status") && d["status"].is_number() && d["status"].get<int>() != 200)
            return std::nullopt;
        std::string rs = d.value("room_status", "");
        if (rs == "public" || rs == "private" || rs == "hidden") {
            int fc = 0;
            if (d.contains("follower_count") && d["follower_count"].is_number())
                fc = d["follower_count"].get<int>();
            return json{{"model_name", model}, {"room_status", rs}, {"follower_count", fc}};
        }
    } catch (...) {}
    return std::nullopt;
}

void refresh_live() {
    std::vector<std::string> models;
    {
        DB db;
        if (db) {
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(db.db,
                    "SELECT DISTINCT model_name FROM videos WHERE model_name NOT LIKE '.%'",
                    -1, &st, nullptr) == SQLITE_OK) {
                while (sqlite3_step(st) == SQLITE_ROW) models.push_back(col_text(st, 0));
            }
            sqlite3_finalize(st);
        }
    }

    std::vector<json> live;
    std::mutex lm;
    std::atomic<size_t> idx{0};
    const int N = 48;
    std::vector<std::thread> workers;
    workers.reserve(N);
    for (int w = 0; w < N; ++w) {
        workers.emplace_back([&] {
            for (;;) {
                size_t i = idx.fetch_add(1);
                if (i >= models.size()) break;
                auto r = check_model_live(models[i]);
                if (r) { std::lock_guard<std::mutex> lk(lm); live.push_back(*r); }
            }
        });
    }
    for (auto& t : workers) t.join();

    std::sort(live.begin(), live.end(), [](const json& a, const json& b) {
        return a.value("follower_count", 0) > b.value("follower_count", 0);
    });
    json arr = json::array();
    for (auto& x : live) arr.push_back(x);
    { std::lock_guard<std::mutex> lk(g_live_mu); g_live_models = arr; }
    g_live_last.store(static_cast<int64_t>(std::time(nullptr)));
}

// ===================== CSV =====================

std::string csv_field(const std::string& s) {
    bool needq = s.find_first_of(",\"\n\r") != std::string::npos;
    if (!needq) return s;
    std::string out = "\"";
    for (char c : s) { if (c == '"') out += '"'; out += c; }
    out += '"';
    return out;
}

// Builds the leaderboard WHERE clause shared by /leaderboard and /export.
std::string leaderboard_where(const Request& req, std::vector<Bind>& params) {
    std::vector<std::string> conds;
    std::string resolution = qstr(req, "resolution");
    std::string period     = qstr(req, "period");
    if (!resolution.empty()) {
        conds.push_back(std::string(RESOLUTION_CASE) + " = ?");
        params.emplace_back(resolution);
    }
    if (!period.empty()) {
        int days = (period == "7d") ? 7 : (period == "30d") ? 30 : 0;
        if (days) {
            conds.push_back("uploaded_at > ?");
            params.emplace_back(static_cast<int64_t>(std::time(nullptr)) - int64_t(days) * 86400);
        }
    }
    if (conds.empty()) return "";
    std::string w = "WHERE ";
    for (size_t i = 0; i < conds.size(); ++i) { if (i) w += " AND "; w += conds[i]; }
    return w;
}

}  // namespace

// ===================== routes =====================

void register_search_routes(httplib::Server& svr) {
    curl_global_init(CURL_GLOBAL_DEFAULT);

    // ---- login (index.html posts here; returns {token}) ----
    svr.Post("/api/login", [](const Request& req, Response& res) {
        json body;
        try { body = json::parse(req.body); }
        catch (...) { res.status = 400; res.set_content("{\"detail\":\"Invalid JSON\"}", "application/json"); return; }
        std::string u = body.value("username", "");
        std::string p = body.value("password", "");
        if (!user_auth_verify(u, p)) {
            res.status = 401;
            res.set_content("{\"detail\":\"Invalid credentials\"}", "application/json");
            return;
        }
        send_json(res, json{{"token", user_auth_sign_token(u, 24)}});
    });

    // ---- video search ----
    svr.Get("/api/videos", [](const Request& req, Response& res) {
        if (!sauth(req)) return unauth(res);
        DB db; if (!db) { res.status = 500; return; }

        int per_page = qint(req, "per_page", 50);
        if (per_page > 200) per_page = 200;
        if (per_page < 1) per_page = 1;
        int page = qint(req, "page", 1);
        if (page < 1) page = 1;
        int offset = (page - 1) * per_page;
        auto it = SORT_OPTIONS.find(qstr(req, "sort"));
        std::string order_by = (it != SORT_OPTIONS.end()) ? it->second : SORT_OPTIONS.at("date_desc");

        std::vector<std::string> conds;
        std::vector<Bind> params;

        std::string q = qstr(req, "q");
        if (!q.empty()) {
            std::string fts;
            for (char c : q) if (c != '"') fts += c;          // strip quotes
            // trim surrounding whitespace
            size_t a = fts.find_first_not_of(" \t\n\r");
            size_t b = fts.find_last_not_of(" \t\n\r");
            fts = (a == std::string::npos) ? "" : fts.substr(a, b - a + 1);
            fts += "*";
            conds.push_back("v.message_id IN (SELECT rowid FROM videos_fts WHERE videos_fts MATCH ?)");
            params.emplace_back(fts);
        }
        std::string platform = qstr(req, "platform");
        if (!platform.empty()) { conds.push_back("v.platform = ?"); params.emplace_back(platform); }
        std::string date_from = qstr(req, "date_from");
        if (!date_from.empty()) { conds.push_back("v.record_date >= ?"); params.emplace_back(date_from); }
        std::string date_to = qstr(req, "date_to");
        if (!date_to.empty()) { conds.push_back("v.record_date <= ?"); params.emplace_back(date_to); }
        int dmin = qint(req, "duration_min", 0);
        if (dmin > 0) { conds.push_back("v.duration >= ?"); params.emplace_back((int64_t)dmin); }
        int dmax = qint(req, "duration_max", 0);
        if (dmax > 0) { conds.push_back("v.duration <= ?"); params.emplace_back((int64_t)dmax); }
        std::string tag = qstr(req, "tag");
        if (!tag.empty()) {
            conds.push_back("v.model_name IN (SELECT model_name FROM model_profile "
                            "WHERE tags = ? OR tags LIKE ? OR tags LIKE ? OR tags LIKE ?)");
            params.emplace_back(tag);
            params.emplace_back(tag + ",%");
            params.emplace_back("%," + tag + ",%");
            params.emplace_back("%," + tag);
        }

        std::string where = "1=1";
        if (!conds.empty()) {
            where.clear();
            for (size_t i = 0; i < conds.size(); ++i) { if (i) where += " AND "; where += conds[i]; }
        }

        // count
        int64_t total = 0;
        {
            std::string sql = "SELECT COUNT(*) FROM videos v WHERE " + where;
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(db.db, sql.c_str(), -1, &st, nullptr) == SQLITE_OK) {
                bind_all(st, params);
                if (sqlite3_step(st) == SQLITE_ROW) total = sqlite3_column_int64(st, 0);
            }
            sqlite3_finalize(st);
        }

        // page
        json items = json::array();
        {
            std::string sql =
                "SELECT v.message_id, v.url_id, v.file_name, v.model_name, v.platform, "
                "v.record_date, v.record_time, v.duration, v.width, v.height, v.file_size, "
                "v.thumb_path, v.caption_text, v.uploaded_at, v.tags, "
                "COALESCE(mp.tags,'') as model_tags, COALESCE(mp.room_status,'') as room_status "
                "FROM videos v "
                "LEFT JOIN model_profile mp ON mp.model_name = v.model_name COLLATE NOCASE "
                "WHERE " + where + " ORDER BY " + order_by + " LIMIT ? OFFSET ?";
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(db.db, sql.c_str(), -1, &st, nullptr) == SQLITE_OK) {
                auto pp = params;
                pp.emplace_back((int64_t)per_page);
                pp.emplace_back((int64_t)offset);
                bind_all(st, pp);
                while (sqlite3_step(st) == SQLITE_ROW) {
                    int64_t url_id = sqlite3_column_int64(st, 1);
                    std::string thumb_path = col_text(st, 11);
                    int64_t thumb_sz = 0;
                    if (!thumb_path.empty())
                        thumb_sz = file_size_of(thumbs_dir() + "/" + std::to_string(url_id) + ".jpg");
                    items.push_back(json{
                        {"message_id", sqlite3_column_int64(st, 0)},
                        {"url_id", url_id},
                        {"file_name", col_text(st, 2)},
                        {"model_name", col_text(st, 3)},
                        {"platform", col_text(st, 4)},
                        {"record_date", col_text(st, 5)},
                        {"record_time", col_text(st, 6)},
                        {"duration", sqlite3_column_int(st, 7)},
                        {"width", sqlite3_column_int(st, 8)},
                        {"height", sqlite3_column_int(st, 9)},
                        {"file_size", sqlite3_column_int64(st, 10)},
                        {"thumb_path", thumb_path},
                        {"caption_text", col_text(st, 12)},
                        {"uploaded_at", sqlite3_column_int64(st, 13)},
                        {"tags", col_text(st, 14)},
                        {"model_tags", col_text(st, 15)},
                        {"room_status", col_text(st, 16)},
                        {"tg_link", std::string("https://t.me/c/") + CHANNEL_ID_BARE + "/" + std::to_string(url_id)},
                        {"thumb_hd", thumb_sz >= 100000},
                    });
                }
            }
            sqlite3_finalize(st);
        }

        send_json(res, json{
            {"total", total}, {"page", page}, {"per_page", per_page},
            {"pages", per_page > 0 ? (total + per_page - 1) / per_page : 0},
            {"items", items},
        });
    });

    // ---- models ----
    svr.Get("/api/models", [](const Request& req, Response& res) {
        if (!sauth(req)) return unauth(res);
        DB db; if (!db) { res.status = 500; return; }
        std::string q = qstr(req, "q");
        json out = json::array();
        sqlite3_stmt* st = nullptr;
        std::string sql = q.empty()
            ? "SELECT model_name, COUNT(*) c FROM videos GROUP BY model_name COLLATE NOCASE ORDER BY c DESC LIMIT 100"
            : "SELECT model_name, COUNT(*) c FROM videos WHERE model_name LIKE ? GROUP BY model_name COLLATE NOCASE ORDER BY c DESC LIMIT 100";
        if (sqlite3_prepare_v2(db.db, sql.c_str(), -1, &st, nullptr) == SQLITE_OK) {
            if (!q.empty()) { std::string like = q + "%"; sqlite3_bind_text(st, 1, like.c_str(), -1, SQLITE_TRANSIENT); }
            while (sqlite3_step(st) == SQLITE_ROW)
                out.push_back(json{{"model_name", col_text(st, 0)}, {"count", sqlite3_column_int(st, 1)}});
        }
        sqlite3_finalize(st);
        send_json(res, out);
    });

    // ---- platforms ----
    svr.Get("/api/platforms", [](const Request& req, Response& res) {
        if (!sauth(req)) return unauth(res);
        DB db; if (!db) { res.status = 500; return; }
        json out = json::array();
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db.db,
                "SELECT platform, COUNT(*) c FROM videos WHERE platform != '' GROUP BY platform ORDER BY c DESC",
                -1, &st, nullptr) == SQLITE_OK) {
            while (sqlite3_step(st) == SQLITE_ROW)
                out.push_back(json{{"platform", col_text(st, 0)}, {"count", sqlite3_column_int(st, 1)}});
        }
        sqlite3_finalize(st);
        send_json(res, out);
    });

    // ---- tags ----
    svr.Get("/api/tags", [](const Request& req, Response& res) {
        if (!sauth(req)) return unauth(res);
        DB db; if (!db) { res.status = 500; return; }
        int limit = qint(req, "limit", 100);
        std::map<std::string, int> counts;
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db.db,
                "SELECT tags FROM model_profile WHERE tags IS NOT NULL AND tags != ''",
                -1, &st, nullptr) == SQLITE_OK) {
            while (sqlite3_step(st) == SQLITE_ROW) {
                std::string s = col_text(st, 0), cur;
                std::stringstream ss(s);
                while (std::getline(ss, cur, ',')) {
                    size_t a = cur.find_first_not_of(" \t");
                    size_t b = cur.find_last_not_of(" \t");
                    if (a != std::string::npos) counts[cur.substr(a, b - a + 1)]++;
                }
            }
        }
        sqlite3_finalize(st);
        std::vector<std::pair<std::string, int>> v(counts.begin(), counts.end());
        std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
        json out = json::array();
        for (int i = 0; i < (int)v.size() && i < limit; ++i)
            out.push_back(json{{"tag", v[i].first}, {"count", v[i].second}});
        send_json(res, out);
    });

    // ---- live (on-demand + 90s cache) ----
    svr.Get("/api/live", [](const Request& req, Response& res) {
        if (!sauth(req)) return unauth(res);
        int64_t now = std::time(nullptr);
        if (!g_live_refreshing.load() && now - g_live_last.load() >= 90) {
            bool expected = false;
            if (g_live_refreshing.compare_exchange_strong(expected, true)) {
                std::thread([] { refresh_live(); g_live_refreshing.store(false); }).detach();
            }
        }
        json out;
        { std::lock_guard<std::mutex> lk(g_live_mu); out["models"] = g_live_models; }
        out["refreshing"] = g_live_refreshing.load();
        send_json(res, out);
    });

    // ---- live room stream ----
    svr.Get(R"(/api/live/room/(.+))", [](const Request& req, Response& res) {
        if (!sauth(req)) return unauth(res);
        std::string model = req.matches[1];
        std::string body = http_get("https://chaturbate.com/api/chatvideocontext/" + model + "/",
                                    "https://chaturbate.com/" + model + "/", 8);
        if (body.empty()) { res.status = 502; res.set_content("{\"detail\":\"fetch failed\"}", "application/json"); return; }
        try {
            json d = json::parse(body);
            if (!d.is_object() || d.value("hls_source", "").empty()) {
                res.status = 404; res.set_content("{\"detail\":\"Model offline or no stream\"}", "application/json"); return;
            }
            send_json(res, json{
                {"model_name", model},
                {"hls_source", d.value("hls_source", "")},
                {"room_title", d.value("room_title", "")},
                {"room_status", d.value("room_status", "")},
                {"num_viewers", d.value("num_viewers", 0)},
                {"is_widescreen", d.value("is_widescreen", true)},
            });
        } catch (...) { res.status = 502; res.set_content("{\"detail\":\"parse error\"}", "application/json"); }
    });

    // ---- stats ----
    svr.Get("/api/stats", [](const Request& req, Response& res) {
        if (!sauth(req)) return unauth(res);
        DB db; if (!db) { res.status = 500; return; }
        auto scalar_i = [&](const char* sql, const std::vector<Bind>& ps = {}) -> int64_t {
            sqlite3_stmt* st = nullptr; int64_t v = 0;
            if (sqlite3_prepare_v2(db.db, sql, -1, &st, nullptr) == SQLITE_OK) {
                bind_all(st, ps);
                if (sqlite3_step(st) == SQLITE_ROW) v = sqlite3_column_int64(st, 0);
            }
            sqlite3_finalize(st); return v;
        };
        int64_t now = std::time(nullptr);
        std::string df, dt;
        {
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(db.db,
                    "SELECT MIN(record_date), MAX(record_date) FROM videos WHERE record_date != ''",
                    -1, &st, nullptr) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW) {
                df = col_text(st, 0); dt = col_text(st, 1);
            }
            sqlite3_finalize(st);
        }
        send_json(res, json{
            {"total_videos", scalar_i("SELECT COUNT(*) FROM videos")},
            {"total_models", scalar_i("SELECT COUNT(DISTINCT model_name) FROM videos")},
            {"total_size", scalar_i("SELECT COALESCE(SUM(file_size),0) FROM videos")},
            {"total_duration", scalar_i("SELECT COALESCE(SUM(duration),0) FROM videos")},
            {"date_from", df}, {"date_to", dt},
            {"recent_24h", scalar_i("SELECT COUNT(*) FROM videos WHERE uploaded_at > ?", {now - 86400})},
            {"recent_7d", scalar_i("SELECT COUNT(*) FROM videos WHERE uploaded_at > ?", {now - 86400 * 7})},
        });
    });

    // ---- dashboard: daily-trend ----
    svr.Get("/api/dashboard/daily-trend", [](const Request& req, Response& res) {
        if (!sauth(req)) return unauth(res);
        DB db; if (!db) { res.status = 500; return; }
        int days = qint(req, "days", 30); if (days > 90) days = 90;
        int64_t cutoff = (int64_t)std::time(nullptr) - (int64_t)days * 86400;
        json out = json::array();
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db.db,
                "SELECT DATE(uploaded_at,'unixepoch') day, COUNT(*) c FROM videos "
                "WHERE uploaded_at > ? GROUP BY day ORDER BY day", -1, &st, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, cutoff);
            while (sqlite3_step(st) == SQLITE_ROW) {
                std::string d = col_text(st, 0);
                if (!d.empty()) out.push_back(json{{"date", d}, {"count", sqlite3_column_int(st, 1)}});
            }
        }
        sqlite3_finalize(st);
        send_json(res, out);
    });

    // ---- dashboard: hourly-heatmap ----
    svr.Get("/api/dashboard/hourly-heatmap", [](const Request& req, Response& res) {
        if (!sauth(req)) return unauth(res);
        DB db; if (!db) { res.status = 500; return; }
        std::vector<std::vector<int>> matrix(7, std::vector<int>(24, 0));
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db.db,
                "SELECT CAST(SUBSTR(record_time,1,2) AS INTEGER) hour, "
                "CAST(STRFTIME('%w', record_date) AS INTEGER) dow, COUNT(*) c FROM videos "
                "WHERE record_time != '' AND record_date != '' AND STRFTIME('%w', record_date) IS NOT NULL "
                "GROUP BY hour, dow", -1, &st, nullptr) == SQLITE_OK) {
            while (sqlite3_step(st) == SQLITE_ROW) {
                int hour = sqlite3_column_int(st, 0), dow = sqlite3_column_int(st, 1), c = sqlite3_column_int(st, 2);
                if (dow >= 0 && dow <= 6 && hour >= 0 && hour <= 23) matrix[dow][hour] = c;
            }
        }
        sqlite3_finalize(st);
        send_json(res, json{{"matrix", matrix}});
    });

    // ---- dashboard: platform-stats ----
    svr.Get("/api/dashboard/platform-stats", [](const Request& req, Response& res) {
        if (!sauth(req)) return unauth(res);
        DB db; if (!db) { res.status = 500; return; }
        json out = json::array();
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db.db,
                "SELECT platform, COUNT(*) vc, SUM(file_size) ts, SUM(duration) td FROM videos "
                "WHERE platform != '' GROUP BY platform ORDER BY vc DESC", -1, &st, nullptr) == SQLITE_OK) {
            while (sqlite3_step(st) == SQLITE_ROW)
                out.push_back(json{
                    {"platform", col_text(st, 0)}, {"video_count", sqlite3_column_int(st, 1)},
                    {"total_size", sqlite3_column_int64(st, 2)}, {"total_duration", sqlite3_column_int64(st, 3)}});
        }
        sqlite3_finalize(st);
        send_json(res, out);
    });

    // ---- dashboard: resolution-stats ----
    svr.Get("/api/dashboard/resolution-stats", [](const Request& req, Response& res) {
        if (!sauth(req)) return unauth(res);
        DB db; if (!db) { res.status = 500; return; }
        json out = json::array();
        std::string sql = std::string("SELECT ") + RESOLUTION_CASE +
            " res, COUNT(*) vc, SUM(file_size) ts, SUM(duration) td, COUNT(DISTINCT model_name) mc "
            "FROM videos GROUP BY res ORDER BY vc DESC";
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db.db, sql.c_str(), -1, &st, nullptr) == SQLITE_OK) {
            while (sqlite3_step(st) == SQLITE_ROW)
                out.push_back(json{
                    {"resolution", col_text(st, 0)}, {"video_count", sqlite3_column_int(st, 1)},
                    {"total_size", sqlite3_column_int64(st, 2)}, {"total_duration", sqlite3_column_int64(st, 3)},
                    {"model_count", sqlite3_column_int(st, 4)}});
        }
        sqlite3_finalize(st);
        send_json(res, out);
    });

    // ---- leaderboard ----
    svr.Get("/api/leaderboard", [](const Request& req, Response& res) {
        if (!sauth(req)) return unauth(res);
        DB db; if (!db) { res.status = 500; return; }
        int per_page = qint(req, "per_page", 50); if (per_page > 200) per_page = 200; if (per_page < 1) per_page = 1;
        int page = qint(req, "page", 1); if (page < 1) page = 1;
        int offset = (page - 1) * per_page;
        std::string order = (qstr(req, "sort") == "duration") ? "total_duration DESC" : "video_count DESC";
        std::vector<Bind> params;
        std::string where = leaderboard_where(req, params);

        int64_t total = 0;
        {
            std::string sql = "SELECT COUNT(DISTINCT model_name) FROM videos " + where;
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(db.db, sql.c_str(), -1, &st, nullptr) == SQLITE_OK) {
                bind_all(st, params);
                if (sqlite3_step(st) == SQLITE_ROW) total = sqlite3_column_int64(st, 0);
            }
            sqlite3_finalize(st);
        }
        json items = json::array();
        {
            std::string sql =
                "SELECT model_name, COUNT(*) video_count, SUM(duration) total_duration, "
                "CAST(AVG(duration) AS INTEGER) ad, SUM(file_size) ts, "
                "MIN(record_date) fd, MAX(record_date) ld FROM videos " + where +
                " GROUP BY model_name COLLATE NOCASE ORDER BY " + order + " LIMIT ? OFFSET ?";
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(db.db, sql.c_str(), -1, &st, nullptr) == SQLITE_OK) {
                auto pp = params; pp.emplace_back((int64_t)per_page); pp.emplace_back((int64_t)offset);
                bind_all(st, pp);
                int i = 0;
                while (sqlite3_step(st) == SQLITE_ROW) {
                    items.push_back(json{
                        {"rank", offset + i + 1}, {"model_name", col_text(st, 0)},
                        {"video_count", sqlite3_column_int(st, 1)}, {"total_duration", sqlite3_column_int64(st, 2)},
                        {"avg_duration", sqlite3_column_int64(st, 3)}, {"total_size", sqlite3_column_int64(st, 4)},
                        {"first_date", col_text(st, 5)}, {"last_date", col_text(st, 6)}});
                    ++i;
                }
            }
            sqlite3_finalize(st);
        }
        send_json(res, json{
            {"total", total}, {"page", page}, {"per_page", per_page},
            {"pages", per_page > 0 ? (total + per_page - 1) / per_page : 0}, {"items", items}});
    });

    // ---- leaderboard CSV export ----
    svr.Get("/api/leaderboard/export", [](const Request& req, Response& res) {
        if (!sauth(req)) return unauth(res);
        DB db; if (!db) { res.status = 500; return; }
        std::string order = (qstr(req, "sort") == "duration") ? "total_duration DESC" : "video_count DESC";
        std::vector<Bind> params;
        std::string where = leaderboard_where(req, params);
        std::string sql =
            "SELECT model_name, COUNT(*) video_count, SUM(duration) total_duration, "
            "CAST(AVG(duration) AS INTEGER) ad, SUM(file_size) ts, "
            "MIN(record_date) fd, MAX(record_date) ld FROM videos " + where +
            " GROUP BY model_name COLLATE NOCASE ORDER BY " + order;
        std::string csv = "Rank,Model,Videos,Total Duration (s),Avg Duration (s),Total Size (bytes),First Date,Last Date\r\n";
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db.db, sql.c_str(), -1, &st, nullptr) == SQLITE_OK) {
            bind_all(st, params);
            int i = 0;
            while (sqlite3_step(st) == SQLITE_ROW) {
                csv += std::to_string(i + 1) + "," + csv_field(col_text(st, 0)) + "," +
                       std::to_string(sqlite3_column_int(st, 1)) + "," +
                       std::to_string(sqlite3_column_int64(st, 2)) + "," +
                       std::to_string(sqlite3_column_int64(st, 3)) + "," +
                       std::to_string(sqlite3_column_int64(st, 4)) + "," +
                       csv_field(col_text(st, 5)) + "," + csv_field(col_text(st, 6)) + "\r\n";
                ++i;
            }
        }
        sqlite3_finalize(st);
        res.set_header("Content-Disposition", "attachment; filename=leaderboard.csv");
        res.set_content(csv, "text/csv");
    });

    // ---- resolutions ----
    svr.Get("/api/resolutions", [](const Request& req, Response& res) {
        if (!sauth(req)) return unauth(res);
        DB db; if (!db) { res.status = 500; return; }
        std::string sql = std::string("SELECT ") + RESOLUTION_CASE +
            " res, COUNT(*) vc, COUNT(DISTINCT model_name) mc FROM videos GROUP BY res ORDER BY vc DESC";
        json out = json::array();
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db.db, sql.c_str(), -1, &st, nullptr) == SQLITE_OK) {
            while (sqlite3_step(st) == SQLITE_ROW)
                out.push_back(json{{"resolution", col_text(st, 0)},
                    {"video_count", sqlite3_column_int(st, 1)}, {"model_count", sqlite3_column_int(st, 2)}});
        }
        sqlite3_finalize(st);
        send_json(res, out);
    });

    // ---- splash (public) ----
    svr.Get("/api/splash", [](const Request&, Response& res) {
        DB db; if (!db) { res.status = 500; return; }
        json out = json::array();
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db.db,
                "SELECT url_id FROM videos WHERE thumb_path > '' ORDER BY RANDOM() LIMIT 60",
                -1, &st, nullptr) == SQLITE_OK) {
            while (sqlite3_step(st) == SQLITE_ROW) out.push_back(sqlite3_column_int64(st, 0));
        }
        sqlite3_finalize(st);
        send_json(res, out);
    });

    // ---- splash-stats (public) ----
    svr.Get("/api/splash-stats", [](const Request&, Response& res) {
        DB db; if (!db) { res.status = 500; return; }
        auto sc = [&](const char* sql) -> int64_t {
            sqlite3_stmt* st = nullptr; int64_t v = 0;
            if (sqlite3_prepare_v2(db.db, sql, -1, &st, nullptr) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW)
                v = sqlite3_column_int64(st, 0);
            sqlite3_finalize(st); return v;
        };
        send_json(res, json{{"total", sc("SELECT COUNT(*) FROM videos")},
                            {"models", sc("SELECT COUNT(DISTINCT model_name) FROM videos")}});
    });

    // ---- splash thumb (public) ----
    svr.Get(R"(/api/splash/(\d+)\.jpg)", [](const Request& req, Response& res) {
        serve_thumb(res, req.matches[1], "public, max-age=3600");
    });

    // ---- thumb (auth via header or ?token=) ----
    svr.Get(R"(/api/thumbs/(\d+)\.jpg)", [](const Request& req, Response& res) {
        if (!sauth(req)) return unauth(res);
        serve_thumb(res, req.matches[1], "private, max-age=86400");
    });

    // ---- model resolutions ----
    svr.Get(R"(/api/model/([^/]+)/resolutions)", [](const Request& req, Response& res) {
        if (!sauth(req)) return unauth(res);
        DB db; if (!db) { res.status = 500; return; }
        std::string model = req.matches[1];
        std::string sql = std::string("SELECT ") + RESOLUTION_CASE +
            " res, COUNT(*) c FROM videos WHERE model_name = ? COLLATE NOCASE GROUP BY res ORDER BY c DESC";
        json out = json::array();
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db.db, sql.c_str(), -1, &st, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, model.c_str(), -1, SQLITE_TRANSIENT);
            while (sqlite3_step(st) == SQLITE_ROW)
                out.push_back(json{{"resolution", col_text(st, 0)}, {"count", sqlite3_column_int(st, 1)}});
        }
        sqlite3_finalize(st);
        send_json(res, out);
    });

    // ---- model detail ----
    svr.Get(R"(/api/model/([^/]+)/detail)", [](const Request& req, Response& res) {
        if (!sauth(req)) return unauth(res);
        DB db; if (!db) { res.status = 500; return; }
        std::string model = req.matches[1];

        json agg; int64_t vc = 0;
        {
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(db.db,
                    "SELECT COUNT(*) vc, SUM(duration) td, CAST(AVG(duration) AS INTEGER) ad, "
                    "SUM(file_size) ts, MIN(record_date) fd, MAX(record_date) ld "
                    "FROM videos WHERE model_name = ? COLLATE NOCASE", -1, &st, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(st, 1, model.c_str(), -1, SQLITE_TRANSIENT);
                if (sqlite3_step(st) == SQLITE_ROW) {
                    vc = sqlite3_column_int64(st, 0);
                    agg = json{
                        {"video_count", vc}, {"total_duration", sqlite3_column_int64(st, 1)},
                        {"avg_duration", sqlite3_column_int64(st, 2)}, {"total_size", sqlite3_column_int64(st, 3)},
                        {"first_date", col_text(st, 4)}, {"last_date", col_text(st, 5)}};
                }
            }
            sqlite3_finalize(st);
        }
        if (vc == 0) { res.status = 404; res.set_content("{\"detail\":\"Model not found\"}", "application/json"); return; }

        auto bind1 = [&](const char* sql) -> sqlite3_stmt* {
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(db.db, sql, -1, &st, nullptr) != SQLITE_OK) return nullptr;
            sqlite3_bind_text(st, 1, model.c_str(), -1, SQLITE_TRANSIENT);
            return st;
        };

        json plats = json::array();
        if (auto st = bind1("SELECT platform, COUNT(*) c FROM videos WHERE model_name = ? COLLATE NOCASE "
                            "AND platform != '' GROUP BY platform ORDER BY c DESC")) {
            while (sqlite3_step(st) == SQLITE_ROW)
                plats.push_back(json{{"platform", col_text(st, 0)}, {"count", sqlite3_column_int(st, 1)}});
            sqlite3_finalize(st);
        }

        json ress = json::array();
        {
            std::string sql = std::string("SELECT ") + RESOLUTION_CASE +
                " res, COUNT(*) c FROM videos WHERE model_name = ? COLLATE NOCASE GROUP BY res ORDER BY c DESC";
            if (auto st = bind1(sql.c_str())) {
                while (sqlite3_step(st) == SQLITE_ROW)
                    ress.push_back(json{{"resolution", col_text(st, 0)}, {"count", sqlite3_column_int(st, 1)}});
                sqlite3_finalize(st);
            }
        }

        std::vector<json> tl;
        if (auto st = bind1("SELECT SUBSTR(record_date,1,7) m, COUNT(*) c FROM videos "
                            "WHERE model_name = ? COLLATE NOCASE AND record_date != '' "
                            "GROUP BY m ORDER BY m DESC LIMIT 12")) {
            while (sqlite3_step(st) == SQLITE_ROW)
                tl.push_back(json{{"month", col_text(st, 0)}, {"count", sqlite3_column_int(st, 1)}});
            sqlite3_finalize(st);
        }
        std::reverse(tl.begin(), tl.end());
        json timeline = json::array(); for (auto& x : tl) timeline.push_back(x);

        json thumb_id = nullptr;
        if (auto st = bind1("SELECT url_id FROM videos WHERE model_name = ? COLLATE NOCASE "
                            "AND thumb_path != '' ORDER BY record_date DESC, record_time DESC LIMIT 1")) {
            if (sqlite3_step(st) == SQLITE_ROW) thumb_id = sqlite3_column_int64(st, 0);
            sqlite3_finalize(st);
        }

        json profile = nullptr;
        if (auto st = bind1("SELECT follower_count, location, real_name, body_type, body_decorations, "
                            "smoke_drink, display_birthday, display_age, about_me, wish_list, interested_in, "
                            "sex, subgender, room_status, languages, tags, room_subject, social_medias, "
                            "photo_sets, last_broadcast, updated_at FROM model_profile "
                            "WHERE model_name = ? COLLATE NOCASE")) {
            if (sqlite3_step(st) == SQLITE_ROW) {
                json age = sqlite3_column_type(st, 7) == SQLITE_NULL
                           ? json(nullptr) : json(sqlite3_column_int(st, 7));
                auto txt = [&](int i) { return col_text(st, i); };
                profile = json{
                    {"follower_count", sqlite3_column_int64(st, 0)},
                    {"location", txt(1)}, {"real_name", txt(2)}, {"body_type", txt(3)},
                    {"body_decorations", txt(4)}, {"smoke_drink", txt(5)}, {"display_birthday", txt(6)},
                    {"display_age", age}, {"about_me", txt(8)}, {"wish_list", txt(9)},
                    {"interested_in", txt(10)}, {"sex", txt(11)}, {"subgender", txt(12)},
                    {"room_status", txt(13)}, {"languages", txt(14)}, {"tags", txt(15)},
                    {"room_subject", txt(16)},
                    {"social_medias", sqlite3_column_type(st, 17) == SQLITE_NULL ? "[]" : txt(17)},
                    {"photo_sets", sqlite3_column_type(st, 18) == SQLITE_NULL ? "[]" : txt(18)},
                    {"last_broadcast", txt(19)}, {"updated_at", sqlite3_column_int64(st, 20)}};
            }
            sqlite3_finalize(st);
        }

        json out = agg;
        out["model_name"] = model;
        out["platforms"] = plats;
        out["resolutions"] = ress;
        out["timeline"] = timeline;
        out["thumb_url_id"] = thumb_id;
        out["profile"] = profile;
        send_json(res, out);
    });
}
