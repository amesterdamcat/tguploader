#include "recorder_api.h"
#include "jwt_verify.h"
#include "../include/httplib.h"
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <openssl/hmac.h>
#include <openssl/sha.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

using json = nlohmann::json;
using httplib::Request;
using httplib::Response;
namespace fs = std::filesystem;

namespace {

// ---- where the ctbrec server lives + how to authenticate to it ----
// Defaults match the local docker deployment; overridable via env so the box
// owner can repoint without a rebuild.
std::string ctbrec_base() {
    if (const char* e = std::getenv("CTBREC_URL")) return e;
    return "https://127.0.0.1:8443";
}

// ctbrec keeps one settings dir per app version (e.g. .../config/26.3.18/) plus
// timestamped *_backup_* copies. We read the live one: the highest semantic
// version among the non-backup dirs.
std::string ctbrec_config_dir() {
    if (const char* e = std::getenv("CTBREC_CONFIG_DIR")) return e;
    const fs::path root = "/mnt/storage/ctbrec/config";
    std::vector<int> best;       // version tuple of the current pick
    fs::path best_path;
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return "";
    for (const auto& de : fs::directory_iterator(root, ec)) {
        if (!de.is_directory()) continue;
        const std::string name = de.path().filename().string();
        if (name.find("_backup") != std::string::npos) continue;
        // parse "a.b.c" → tuple of ints; skip anything that isn't pure version
        std::vector<int> ver;
        int cur = 0; bool any = false, ok = true;
        for (char c : name) {
            if (c == '.') { ver.push_back(cur); cur = 0; any = false; }
            else if (c >= '0' && c <= '9') { cur = cur * 10 + (c - '0'); any = true; }
            else { ok = false; break; }
        }
        if (!ok || !any) continue;
        ver.push_back(cur);
        if (!fs::exists(de.path() / "server.json")) continue;
        if (ver > best) { best = ver; best_path = de.path(); }
    }
    return best_path.empty() ? "" : best_path.string();
}

struct Creds { std::string user, pass; bool ok = false; };

Creds ctbrec_creds() {
    Creds c;
    if (const char* u = std::getenv("CTBREC_USER")) c.user = u;
    if (const char* p = std::getenv("CTBREC_PASS")) c.pass = p;
    if (!c.user.empty()) { c.ok = true; return c; }
    const std::string dir = ctbrec_config_dir();
    if (dir.empty()) return c;
    std::ifstream f(dir + "/server.json");
    if (!f) return c;
    try {
        json s = json::parse(f);
        c.user = s.value("webinterfaceUsername", "");
        c.pass = s.value("webinterfacePassword", "");
        c.ok   = !c.user.empty();
    } catch (...) {}
    return c;
}

// ---- libcurl plumbing ----
size_t wcb(char* p, size_t s, size_t n, void* u) {
    static_cast<std::string*>(u)->append(p, s * n);
    return s * n;
}

// HTTP call to ctbrec. `extra` headers are appended (e.g. the HMAC). For GETs
// pass body="" and method GET; for POSTs pass the JSON body.
struct HttpResult { long status = 0; std::string body; bool transport_ok = false; };

HttpResult ctbrec_http(const std::string& path, const std::string& body,
                       bool is_post, const std::vector<std::string>& extra) {
    HttpResult r;
    CURL* c = curl_easy_init();
    if (!c) return r;
    const Creds cr = ctbrec_creds();
    const std::string url = ctbrec_base() + path;

    struct curl_slist* hdr = nullptr;
    hdr = curl_slist_append(hdr, "X-Requested-With: XMLHttpRequest");
    if (is_post) hdr = curl_slist_append(hdr, "Content-Type: application/json");
    for (const auto& h : extra) hdr = curl_slist_append(hdr, h.c_str());

    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdr);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, wcb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &r.body);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    // self-signed origin cert on the loopback ctbrec server
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
    if (cr.ok) {
        curl_easy_setopt(c, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
        curl_easy_setopt(c, CURLOPT_USERNAME, cr.user.c_str());
        curl_easy_setopt(c, CURLOPT_PASSWORD, cr.pass.c_str());
    }
    if (is_post) {
        curl_easy_setopt(c, CURLOPT_POST, 1L);
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    }
    CURLcode rc = curl_easy_perform(c);
    r.transport_ok = (rc == CURLE_OK);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r.status);
    curl_slist_free_all(hdr);
    curl_easy_cleanup(c);
    return r;
}

// ---- HMAC key (fetched from /secured/hmac, cached for the process) ----
std::mutex     g_key_mu;
std::string    g_hmac_key;

std::string fetch_hmac_key() {
    HttpResult r = ctbrec_http("/secured/hmac", "", /*post=*/false, {});
    if (!r.transport_ok || r.status != 200) return "";
    try { return json::parse(r.body).value("hmac", std::string()); }
    catch (...) { return ""; }
}

std::string hmac_key(bool force_refresh = false) {
    std::lock_guard<std::mutex> lk(g_key_mu);
    if (force_refresh || g_hmac_key.empty()) g_hmac_key = fetch_hmac_key();
    return g_hmac_key;
}

std::string hmac_sha256_hex(const std::string& key, const std::string& msg) {
    unsigned char mac[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    HMAC(EVP_sha256(),
         key.data(), static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char*>(msg.data()), msg.size(),
         mac, &len);
    static const char* hexd = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (unsigned int i = 0; i < len; ++i) {
        out.push_back(hexd[mac[i] >> 4]);
        out.push_back(hexd[mac[i] & 0xF]);
    }
    return out;
}

// Sign and POST an action to /rec. Retries once with a fresh HMAC key if the
// first attempt is rejected (the key rotates when ctbrec restarts).
HttpResult ctbrec_action(const json& action) {
    const std::string body = action.dump();
    auto attempt = [&](const std::string& key) {
        return ctbrec_http("/rec", body, /*post=*/true,
                           {"CTBREC-HMAC: " + hmac_sha256_hex(key, body)});
    };
    std::string key = hmac_key();
    HttpResult r = attempt(key);
    if (r.transport_ok && (r.status == 401 || r.status == 403)) {
        key = hmac_key(/*force_refresh=*/true);
        if (!key.empty()) r = attempt(key);
    }
    return r;
}

// Forward a ctbrec response to our client verbatim when it's JSON, otherwise
// wrap the transport failure in our own error envelope.
void relay(const HttpResult& r, Response& res) {
    if (!r.transport_ok) {
        res.status = 502;
        res.set_content(json{{"status", "error"},
                             {"msg", "ctbrec unreachable"}}.dump(), "application/json");
        return;
    }
    res.status = (r.status >= 200 && r.status < 300) ? 200 : static_cast<int>(r.status);
    res.set_content(r.body.empty() ? "{}" : r.body, "application/json");
}

// ---- auth (same scheme as the rest of the panel) ----
bool rauth(const Request& req) {
    std::string tok;
    auto h = req.get_header_value("Authorization");
    if (h.rfind("Bearer ", 0) == 0) tok = h.substr(7);
    else if (req.has_param("token")) tok = req.get_param_value("token");
    return !tok.empty() && jwt_verify_hs256(tok);
}
void unauth(Response& res) {
    res.status = 401;
    res.set_content("{\"detail\":\"Unauthorized\"}", "application/json");
}

// Actions the frontend is allowed to invoke through the generic proxy. Read and
// recording-control only — config mutation lives behind a different endpoint and
// stays off-limits here.
const std::set<std::string>& allowed_actions() {
    static const std::set<std::string> a = {
        "list", "listOnline", "listCurrentlyRecording", "listModelGroups",
        "recordings", "space",
        "start", "stop", "switch", "startByName", "startByUrl",
        "suspend", "resume", "suspendRecording", "resumeRecording",
        "markForLater", "markForLaterRecording", "delete", "addModel",
        "setPriority", "setNote", "setPreferredResolution", "setRecordUntil",
        "stopAt", "setSuspended", "rerunPostProcessing",
    };
    return a;
}

// ---- recordings cache + server-side paging ----
// ctbrec's `recordings` action returns the ENTIRE list (tens of thousands of
// entries, ~30 MB) and ignores any paging/sort params — which is exactly why its
// own UI grinds to a halt. We fetch it at most once per TTL, keep the parsed
// array, and do the filter/sort/paginate here so the browser only ever receives
// one page.
std::mutex g_rec_mu;
json       g_rec_cache = json::array();
int64_t    g_rec_ts    = 0;   // unix seconds of last successful fetch
const int  REC_TTL_S   = 20;

// Map a ctbrec container media path to its host path so we can tell whether the
// recording's file still exists (this project uploads then deletes sources, so
// many metadata entries outlive their files).
std::string to_host_path(const std::string& p) {
    static const std::string cont = []{
        if (const char* e = std::getenv("CTBREC_MEDIA_CONTAINER")) return std::string(e);
        return std::string("/app/media");
    }();
    static const std::string host = []{
        if (const char* e = std::getenv("CTBREC_MEDIA_HOST")) return std::string(e);
        return std::string("/mnt/storage/ctbrec/media");
    }();
    if (p.rfind(cont, 0) == 0) return host + p.substr(cont.size());
    return p;
}

// Refresh the cache if stale. Caller must hold g_rec_mu. Returns false if we have
// no data at all (fetch failed and cache empty).
bool ensure_recordings(bool force) {
    const int64_t nows = static_cast<int64_t>(std::time(nullptr));
    if (!force && !g_rec_cache.empty() && (nows - g_rec_ts) < REC_TTL_S) return true;
    HttpResult r = ctbrec_action(json{{"action", "recordings"}});
    if (!r.transport_ok || r.status != 200) return !g_rec_cache.empty();
    try {
        json d = json::parse(r.body);
        if (d.contains("recordings") && d["recordings"].is_array()) {
            g_rec_cache = std::move(d["recordings"]);
            g_rec_ts = nows;
            return true;
        }
    } catch (...) {}
    return !g_rec_cache.empty();
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return std::tolower(c); });
    return s;
}

// True if this recording's media file is gone from disk. The selection criterion
// for cleanup is *purely* file existence — never the recording status. This
// project uploads a recording then deletes the source, so a missing file means
// it's already handled; a file still on disk means it has NOT been uploaded yet
// and must be kept (e.g. a FINISHED-but-not-yet-uploaded recording).
bool file_missing(const json& r) {
    std::string f = r.value("absoluteFile", "");
    if (f.empty()) return true;
    std::error_code ec;
    return !fs::exists(to_host_path(f), ec);
}

// ---- background jobs over the recordings set ----
// Shared shape for the long-running bulk operations (delete-by-missing-file and
// rerun-post-processing-on-WAITING). The UI polls a GET for live progress.
struct BulkJob {
    std::atomic<bool> running{false};
    std::atomic<int>  total{0};   // eligible at job start
    std::atomic<int>  done{0};    // succeeded
    std::atomic<int>  failed{0};
    std::atomic<int64_t> started_at{0};
};
BulkJob g_cleanup;     // delete recordings whose file is gone
BulkJob g_reprocess;   // rerun post-processing on WAITING recordings

// Look up the full recording object for an id in the cache. Caller need NOT hold
// the lock; this takes it. Returns null json if not found.
json find_recording(const std::string& id) {
    std::lock_guard<std::mutex> lk(g_rec_mu);
    ensure_recordings(false);
    for (const auto& r : g_rec_cache)
        if (r.value("id", "") == id) return r;
    return json();
}

std::string basename_of(const std::string& p) {
    auto pos = p.find_last_of('/');
    return pos == std::string::npos ? p : p.substr(pos + 1);
}

// Build ctbrec's signed poster-image path for a recording:
//   /image/recording/<model>/<basename>.jpg?hmac=<HMAC-SHA256(key, pathWithoutQuery)>
// The path is derived entirely from server-side cached data (never client input),
// so it can't be used to traverse to arbitrary files. Returns "" if no poster.
std::string signed_image_path(const json& rec) {
    const std::string model = rec.contains("model") && rec["model"].is_object()
                              ? rec["model"].value("name", "") : "";
    if (model.empty()) return "";
    // prefer the .jpg listed in associatedFiles; else derive from the media file
    std::string jpg;
    if (rec.contains("associatedFiles") && rec["associatedFiles"].is_array()) {
        for (const auto& f : rec["associatedFiles"]) {
            if (f.is_string()) {
                std::string s = f.get<std::string>();
                if (s.size() >= 4 && s.substr(s.size() - 4) == ".jpg") { jpg = basename_of(s); break; }
            }
        }
    }
    if (jpg.empty()) {
        std::string base = basename_of(rec.value("absoluteFile", ""));
        auto dot = base.find_last_of('.');
        if (dot != std::string::npos) base = base.substr(0, dot);
        if (base.empty()) return "";
        jpg = base + ".jpg";
    }
    const std::string path = "/image/recording/" + model + "/" + jpg;
    return path + "?hmac=" + hmac_sha256_hex(hmac_key(), path);
}

}  // namespace

void register_recorder_routes(httplib::Server& svr) {
    // Convenience GETs — no body needed for these ctbrec actions.
    auto simple = [&svr](const char* path, const char* action) {
        svr.Get(path, [action](const Request& req, Response& res) {
            if (!rauth(req)) return unauth(res);
            relay(ctbrec_action(json{{"action", action}}), res);
        });
    };
    simple("/api/rec/models",    "list");
    simple("/api/rec/online",    "listOnline");
    simple("/api/rec/recording", "listCurrentlyRecording");
    simple("/api/rec/space",     "space");

    // Add (and immediately track/record) a model by name. The headline feature.
    // body: {"name":"evangel111","site":"Chaturbate"}  (site defaults to Chaturbate)
    svr.Post("/api/rec/add", [](const Request& req, Response& res) {
        if (!rauth(req)) return unauth(res);
        json body;
        try { body = json::parse(req.body); } catch (...) {
            res.status = 400;
            res.set_content(json{{"status","error"},{"msg","invalid JSON"}}.dump(),
                            "application/json");
            return;
        }
        std::string name = body.value("name", "");
        // strip a leading @ and surrounding whitespace, common when pasting
        while (!name.empty() && (name.front() == ' ' || name.front() == '@')) name.erase(name.begin());
        while (!name.empty() && name.back() == ' ') name.pop_back();
        if (name.empty()) {
            res.status = 400;
            res.set_content(json{{"status","error"},{"msg","name required"}}.dump(),
                            "application/json");
            return;
        }
        const std::string site = body.value("site", "Chaturbate");
        json action = {
            {"action", "startByName"},
            {"model", {{"type", nullptr}, {"name", ""}, {"url", site + ":" + name}}},
        };
        relay(ctbrec_action(action), res);
    });

    // Generic signed passthrough for the remaining whitelisted actions. The
    // frontend sends a ctbrec action object (with the model/recording it got
    // from a list call); we sign and forward it.
    svr.Post("/api/rec/call", [](const Request& req, Response& res) {
        if (!rauth(req)) return unauth(res);
        json body;
        try { body = json::parse(req.body); } catch (...) {
            res.status = 400;
            res.set_content(json{{"status","error"},{"msg","invalid JSON"}}.dump(),
                            "application/json");
            return;
        }
        const std::string action = body.value("action", "");
        if (!allowed_actions().count(action)) {
            res.status = 403;
            res.set_content(json{{"status","error"},
                                 {"msg","action not allowed: " + action}}.dump(),
                            "application/json");
            return;
        }
        relay(ctbrec_action(body), res);
    });

    // Paged recordings browser. Defaults to TODAY's recordings only (the full
    // list is tens of thousands of entries). All filtering/sorting/paging happens
    // server-side against the cached list.
    //   ?date=today|all|YYYY-MM-DD  &q=  &status=  &sort=date_desc|date_asc|size_desc|size_asc
    //   &page=0  &page_size=60
    svr.Get("/api/rec/recordings", [](const Request& req, Response& res) {
        if (!rauth(req)) return unauth(res);
        const std::string date   = req.has_param("date")   ? req.get_param_value("date")   : "today";
        const std::string q      = lower(req.has_param("q") ? req.get_param_value("q") : "");
        const std::string status = req.has_param("status") ? req.get_param_value("status") : "";
        const std::string sort   = req.has_param("sort")   ? req.get_param_value("sort")   : "date_desc";
        int page      = req.has_param("page")      ? std::atoi(req.get_param_value("page").c_str())      : 0;
        int page_size = req.has_param("page_size") ? std::atoi(req.get_param_value("page_size").c_str()) : 60;
        if (page < 0) page = 0;
        page_size = std::max(1, std::min(200, page_size));

        // resolve the day window [lo, hi) in unix ms (server local time)
        int64_t lo = 0, hi = 0;  // 0,0 ⇒ no date filter (date=all)
        if (date != "all") {
            std::tm tmv{};
            std::time_t base = std::time(nullptr);
            localtime_r(&base, &tmv);
            if (date != "today") {
                // parse YYYY-MM-DD; fall back to today on malformed input
                int y=0, mo=0, dd=0;
                if (std::sscanf(date.c_str(), "%d-%d-%d", &y, &mo, &dd) == 3 && y > 1970) {
                    tmv.tm_year = y - 1900; tmv.tm_mon = mo - 1; tmv.tm_mday = dd;
                }
            }
            tmv.tm_hour = 0; tmv.tm_min = 0; tmv.tm_sec = 0; tmv.tm_isdst = -1;
            std::time_t day0 = std::mktime(&tmv);
            lo = static_cast<int64_t>(day0) * 1000;
            hi = lo + 86400LL * 1000;
        }

        std::lock_guard<std::mutex> lk(g_rec_mu);
        if (!ensure_recordings(/*force=*/false) ) {
            res.status = 502;
            res.set_content(json{{"status","error"},{"msg","ctbrec unreachable"}}.dump(),
                            "application/json");
            return;
        }

        // collect indices that pass the filters, plus status tally for the window
        std::vector<size_t> idx;
        json counts = {{"FINISHED",0},{"RECORDING",0},{"FAILED",0},{"WAITING",0},{"POST_PROCESSING",0}};
        for (size_t i = 0; i < g_rec_cache.size(); ++i) {
            const json& r = g_rec_cache[i];
            int64_t sd = r.value("startDate", (int64_t)0);
            if (lo && (sd < lo || sd >= hi)) continue;
            const std::string st = r.value("status", "");
            if (counts.contains(st)) counts[st] = counts[st].get<int>() + 1;
            if (!status.empty() && st != status) continue;
            if (!q.empty()) {
                std::string nm = lower(r.contains("model") && r["model"].contains("name")
                                       ? r["model"].value("name", "") : "");
                if (nm.find(q) == std::string::npos) continue;
            }
            idx.push_back(i);
        }

        // sort
        auto sd_of   = [&](size_t i){ return g_rec_cache[i].value("startDate", (int64_t)0); };
        auto size_of = [&](size_t i){ return g_rec_cache[i].value("sizeInByte", (int64_t)0); };
        if      (sort == "date_asc")  std::sort(idx.begin(), idx.end(), [&](size_t a,size_t b){ return sd_of(a) < sd_of(b); });
        else if (sort == "size_desc") std::sort(idx.begin(), idx.end(), [&](size_t a,size_t b){ return size_of(a) > size_of(b); });
        else if (sort == "size_asc")  std::sort(idx.begin(), idx.end(), [&](size_t a,size_t b){ return size_of(a) < size_of(b); });
        else                          std::sort(idx.begin(), idx.end(), [&](size_t a,size_t b){ return sd_of(a) > sd_of(b); }); // date_desc

        const int64_t filtered = static_cast<int64_t>(idx.size());
        const size_t start = static_cast<size_t>(page) * page_size;
        json items = json::array();
        for (size_t k = start; k < idx.size() && k < start + page_size; ++k) {
            const json& r = g_rec_cache[idx[k]];
            const json& m = r.contains("model") && r["model"].is_object() ? r["model"] : json::object();
            std::string file = r.value("absoluteFile", "");
            bool exists = false;
            if (!file.empty()) { std::error_code ec; exists = fs::exists(to_host_path(file), ec); }
            std::string base = file;
            if (auto pos = base.find_last_of('/'); pos != std::string::npos) base = base.substr(pos + 1);
            items.push_back({
                {"id",     r.value("id", "")},
                {"model",  m.value("name", "")},
                {"site",   m.contains("url") ? m.value("url","") : ""},
                {"start",  r.value("startDate", (int64_t)0)},
                {"status", r.value("status", "")},
                {"size",   r.value("sizeInByte", (int64_t)0)},
                {"res",    r.value("selectedResolution", 0)},
                {"pinned", r.value("pinned", false)},
                {"note",   r.is_object() && r.contains("note") && r["note"].is_string() ? r.value("note","") : ""},
                {"file",   base},
                {"exists", exists},
            });
        }

        json out = {
            {"status", "success"},
            {"total", static_cast<int64_t>(g_rec_cache.size())},
            {"filtered", filtered},
            {"page", page}, {"page_size", page_size},
            {"date", date}, {"counts", counts},
            {"items", items},
        };
        res.set_content(out.dump(), "application/json");
    });

    // Delete a recording by id (looks up the full object in the cache and sends
    // it to ctbrec, which needs the whole recording object).
    svr.Post("/api/rec/recording/delete", [](const Request& req, Response& res) {
        if (!rauth(req)) return unauth(res);
        json body;
        try { body = json::parse(req.body); } catch (...) {
            res.status = 400;
            res.set_content(json{{"status","error"},{"msg","invalid JSON"}}.dump(), "application/json");
            return;
        }
        const std::string id = body.value("id", "");
        json rec_obj;
        {
            std::lock_guard<std::mutex> lk(g_rec_mu);
            ensure_recordings(false);
            for (const auto& r : g_rec_cache) {
                if (r.value("id", "") == id) { rec_obj = r; break; }
            }
        }
        if (rec_obj.is_null()) {
            res.status = 404;
            res.set_content(json{{"status","error"},{"msg","recording not found"}}.dump(), "application/json");
            return;
        }
        HttpResult r = ctbrec_action(json{{"action","delete"},{"recording", rec_obj}});
        // invalidate cache so the list reflects the removal on next load
        { std::lock_guard<std::mutex> lk(g_rec_mu); g_rec_ts = 0; }
        relay(r, res);
    });

    // Cleanup preview — compare what ctbrec has on record against what's actually
    // on disk. Read-only: deletes nothing, just reports the tally so the UI can
    // show "清理已上传 (N)" and a confirm dialog. Selection is by file existence
    // only (see file_missing()), independent of recording status.
    svr.Get("/api/rec/recordings/cleanup", [](const Request& req, Response& res) {
        if (!rauth(req)) return unauth(res);
        int on_disk = 0, missing = 0, pinned_missing = 0, waiting = 0;
        int total = 0;
        {
            std::lock_guard<std::mutex> lk(g_rec_mu);
            ensure_recordings(/*force=*/false);
            total = static_cast<int>(g_rec_cache.size());
            for (const auto& r : g_rec_cache) {
                if (file_missing(r)) {
                    ++missing;
                    if (r.value("pinned", false)) ++pinned_missing;
                } else {
                    ++on_disk;
                    // WAITING with its file still on disk = stuck before/at
                    // post-processing; eligible for a rerun.
                    if (r.value("status", "") == "WAITING") ++waiting;
                }
            }
        }
        // eligible = missing files that are not pinned (pinned kept as a safety net)
        const int eligible = missing - pinned_missing;
        json out = {
            {"status", "success"},
            {"total", total},
            {"on_disk", on_disk},
            {"missing", missing},
            {"pinned_missing", pinned_missing},
            {"eligible", eligible},
            {"waiting", waiting},
            {"running", g_cleanup.running.load()},
            {"done", g_cleanup.done.load()},
            {"failed", g_cleanup.failed.load()},
            {"job_total", g_cleanup.total.load()},
            {"started_at", g_cleanup.started_at.load()},
            {"reprocess", {
                {"running", g_reprocess.running.load()},
                {"done", g_reprocess.done.load()},
                {"failed", g_reprocess.failed.load()},
                {"job_total", g_reprocess.total.load()},
            }},
        };
        res.set_content(out.dump(), "application/json");
    });

    // Run the cleanup: delete every recording whose file is missing (non-pinned).
    // Long-running (can be ~19k deletes), so it runs on a detached thread and the
    // UI polls the GET above for progress.
    svr.Post("/api/rec/recordings/cleanup", [](const Request& req, Response& res) {
        if (!rauth(req)) return unauth(res);
        bool expected = false;
        if (!g_cleanup.running.compare_exchange_strong(expected, true)) {
            res.status = 409;
            res.set_content(json{{"status","error"},{"msg","cleanup already running"}}.dump(),
                            "application/json");
            return;
        }
        // Snapshot the victims under lock, then delete outside the lock.
        std::vector<json> victims;
        {
            std::lock_guard<std::mutex> lk(g_rec_mu);
            ensure_recordings(/*force=*/true);
            for (const auto& r : g_rec_cache)
                if (file_missing(r) && !r.value("pinned", false)) victims.push_back(r);
        }
        g_cleanup.total.store(static_cast<int>(victims.size()));
        g_cleanup.done.store(0);
        g_cleanup.failed.store(0);
        g_cleanup.started_at.store(static_cast<int64_t>(std::time(nullptr)));

        std::thread([victims = std::move(victims)]() {
            for (const auto& rec : victims) {
                HttpResult r = ctbrec_action(json{{"action","delete"},{"recording", rec}});
                bool ok = false;
                if (r.transport_ok && r.status == 200) {
                    try { ok = json::parse(r.body).value("status","") == "success"; }
                    catch (...) {}
                }
                if (ok) g_cleanup.done.fetch_add(1);
                else    g_cleanup.failed.fetch_add(1);
            }
            { std::lock_guard<std::mutex> lk(g_rec_mu); g_rec_ts = 0; }  // force refresh
            std::cout << "[REC] cleanup done: deleted " << g_cleanup.done.load()
                      << ", failed " << g_cleanup.failed.load() << "\n";
            g_cleanup.running.store(false);
        }).detach();

        res.set_content(json{{"status","success"},
                             {"msg","cleanup started"},
                             {"eligible", g_cleanup.total.load()}}.dump(),
                        "application/json");
    });

    // Recording poster thumbnail proxy. The browser can't reach the loopback
    // ctbrec server, so we fetch the HMAC-signed /image/... poster on its behalf.
    // Auth via ?token= since this is loaded as an <img>. The image path is built
    // from cached server data, so the client can't request arbitrary files.
    svr.Get("/api/rec/thumb", [](const Request& req, Response& res) {
        if (!rauth(req)) return unauth(res);
        json rec = find_recording(req.has_param("id") ? req.get_param_value("id") : "");
        std::string path = rec.is_null() ? "" : signed_image_path(rec);
        if (path.empty()) { res.status = 404; res.set_content("", "image/jpeg"); return; }
        HttpResult r = ctbrec_http(path, "", /*post=*/false, {});
        if (!r.transport_ok || r.status != 200 || r.body.empty()) {
            res.status = r.status >= 400 ? static_cast<int>(r.status) : 404;
            res.set_content("", "image/jpeg");
            return;
        }
        res.set_header("Cache-Control", "private, max-age=3600");
        res.set_content(r.body, "image/jpeg");
    });

    // Re-run post-processing on a single recording (by id). ctbrec needs the full
    // recording object, so we look it up in the cache first (mirrors delete).
    svr.Post("/api/rec/recording/postprocess", [](const Request& req, Response& res) {
        if (!rauth(req)) return unauth(res);
        json body;
        try { body = json::parse(req.body); } catch (...) {
            res.status = 400;
            res.set_content(json{{"status","error"},{"msg","invalid JSON"}}.dump(), "application/json");
            return;
        }
        json rec_obj = find_recording(body.value("id", ""));
        if (rec_obj.is_null()) {
            res.status = 404;
            res.set_content(json{{"status","error"},{"msg","recording not found"}}.dump(), "application/json");
            return;
        }
        relay(ctbrec_action(json{{"action","rerunPostProcessing"},{"recording", rec_obj}}), res);
    });

    // Batch: re-run post-processing on every WAITING recording (file present).
    // These are recordings that downloaded but never finished post-processing.
    // Long-running ⇒ detached thread + progress polled via the cleanup GET's
    // "reprocess" block.
    svr.Post("/api/rec/recordings/reprocess", [](const Request& req, Response& res) {
        if (!rauth(req)) return unauth(res);
        bool expected = false;
        if (!g_reprocess.running.compare_exchange_strong(expected, true)) {
            res.status = 409;
            res.set_content(json{{"status","error"},{"msg","reprocess already running"}}.dump(),
                            "application/json");
            return;
        }
        std::vector<json> jobs;
        {
            std::lock_guard<std::mutex> lk(g_rec_mu);
            ensure_recordings(/*force=*/true);
            for (const auto& r : g_rec_cache)
                if (r.value("status", "") == "WAITING" && !file_missing(r)) jobs.push_back(r);
        }
        g_reprocess.total.store(static_cast<int>(jobs.size()));
        g_reprocess.done.store(0);
        g_reprocess.failed.store(0);
        g_reprocess.started_at.store(static_cast<int64_t>(std::time(nullptr)));

        std::thread([jobs = std::move(jobs)]() {
            for (const auto& rec : jobs) {
                HttpResult r = ctbrec_action(json{{"action","rerunPostProcessing"},{"recording", rec}});
                bool ok = false;
                if (r.transport_ok && r.status == 200) {
                    try { ok = json::parse(r.body).value("status","") == "success"; } catch (...) {}
                }
                if (ok) g_reprocess.done.fetch_add(1);
                else    g_reprocess.failed.fetch_add(1);
            }
            { std::lock_guard<std::mutex> lk(g_rec_mu); g_rec_ts = 0; }
            std::cout << "[REC] reprocess done: triggered " << g_reprocess.done.load()
                      << ", failed " << g_reprocess.failed.load() << "\n";
            g_reprocess.running.store(false);
        }).detach();

        res.set_content(json{{"status","success"},
                             {"msg","reprocess started"},
                             {"eligible", g_reprocess.total.load()}}.dump(),
                        "application/json");
    });
}
