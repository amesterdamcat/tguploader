#include "web_server.h"
#include "log_tee.h"
#include "jwt_verify.h"
#include "user_auth.h"
#include "config.h"
#include "utils.h"
#include "net_monitor.h"
#include "job_history.h"
#include "scheduler.h"
#include "backup.h"
#include "bot_client.h"
#include "td_client.h"
#include "scanner.h"
#include "search_api.h"
#include "recorder_api.h"
#include "lequ_api.h"
#include "../include/httplib.h"
#include <algorithm>
#include <set>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <cctype>
#include <fcntl.h>
#include <sstream>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace fs = std::filesystem;
using json = nlohmann::json;

// -- Cancel flags --------------------------------------------------------

std::atomic<bool> g_cancel_fixbig{false};
std::atomic<bool> g_cancel_botupload{false};
std::atomic<bool> g_cancel_backup{false};

// -- Active fix-big paths (consulted by bot-upload to avoid races) -------

std::set<std::string> g_fixbig_active;
std::mutex            g_fixbig_active_mu;

void fixbig_mark_active(const std::string& p) {
    std::lock_guard<std::mutex> lk(g_fixbig_active_mu);
    g_fixbig_active.insert(p);
}
void fixbig_unmark_active(const std::string& p) {
    std::lock_guard<std::mutex> lk(g_fixbig_active_mu);
    g_fixbig_active.erase(p);
}
bool fixbig_is_active(const std::string& p) {
    std::lock_guard<std::mutex> lk(g_fixbig_active_mu);
    return g_fixbig_active.count(p) > 0;
}

ActivePathGuard::ActivePathGuard(std::initializer_list<std::string> ps) : paths(ps) {
    std::lock_guard<std::mutex> lk(g_fixbig_active_mu);
    for (auto& p : paths) g_fixbig_active.insert(p);
}
ActivePathGuard::~ActivePathGuard() {
    std::lock_guard<std::mutex> lk(g_fixbig_active_mu);
    for (auto& p : paths) g_fixbig_active.erase(p);
}

// -- Forward-declare task entry points from main.cpp ---------------------

void run_fix_big_task(const std::string& dir, bool recursive);
void run_bot_upload_task(const std::string& dir, bool recursive);
void run_bot_upload_multi(const std::vector<std::string>& dirs, bool recursive);
void run_fix_big_multi(const std::vector<std::string>& dirs, bool recursive);

// Forward decl of helper used by scheduler — defined below after launch_job.
void schedule_launch_job(const std::string& type,
                         const std::vector<std::string>& dirs,
                         const std::string& trigger);

// -- Job state (one per type) --------------------------------------------

struct JobState {
    std::atomic<bool> running{false};
    std::string type;
    std::time_t started_at = 0;
    std::time_t finished_at = 0;
    std::string last_error;
    std::string dir;
    std::vector<std::string> dirs;       // bulk-mode list (empty for single)
    std::mutex meta_mu;
};

static JobState g_job_fixbig;
static JobState g_job_botupload;
static JobState g_job_backup;
static LogBroadcaster g_bcast;

static std::mutex g_td_login_mu;
static uint64_t g_td_login_next_id = 1;
static std::map<std::string, std::unique_ptr<TdClient>> g_td_login_sessions;

static JobState* job_by_type(const std::string& type) {
    if (type == "fix-big") return &g_job_fixbig;
    if (type == "bot-upload") return &g_job_botupload;
    return nullptr;
}

static void job_status_json(json& j, const std::string& type, const JobState& s) {
    json o;
    o["type"] = type;
    o["running"] = s.running.load();
    o["started_at"] = s.started_at;
    o["finished_at"] = s.finished_at;
    o["last_error"] = s.last_error;
    o["dir"] = s.dir;
    if (type == "fix-big") o["cancel_requested"] = g_cancel_fixbig.load();
    else if (type == "bot-upload") o["cancel_requested"] = g_cancel_botupload.load();
    j[type] = o;
}

// Full job-status payload (shared by GET /api/job/status and the SSE push).
static json build_job_status() {
    json j;
    job_status_json(j, "fix-big", g_job_fixbig);
    job_status_json(j, "bot-upload", g_job_botupload);
    j["log_count"] = g_bcast.history_count();
    {
        std::lock_guard<std::mutex> lk(g_fixbig_active_mu);
        j["active_paths"] = static_cast<int>(g_fixbig_active.size());
    }
    return j;
}

// Backup progress payload (shared by GET /api/backup/status and the SSE push).
static json build_backup_status() {
    auto& P = backup_progress();
    json j;
    j["running"]          = g_job_backup.running.load();
    j["cancel_requested"] = g_cancel_backup.load();
    j["total"]            = P.total.load();
    j["done"]             = P.done.load();
    j["skipped"]          = P.skipped.load();
    j["failed"]           = P.failed.load();
    j["started_at"]       = P.started_at.load();
    j["finished_at"]      = P.finished_at.load();
    {
        std::lock_guard<std::mutex> lk(P.mu);
        j["scope"]         = P.scope;
        j["current_model"] = P.current_model;
        j["last_error"]    = P.last_error;
    }
    return j;
}

static std::string now_iso8601() {
    std::time_t t = std::time(nullptr);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", std::gmtime(&t));
    return std::string(buf) + "Z";
}

// -- Auth ---------------------------------------------------------------

static bool auth_ok(const httplib::Request& req) {
    std::string token;
    auto hdr = req.get_header_value("Authorization");
    if (hdr.rfind("Bearer ", 0) == 0) token = hdr.substr(7);
    else if (req.has_param("token")) token = req.get_param_value("token");
    if (token.empty()) return false;
    return jwt_verify_hs256(token);
}

static void reject_unauth(httplib::Response& res) {
    res.status = 401;
    res.set_content("{\"detail\":\"Unauthorized\"}", "application/json");
}

static std::string account_file_key(const std::string& phone) {
    std::string out;
    for (unsigned char c : phone) {
        if (std::isdigit(c)) out.push_back(static_cast<char>(c));
    }
    return out.empty() ? phone : out;
}

static json read_json_file_or(const std::string& path, const json& fallback) {
    std::ifstream f(path);
    if (!f) return fallback;
    try { json j; f >> j; return j; } catch (...) { return fallback; }
}

static bool write_json_0600(const std::string& path, const json& j, std::string& err) {
    std::string tmp = path + ".tmp";
    std::string body = j.dump(2);
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { err = "cannot write " + tmp; return false; }
    const char* p = body.data();
    size_t left = body.size();
    while (left > 0) {
        ssize_t n = ::write(fd, p, left);
        if (n <= 0) {
            ::close(fd);
            err = "write failed " + tmp;
            return false;
        }
        p += n;
        left -= static_cast<size_t>(n);
    }
    if (::close(fd) != 0) { err = "close failed " + tmp; return false; }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) { err = "rename failed: " + ec.message(); return false; }
    ::chmod(path.c_str(), 0600);
    return true;
}

static json td_auth_status_json(const TdAuthStatus& s, const std::string& sid = "") {
    json j{
        {"ok", s.ok},
        {"authorized", s.authorized},
        {"state", s.state},
        {"phone", s.phone},
    };
    if (!sid.empty()) j["session_id"] = sid;
    if (!s.error.empty()) j["error"] = s.error;
    return j;
}

static json td_accounts_public() {
    json arr = json::array();
    for (const auto& acc : list_accounts()) {
        arr.push_back(json{
            {"name", acc.name},
            {"phone", acc.phone},
            {"api_id", acc.api_id},
            {"api_hash", acc.api_hash},
            {"has_api_hash", !acc.api_hash.empty()},
            {"channel_id", acc.channel_id},
            {"session_dir", acc.session_dir},
        });
    }
    return arr;
}

// -- Pending .big scanner ------------------------------------------------

static std::string read_default_upload_dir() {
    std::string base = get_base_dir();
    std::ifstream env(base + "/.env");
    std::string line;
    while (std::getline(env, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("DEFAULT_UPLOAD_DIR=", 0) == 0) return line.substr(19);
    }
    return "";
}

// Per-model folder summary: size, video count, pending .big, uploaded markers.
// Sorted descending by total bytes so the worst hogs surface first.
static json scan_folders(const std::string& root) {
    struct Entry {
        std::string name;
        std::string path;
        int    video_count = 0;
        int    pending_big = 0;
        int    uploaded    = 0;
        int    available   = 0;
        uint64_t size_bytes = 0;
    };
    json result;
    result["root"] = root;
    result["total_bytes"] = 0;
    result["folders"] = json::array();
    if (root.empty() || !fs::is_directory(root)) return result;

    std::vector<Entry> folders;
    try {
        for (auto& sub : fs::directory_iterator(root)) {
            if (!sub.is_directory()) continue;
            Entry e;
            e.name = sub.path().filename().string();
            e.path = sub.path().string();
            // One level deep — per-model directories don't have sub-folders.
            try {
                for (auto& f : fs::directory_iterator(sub.path())) {
                    if (!f.is_regular_file()) continue;
                    auto p = f.path().string();
                    auto ext = f.path().extension().string();
                    uint64_t sz = 0;
                    std::error_code ec;
                    sz = fs::file_size(f.path(), ec);
                    if (!ec) e.size_bytes += sz;

                    if (ext == ".big") {
                        e.pending_big++;
                        continue;
                    }
                    // .big.done — already fix-big'd marker, not pending
                    if (p.size() >= 9 && p.compare(p.size() - 9, 9, ".big.done") == 0) continue;

                    if (is_video_file(p)) {
                        e.video_count++;
                        struct stat st;
                        bool has_uploaded = (::stat((p + ".uploaded").c_str(), &st) == 0);
                        bool has_big_sib  = (::stat((p + ".big").c_str(), &st) == 0);
                        bool name_uploaded = p.size() >= 9 && p.compare(p.size() - 9, 9, ".uploaded") == 0;
                        if (name_uploaded || has_uploaded) e.uploaded++;
                        else if (!has_big_sib) e.available++;
                    }
                }
            } catch (const std::exception&) {}
            folders.push_back(std::move(e));
        }
    } catch (const std::exception&) {}

    // Sort by size descending
    std::sort(folders.begin(), folders.end(),
              [](const Entry& a, const Entry& b) { return a.size_bytes > b.size_bytes; });

    uint64_t total = 0;
    json arr = json::array();
    for (auto& e : folders) {
        json o;
        o["name"] = e.name;
        o["path"] = e.path;
        o["video_count"] = e.video_count;
        o["pending_big"] = e.pending_big;
        o["uploaded"]    = e.uploaded;
        o["available"]   = e.available;
        o["size_bytes"]  = e.size_bytes;
        arr.push_back(o);
        total += e.size_bytes;
    }
    result["folders"] = arr;
    result["total_bytes"] = total;
    return result;
}

// Disk usage of the root partition for the upload dir (e.g. /mnt/storage).
static json scan_disk(const std::string& root) {
    json j;
    j["path"] = root;
    j["free"] = 0;
    j["total"] = 0;
    j["used"] = 0;
    if (root.empty() || !fs::exists(root)) return j;
    std::error_code ec;
    auto info = fs::space(root, ec);
    if (ec) return j;
    j["free"]  = static_cast<uint64_t>(info.free);
    j["total"] = static_cast<uint64_t>(info.capacity);
    j["used"]  = static_cast<uint64_t>(info.capacity - info.free);
    return j;
}

static json scan_pending(const std::string& root) {
    json result;
    result["root"] = root;
    json folders = json::array();
    int total = 0;

    if (root.empty() || !fs::is_directory(root)) {
        result["count"] = 0;
        result["folders"] = folders;
        return result;
    }

    std::map<std::string, int> per_folder;
    try {
        for (auto& entry : fs::recursive_directory_iterator(
                 root, fs::directory_options::skip_permission_denied)) {
            if (!entry.is_regular_file()) continue;
            auto p = entry.path();
            if (p.extension() == ".big") {
                std::string parent = p.parent_path().filename().string();
                per_folder[parent]++;
                total++;
            }
        }
    } catch (const std::exception&) {}
    for (auto& kv : per_folder) {
        json f;
        f["name"] = kv.first;
        f["count"] = kv.second;
        folders.push_back(f);
    }
    result["count"] = total;
    result["folders"] = folders;
    return result;
}

// -- Job launcher --------------------------------------------------------

static void launch_job(const std::string& type, const std::string& dir,
                       const std::vector<std::string>& dirs, bool recursive,
                       const std::string& trigger = "manual") {
    JobState* s = job_by_type(type);
    if (!s) return;

    if (type == "fix-big") g_cancel_fixbig.store(false);
    else if (type == "bot-upload") g_cancel_botupload.store(false);

    {
        std::lock_guard<std::mutex> lk(s->meta_mu);
        s->type = type;
        s->started_at = std::time(nullptr);
        s->finished_at = 0;
        s->last_error.clear();
        s->dir = dir;
        s->dirs = dirs;
    }

    // Persist to history (so a server restart mid-job still records it).
    json dj = json::array();
    if (!dirs.empty()) for (auto& d : dirs) dj.push_back(d);
    else if (!dir.empty()) dj.push_back(dir);
    int64_t hist_id = job_history_start(type, trigger, dj.dump());

    std::thread([type, dir, dirs, recursive, s, hist_id] {
        set_log_channel(type);
        std::string finish_status = "done";
        std::string finish_note;
        try {
            if (!dirs.empty()) {
                std::cout << "[JOB] start " << type << " bulk (" << dirs.size() << " folders)\n";
                if (type == "fix-big") run_fix_big_multi(dirs, recursive);
                else                   run_bot_upload_multi(dirs, recursive);
            } else {
                std::cout << "[JOB] start " << type
                          << (dir.empty() ? "" : " --dir " + dir) << "\n";
                if (type == "fix-big") run_fix_big_task(dir, recursive);
                else                   run_bot_upload_task(dir, recursive);
            }
            // Was it canceled mid-flight?
            if ((type == "fix-big" && g_cancel_fixbig.load()) ||
                (type == "bot-upload" && g_cancel_botupload.load())) {
                finish_status = "canceled";
            }
            std::cout << "[JOB] " << type << " " << finish_status << "\n";
        } catch (const std::exception& e) {
            std::cerr << "[JOB] " << type << " crashed: " << e.what() << "\n";
            finish_status = "crashed";
            finish_note = e.what();
            std::lock_guard<std::mutex> lk(s->meta_mu);
            s->last_error = e.what();
        }
        // success/failed counts: peek at log history for the conventional summary line.
        // We just record placeholder zeros for now; finer-grained counters are a TODO.
        job_history_finish(hist_id, finish_status, 0, 0, finish_note);
        {
            std::lock_guard<std::mutex> lk(s->meta_mu);
            s->finished_at = std::time(nullptr);
        }
        s->running.store(false);
    }).detach();
}

// Called by scheduler thread. Skips silently if the same job is already running.
void schedule_launch_job(const std::string& type,
                         const std::vector<std::string>& dirs,
                         const std::string& trigger) {
    JobState* s = job_by_type(type);
    if (!s) return;
    if (s->running.exchange(true)) {
        std::cout << "[SCHED] skip '" << trigger << "': " << type << " already running\n";
        return;
    }
    launch_job(type, "", dirs, true, trigger);
}

// Backup has its own JobState (it works on scope/models, not directories).
// Caller must CAS g_job_backup.running before calling.
static void launch_backup_job(const std::string& scope,
                              const std::vector<std::string>& models) {
    g_cancel_backup.store(false);
    {
        std::lock_guard<std::mutex> lk(g_job_backup.meta_mu);
        g_job_backup.type = "backup";
        g_job_backup.started_at = std::time(nullptr);
        g_job_backup.finished_at = 0;
        g_job_backup.last_error.clear();
        g_job_backup.dir = scope;      // stash scope here
        g_job_backup.dirs = models;
    }
    int64_t hist_id =
        job_history_start("backup", "manual",
                          json(json{{"scope", scope}, {"models", models}}).dump());

    std::thread([scope, models, hist_id] {
        set_log_channel("backup");
        std::string status = "done";
        std::string note;
        try {
            bool ok = run_backup(scope, models, g_cancel_backup);
            if (g_cancel_backup.load()) {
                status = "canceled";
            } else if (!ok) {
                status = "failed";
            }
        } catch (const std::exception& e) {
            std::cerr << "[JOB] backup crashed: " << e.what() << "\n";
            status = "crashed";
            note = e.what();
            backup_progress().running.store(false);
            backup_progress().finished_at.store(std::time(nullptr));
            {
                std::lock_guard<std::mutex> lk(g_job_backup.meta_mu);
                g_job_backup.last_error = e.what();
            }
        } catch (...) {
            std::cerr << "[JOB] backup crashed: unknown exception\n";
            status = "crashed";
            note = "unknown exception";
            backup_progress().running.store(false);
            backup_progress().finished_at.store(std::time(nullptr));
            {
                std::lock_guard<std::mutex> lk(g_job_backup.meta_mu);
                g_job_backup.last_error = note;
            }
        }
        auto& P = backup_progress();
        if (note.empty()) {
            std::lock_guard<std::mutex> lk(P.mu);
            if (status == "failed" && !P.last_error.empty()) note = P.last_error;
        }
        if (note.empty()) {
            note = "copied " + std::to_string(P.done.load()) +
                   ", skipped " + std::to_string(P.skipped.load()) +
                   ", failed " + std::to_string(P.failed.load());
        }
        job_history_finish(hist_id, status, P.done.load(), P.failed.load(), note);
        {
            std::lock_guard<std::mutex> lk(g_job_backup.meta_mu);
            g_job_backup.finished_at = std::time(nullptr);
        }
        g_job_backup.running.store(false);
    }).detach();
}

// -- Server -------------------------------------------------------------

int run_web_server(int port, const std::string& static_dir) {
    using namespace httplib;
    Server svr;
    // cpp-httplib uses one native thread per active handler. Keep the pool bounded:
    // the LeQu UI uses short polling with overlap guards, so 32 workers leave enough
    // headroom without reserving hundreds of thread stacks.
    int web_threads = 32;
    if (const char* raw = std::getenv("WEB_THREAD_POOL_SIZE")) {
        try { web_threads = std::clamp(std::stoi(raw), 8, 96); } catch (...) {}
    }
    svr.new_task_queue = [web_threads] { return new ThreadPool(web_threads); };
    svr.set_keep_alive_timeout(2);
    svr.set_keep_alive_max_count(50);

    if (fs::is_directory(static_dir)) {
        svr.set_mount_point("/static", static_dir);
    }

    svr.Get("/", [](const Request&, Response& res) {
        res.set_redirect("/static/upload.html");
    });

    svr.Get("/api/ping", [](const Request&, Response& res) {
        res.set_content("{\"ok\":true}", "application/json");
    });

    // Login — bcrypt verify against web/users.json, return HS256 JWT (24h).
    // Path is /api/auth/login (not /api/login) so this doesn't collide with the
    // Python viewer's login endpoint when both are reverse-proxied behind nginx.
    svr.Post("/api/auth/login", [](const Request& req, Response& res) {
        json body;
        try { body = json::parse(req.body); }
        catch (...) {
            res.status = 400;
            res.set_content(json{{"detail", "Invalid JSON"}}.dump(), "application/json");
            return;
        }
        std::string user = body.value("username", "");
        std::string pass = body.value("password", "");
        if (user.empty() || pass.empty()) {
            res.status = 400;
            res.set_content(json{{"detail", "username + password required"}}.dump(),
                            "application/json");
            return;
        }
        if (!user_auth_verify(user, pass)) {
            res.status = 401;
            // Constant-ish delay to slow brute force a touch
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            res.set_content(json{{"detail", "Invalid credentials"}}.dump(),
                            "application/json");
            std::cout << "[AUTH] login failed for '" << user << "'\n";
            return;
        }
        std::string token = user_auth_sign_token(user, 24);
        if (token.empty()) {
            res.status = 500;
            res.set_content(json{{"detail", "token sign failed"}}.dump(), "application/json");
            return;
        }
        std::cout << "[AUTH] login ok for '" << user << "'\n";
        json j;
        j["token"] = token;
        j["username"] = user;
        j["expires_in"] = 24 * 3600;
        res.set_content(j.dump(), "application/json");
    });

    // Quick "is this token still good?" check the frontend uses before showing app shell.
    svr.Get("/api/auth/check", [](const Request& req, Response& res) {
        if (!auth_ok(req)) return reject_unauth(res);
        res.set_content("{\"ok\":true}", "application/json");
    });

    // Change the authenticated user's password. Requires current password as
    // a second proof-of-life so a stolen token can't reset credentials.
    svr.Post("/api/auth/change_password", [](const Request& req, Response& res) {
        if (!auth_ok(req)) return reject_unauth(res);
        std::string token;
        auto hdr = req.get_header_value("Authorization");
        if (hdr.rfind("Bearer ", 0) == 0) token = hdr.substr(7);
        std::string user = jwt_extract_sub(token);
        if (user.empty()) return reject_unauth(res);

        json body;
        try { body = json::parse(req.body); }
        catch (...) {
            res.status = 400;
            res.set_content(json{{"detail", "Invalid JSON"}}.dump(), "application/json");
            return;
        }
        std::string current = body.value("current_password", "");
        std::string newp    = body.value("new_password", "");
        if (current.empty() || newp.empty()) {
            res.status = 400;
            res.set_content(json{{"detail", "current_password + new_password required"}}.dump(),
                            "application/json");
            return;
        }
        if (!user_auth_verify(user, current)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            res.status = 401;
            res.set_content(json{{"detail", "current password is wrong"}}.dump(),
                            "application/json");
            std::cout << "[AUTH] change_password rejected (bad current) for '" << user << "'\n";
            return;
        }
        std::string err = user_auth_set_password(user, newp);
        if (!err.empty()) {
            res.status = 400;
            res.set_content(json{{"detail", err}}.dump(), "application/json");
            return;
        }
        std::cout << "[AUTH] password changed for '" << user << "'\n";
        res.set_content(json{{"ok", true}}.dump(), "application/json");
    });

    svr.Get("/api/job/status", [](const Request& req, Response& res) {
        if (!auth_ok(req)) return reject_unauth(res);
        res.set_content(build_job_status().dump(), "application/json");
    });

    svr.Post("/api/job/start", [](const Request& req, Response& res) {
        if (!auth_ok(req)) return reject_unauth(res);
        json body;
        try { body = json::parse(req.body); }
        catch (...) {
            res.status = 400;
            res.set_content("{\"detail\":\"Invalid JSON\"}", "application/json");
            return;
        }
        std::string type = body.value("type", "");
        std::string dir = body.value("dir", "");
        bool recursive = body.value("recursive", true);
        std::vector<std::string> dirs;
        if (body.contains("dirs") && body["dirs"].is_array()) {
            for (auto& d : body["dirs"]) {
                if (d.is_string() && !d.get<std::string>().empty()) {
                    dirs.push_back(d.get<std::string>());
                }
            }
        }

        JobState* s = job_by_type(type);
        if (!s) {
            res.status = 400;
            res.set_content("{\"detail\":\"type must be bot-upload or fix-big\"}", "application/json");
            return;
        }
        if (s->running.exchange(true)) {
            res.status = 409;
            res.set_content("{\"detail\":\"" + type + " already running\"}", "application/json");
            return;
        }
        launch_job(type, dir, dirs, recursive);

        json j;
        j["ok"] = true;
        j["type"] = type;
        res.set_content(j.dump(), "application/json");
    });

    svr.Post("/api/job/stop", [](const Request& req, Response& res) {
        if (!auth_ok(req)) return reject_unauth(res);
        std::string type = req.has_param("type") ? req.get_param_value("type") : "";
        bool force = (req.has_param("force") && req.get_param_value("force") == "1");
        if (type.empty()) {
            try {
                auto b = json::parse(req.body);
                type  = b.value("type", "");
                force = force || b.value("force", false);
            } catch (...) {}
        }
        auto kick = [&](const std::string& t) {
            if (t == "fix-big") {
                g_cancel_fixbig.store(true);
                std::cout << "[JOB] " << (force ? "FORCE-STOP" : "cancel")
                          << " requested: fix-big\n";
            } else if (t == "bot-upload") {
                g_cancel_botupload.store(true);
                std::cout << "[JOB] " << (force ? "FORCE-STOP" : "cancel")
                          << " requested: bot-upload\n";
            }
        };
        if (type == "fix-big" || type == "bot-upload") {
            kick(type);
        } else if (type == "all" || type.empty()) {
            kick("fix-big");
            kick("bot-upload");
        } else {
            res.status = 400;
            res.set_content("{\"detail\":\"unknown type\"}", "application/json");
            return;
        }
        // force mode: also clear stuck active-path locks immediately so
        // bot-upload doesn't keep skipping files that were locked by a
        // crashed/canceled fix-big task.
        if (force) {
            std::lock_guard<std::mutex> lk(g_fixbig_active_mu);
            size_t n = g_fixbig_active.size();
            g_fixbig_active.clear();
            if (n > 0) std::cout << "[JOB] force: cleared " << n << " active path lock(s)\n";
        }
        json j; j["ok"] = true; j["force"] = force;
        res.set_content(j.dump(), "application/json");
    });

    svr.Get("/api/job/logs", [](const Request& req, Response& res) {
        if (!auth_ok(req)) return reject_unauth(res);
        size_t max_lines = 500;
        if (req.has_param("max")) {
            try { max_lines = std::stoul(req.get_param_value("max")); } catch (...) {}
        }
        std::string ch = req.has_param("channel") ? req.get_param_value("channel") : "";
        json j;
        json arr = json::array();
        for (auto& line : g_bcast.snapshot(ch, max_lines)) {
            json o;
            o["ch"]   = line.channel;
            o["text"] = line.text;
            arr.push_back(o);
        }
        j["lines"] = arr;
        j["channels"] = g_bcast.channel_counts();
        res.set_content(j.dump(), "application/json");
    });

    // -- Schedules CRUD ----------------------------------------------------
    svr.Get("/api/schedules", [](const Request& req, Response& res) {
        if (!auth_ok(req)) return reject_unauth(res);
        json arr = json::array();
        for (auto& s : schedules_list()) {
            json o;
            o["id"] = s.id;
            o["name"] = s.name;
            o["cron"] = s.cron;
            o["type"] = s.type;
            try { o["dirs"] = json::parse(s.dirs_json); } catch (...) { o["dirs"] = json::array(); }
            o["enabled"] = s.enabled;
            o["last_run_at"] = s.last_run_at;
            arr.push_back(o);
        }
        res.set_content(json{{"items", arr}}.dump(), "application/json");
    });

    svr.Post("/api/schedules", [](const Request& req, Response& res) {
        if (!auth_ok(req)) return reject_unauth(res);
        json body;
        try { body = json::parse(req.body); } catch (...) {
            res.status = 400;
            res.set_content(json{{"detail", "Invalid JSON"}}.dump(), "application/json");
            return;
        }
        std::string name = body.value("name", "");
        std::string cron = body.value("cron", "");
        std::string type = body.value("type", "");
        bool enabled = body.value("enabled", true);
        std::string dirs_json = "[]";
        if (body.contains("dirs") && body["dirs"].is_array()) dirs_json = body["dirs"].dump();

        if (name.empty() || cron.empty()) {
            res.status = 400;
            res.set_content(json{{"detail", "name + cron required"}}.dump(), "application/json");
            return;
        }
        std::string err = cron_validate(cron);
        if (!err.empty()) {
            res.status = 400;
            res.set_content(json{{"detail", err}}.dump(), "application/json");
            return;
        }
        if (type != "fix-big" && type != "bot-upload") {
            res.status = 400;
            res.set_content(json{{"detail", "type must be fix-big or bot-upload"}}.dump(),
                            "application/json");
            return;
        }
        int64_t id = schedules_create(name, cron, type, dirs_json, enabled);
        if (id == 0) {
            res.status = 500;
            res.set_content(json{{"detail", "insert failed"}}.dump(), "application/json");
            return;
        }
        res.set_content(json{{"id", id}, {"ok", true}}.dump(), "application/json");
    });

    svr.Put("/api/schedules/:id", [](const Request& req, Response& res) {
        if (!auth_ok(req)) return reject_unauth(res);
        int64_t id = 0;
        try { id = std::stoll(req.path_params.at("id")); } catch (...) {}
        if (id == 0) { res.status = 400; res.set_content("{\"detail\":\"bad id\"}", "application/json"); return; }
        json body;
        try { body = json::parse(req.body); } catch (...) {
            res.status = 400; res.set_content("{\"detail\":\"Invalid JSON\"}", "application/json"); return;
        }
        std::string dirs_json = "[]";
        if (body.contains("dirs") && body["dirs"].is_array()) dirs_json = body["dirs"].dump();
        bool ok = schedules_update(id,
                                   body.value("name", ""),
                                   body.value("cron", ""),
                                   body.value("type", ""),
                                   dirs_json,
                                   body.value("enabled", true));
        if (!ok) { res.status = 400; res.set_content("{\"detail\":\"update failed (validation?)\"}", "application/json"); return; }
        res.set_content("{\"ok\":true}", "application/json");
    });

    svr.Delete("/api/schedules/:id", [](const Request& req, Response& res) {
        if (!auth_ok(req)) return reject_unauth(res);
        int64_t id = 0;
        try { id = std::stoll(req.path_params.at("id")); } catch (...) {}
        if (id == 0) { res.status = 400; res.set_content("{\"detail\":\"bad id\"}", "application/json"); return; }
        if (!schedules_delete(id)) { res.status = 404; res.set_content("{\"detail\":\"not found\"}", "application/json"); return; }
        res.set_content("{\"ok\":true}", "application/json");
    });

    svr.Get("/api/job/history", [](const Request& req, Response& res) {
        if (!auth_ok(req)) return reject_unauth(res);
        int limit = 50;
        if (req.has_param("limit")) {
            try { limit = std::stoi(req.get_param_value("limit")); } catch (...) {}
        }
        if (limit > 500) limit = 500;
        json arr = json::array();
        for (auto& r : job_history_list(limit)) {
            json o;
            o["id"] = r.id;
            o["type"] = r.type;
            o["trigger"] = r.trigger;
            o["started_at"] = r.started_at;
            o["finished_at"] = r.finished_at;
            o["duration"] = (r.finished_at > 0 ? (r.finished_at - r.started_at) : 0);
            o["status"] = r.status;
            o["success_count"] = r.success_count;
            o["failed_count"] = r.failed_count;
            try { o["dirs"] = json::parse(r.dirs_json); } catch (...) { o["dirs"] = json::array(); }
            o["note"] = r.note;
            arr.push_back(o);
        }
        res.set_content(json{{"items", arr}}.dump(), "application/json");
    });

    svr.Get("/api/job/pending", [](const Request& req, Response& res) {
        if (!auth_ok(req)) return reject_unauth(res);
        std::string dir = req.has_param("dir") ? req.get_param_value("dir")
                                                : read_default_upload_dir();
        json j = scan_pending(dir);
        res.set_content(j.dump(), "application/json");
    });

    // All model folders, sorted by total bytes descending.
    svr.Get("/api/folders", [](const Request& req, Response& res) {
        if (!auth_ok(req)) return reject_unauth(res);
        std::string dir = req.has_param("dir") ? req.get_param_value("dir")
                                                : read_default_upload_dir();
        json j = scan_folders(dir);
        res.set_content(j.dump(), "application/json");
    });

    // Disk capacity / free / used for the upload root.
    svr.Get("/api/system/disk", [](const Request& req, Response& res) {
        if (!auth_ok(req)) return reject_unauth(res);
        std::string dir = req.has_param("dir") ? req.get_param_value("dir")
                                                : read_default_upload_dir();
        json j = scan_disk(dir);
        res.set_content(j.dump(), "application/json");
    });

    // Read editable .env keys (subset — we only expose ones that affect uploader behavior)
    svr.Get("/api/env", [](const Request& req, Response& res) {
        if (!auth_ok(req)) return reject_unauth(res);
        static const std::vector<std::string> KEYS = {
            "DEFAULT_UPLOAD_DIR", "DELETE_AFTER_UPLOAD", "MARK_UPLOADED_FILES",
            "UPLOADED_SUFFIX", "EXEMPT_FOLDERS",
        };
        std::string path = get_base_dir() + "/.env";
        std::map<std::string, std::string> env_map;
        {
            std::ifstream f(path);
            std::string line;
            while (std::getline(f, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty() || line[0] == '#') continue;
                auto eq = line.find('=');
                if (eq == std::string::npos) continue;
                env_map[line.substr(0, eq)] = line.substr(eq + 1);
            }
        }
        json j;
        j["path"] = path;
        for (auto& k : KEYS) j[k] = env_map.count(k) ? env_map[k] : "";
        res.set_content(j.dump(), "application/json");
    });

    // Write a subset of .env keys (preserves existing comments + other keys).
    svr.Post("/api/env", [](const Request& req, Response& res) {
        if (!auth_ok(req)) return reject_unauth(res);
        json body;
        try { body = json::parse(req.body); }
        catch (...) {
            res.status = 400;
            res.set_content(json{{"detail", "Invalid JSON"}}.dump(), "application/json");
            return;
        }
        // Only allow editing these whitelisted keys (defence-in-depth — never write JWT_SECRET et al.)
        static const std::set<std::string> ALLOWED = {
            "DEFAULT_UPLOAD_DIR", "DELETE_AFTER_UPLOAD", "MARK_UPLOADED_FILES",
            "UPLOADED_SUFFIX", "EXEMPT_FOLDERS",
        };
        std::map<std::string, std::string> updates;
        for (auto& [k, v] : body.items()) {
            if (!ALLOWED.count(k)) continue;
            if (!v.is_string()) continue;
            updates[k] = v.get<std::string>();
        }

        // Read existing .env, line by line, replace matching keys, then append new ones
        std::string path = get_base_dir() + "/.env";
        std::vector<std::string> out_lines;
        std::set<std::string> seen_keys;
        {
            std::ifstream f(path);
            std::string line;
            while (std::getline(f, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty() || line[0] == '#') {
                    out_lines.push_back(line);
                    continue;
                }
                auto eq = line.find('=');
                if (eq == std::string::npos) { out_lines.push_back(line); continue; }
                std::string k = line.substr(0, eq);
                if (updates.count(k)) {
                    out_lines.push_back(k + "=" + updates[k]);
                    seen_keys.insert(k);
                } else {
                    out_lines.push_back(line);
                }
            }
        }
        // Append new keys that didn't exist before
        for (auto& [k, v] : updates) {
            if (!seen_keys.count(k)) out_lines.push_back(k + "=" + v);
        }

        std::string tmp = path + ".tmp";
        try {
            std::ofstream out(tmp);
            for (auto& l : out_lines) out << l << "\n";
            out.close();
            if (out.fail()) throw std::runtime_error("write failed");
            fs::rename(tmp, path);
        } catch (const std::exception& e) {
            try { fs::remove(tmp); } catch (...) {}
            res.status = 500;
            res.set_content(json{{"detail", std::string("write failed: ") + e.what()}}.dump(),
                            "application/json");
            return;
        }
        std::cout << "[WEB] .env updated (" << updates.size() << " key(s))\n";
        res.set_content(json{{"ok", true},
                             {"updated", updates.size()},
                             {"note", "next job run picks up new values"}}.dump(),
                        "application/json");
    });

    // Read current bots.json
    svr.Get("/api/bots", [](const Request& req, Response& res) {
        if (!auth_ok(req)) return reject_unauth(res);
        std::string path = get_base_dir() + "/.account_configs/bots.json";
        std::ifstream f(path);
        if (!f.is_open()) {
            res.status = 404;
            res.set_content(json{{"detail", "bots.json not found"}}.dump(), "application/json");
            return;
        }
        std::stringstream ss; ss << f.rdbuf();
        try {
            auto j = json::parse(ss.str());
            j["_path"] = path;
            res.set_content(j.dump(), "application/json");
        } catch (const std::exception& e) {
            res.status = 500;
            res.set_content(json{{"detail", std::string("parse error: ") + e.what()}}.dump(),
                            "application/json");
        }
    });

    // Replace bots.json (atomic write via temp file)
    svr.Post("/api/bots", [](const Request& req, Response& res) {
        if (!auth_ok(req)) return reject_unauth(res);
        json body;
        try { body = json::parse(req.body); }
        catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"detail", std::string("Invalid JSON: ") + e.what()}}.dump(),
                            "application/json");
            return;
        }
        // Validate shape
        if (!body.contains("api_url") || !body["api_url"].is_string() ||
            body["api_url"].get<std::string>().empty()) {
            res.status = 400;
            res.set_content(json{{"detail", "api_url required (non-empty string)"}}.dump(),
                            "application/json");
            return;
        }
        if (!body.contains("channel_id") || !body["channel_id"].is_string() ||
            body["channel_id"].get<std::string>().empty()) {
            res.status = 400;
            res.set_content(json{{"detail", "channel_id required (non-empty string)"}}.dump(),
                            "application/json");
            return;
        }
        if (!body.contains("bots") || !body["bots"].is_object() || body["bots"].empty()) {
            res.status = 400;
            res.set_content(json{{"detail", "bots required (at least one entry)"}}.dump(),
                            "application/json");
            return;
        }
        for (auto& [k, v] : body["bots"].items()) {
            if (k.empty()) {
                res.status = 400;
                res.set_content(json{{"detail", "bot name cannot be empty"}}.dump(),
                                "application/json");
                return;
            }
            if (!v.is_string() || v.get<std::string>().empty()) {
                res.status = 400;
                res.set_content(json{{"detail", "token for '" + k + "' must be a non-empty string"}}.dump(),
                                "application/json");
                return;
            }
        }

        // Remove any client-injected metadata keys before writing.
        body.erase("_path");

        std::string path = get_base_dir() + "/.account_configs/bots.json";
        std::string tmp  = path + ".tmp";
        try {
            std::ofstream out(tmp);
            if (!out) throw std::runtime_error("cannot open tmp file");
            out << body.dump(2) << "\n";
            out.close();
            if (out.fail()) throw std::runtime_error("write failed");
            fs::rename(tmp, path);
        } catch (const std::exception& e) {
            try { fs::remove(tmp); } catch (...) {}
            res.status = 500;
            res.set_content(json{{"detail", std::string("write failed: ") + e.what()}}.dump(),
                            "application/json");
            return;
        }
        std::cout << "[WEB] bots.json updated (" << body["bots"].size() << " bot(s))\n";
        json j;
        j["ok"] = true;
        j["bots_count"] = body["bots"].size();
        j["note"] = "next bot-upload run will pick up the new config";
        res.set_content(j.dump(), "application/json");
    });

    // TDLib account config used by pre-backup source-channel scans.
    svr.Get("/api/td/accounts", [](const Request& req, Response& res) {
        if (!auth_ok(req)) return reject_unauth(res);
        res.set_content(json{{"accounts", td_accounts_public()}}.dump(), "application/json");
    });

    svr.Post("/api/td/accounts", [](const Request& req, Response& res) {
        if (!auth_ok(req)) return reject_unauth(res);
        json body;
        try { body = json::parse(req.body); }
        catch (...) {
            res.status = 400;
            res.set_content(json{{"detail", "Invalid JSON"}}.dump(), "application/json");
            return;
        }
        std::string name = body.value("name", "");
        std::string phone = body.value("phone", "");
        std::string api_hash = body.value("api_hash", "");
        std::string channel_id = body.value("channel_id", "");
        int api_id = 0;
        try {
            if (body.contains("api_id") && body["api_id"].is_number_integer())
                api_id = body["api_id"].get<int>();
            else if (body.contains("api_id") && body["api_id"].is_string())
                api_id = std::stoi(body["api_id"].get<std::string>());
        } catch (...) {}
        if (name.empty() || phone.empty() || api_id <= 0 || api_hash.empty() || channel_id.empty()) {
            res.status = 400;
            res.set_content(json{{"detail", "name, phone, api_id, api_hash, channel_id are required"}}.dump(),
                            "application/json");
            return;
        }

        std::string dir = get_base_dir() + "/.account_configs";
        std::error_code ec;
        fs::create_directories(dir, ec);
        std::string key = account_file_key(phone);
        json mapping = read_json_file_or(dir + "/accounts.json", json::object());
        mapping[name] = key;
        json account = {
            {"phone", phone},
            {"api_id", api_id},
            {"api_hash", api_hash},
            {"channel_id", channel_id},
        };
        std::string err;
        if (!write_json_0600(dir + "/accounts.json", mapping, err) ||
            !write_json_0600(dir + "/" + key + ".json", account, err)) {
            res.status = 500;
            res.set_content(json{{"detail", err}}.dump(), "application/json");
            return;
        }
        std::cout << "[WEB] TD account saved name=" << name << " channel=" << channel_id << "\n";
        res.set_content(json{{"ok", true}, {"accounts", td_accounts_public()}}.dump(), "application/json");
    });

    auto finish_td_login = [](const std::string& sid, const TdAuthStatus& st) {
        json out = td_auth_status_json(st, sid);
        if (st.authorized || st.state == "closed") {
            std::lock_guard<std::mutex> lk(g_td_login_mu);
            g_td_login_sessions.erase(sid);
        }
        return out;
    };

    svr.Post("/api/td/login/start", [finish_td_login](const Request& req, Response& res) {
        if (!auth_ok(req)) return reject_unauth(res);
        json body;
        try { body = json::parse(req.body); }
        catch (...) {
            res.status = 400;
            res.set_content(json{{"detail", "Invalid JSON"}}.dump(), "application/json");
            return;
        }
        std::string name = body.value("name", "");
        if (name.empty()) {
            res.status = 400;
            res.set_content(json{{"detail", "name required"}}.dump(), "application/json");
            return;
        }
        try {
            auto cfg = load_account(name);
            auto client = std::make_unique<TdClient>(cfg);
            std::string sid;
            {
                std::lock_guard<std::mutex> lk(g_td_login_mu);
                sid = std::to_string(g_td_login_next_id++);
                g_td_login_sessions[sid] = std::move(client);
            }
            TdAuthStatus st;
            {
                std::lock_guard<std::mutex> lk(g_td_login_mu);
                st = g_td_login_sessions[sid]->start_web_auth();
            }
            res.set_content(finish_td_login(sid, st).dump(), "application/json");
        } catch (const std::exception& e) {
            res.status = 500;
            res.set_content(json{{"detail", e.what()}}.dump(), "application/json");
        }
    });

    auto continue_td_login = [finish_td_login](const Request& req, Response& res, const std::string& kind) {
        if (!auth_ok(req)) return reject_unauth(res);
        json body;
        try { body = json::parse(req.body); }
        catch (...) {
            res.status = 400;
            res.set_content(json{{"detail", "Invalid JSON"}}.dump(), "application/json");
            return;
        }
        std::string sid = body.value("session_id", "");
        std::string value = body.value(kind, "");
        if (sid.empty() || value.empty()) {
            res.status = 400;
            res.set_content(json{{"detail", "session_id and " + kind + " required"}}.dump(),
                            "application/json");
            return;
        }
        TdAuthStatus st;
        {
            std::lock_guard<std::mutex> lk(g_td_login_mu);
            auto it = g_td_login_sessions.find(sid);
            if (it == g_td_login_sessions.end()) {
                res.status = 404;
                res.set_content(json{{"detail", "login session expired"}}.dump(), "application/json");
                return;
            }
            st = (kind == "code") ? it->second->submit_auth_code(value)
                                  : it->second->submit_auth_password(value);
        }
        res.set_content(finish_td_login(sid, st).dump(), "application/json");
    };

    svr.Post("/api/td/login/code", [continue_td_login](const Request& req, Response& res) {
        continue_td_login(req, res, "code");
    });
    svr.Post("/api/td/login/password", [continue_td_login](const Request& req, Response& res) {
        continue_td_login(req, res, "password");
    });

    svr.Post("/api/td/scan", [](const Request& req, Response& res) {
        if (!auth_ok(req)) return reject_unauth(res);
        json body;
        try { if (!req.body.empty()) body = json::parse(req.body); }
        catch (...) {
            res.status = 400;
            res.set_content(json{{"detail", "Invalid JSON"}}.dump(), "application/json");
            return;
        }
        std::string name = body.value("name", "");
        if (name.empty()) {
            BackupConfig cfg = backup_config_load();
            auto accounts = list_accounts();
            for (const auto& acc : accounts) {
                if (!cfg.source_channel_id.empty() && acc.channel_id == cfg.source_channel_id) {
                    name = acc.name;
                    break;
                }
            }
            if (name.empty() && accounts.size() == 1) name = accounts[0].name;
        }
        if (name.empty()) {
            res.status = 400;
            res.set_content(json{{"detail", "scan account required"}}.dump(), "application/json");
            return;
        }
        bool fetch_thumbnails = body.value("fetch_thumbnails", false);
        if (g_job_backup.running.exchange(true)) {
            res.status = 409;
            res.set_content(json{{"detail", "backup or scan is already running"}}.dump(), "application/json");
            return;
        }
        {
            std::lock_guard<std::mutex> lk(g_job_backup.meta_mu);
            g_job_backup.type = "td-scan";
            g_job_backup.started_at = std::time(nullptr);
            g_job_backup.finished_at = 0;
            g_job_backup.last_error.clear();
            g_job_backup.dir = name;
            g_job_backup.dirs.clear();
        }
        std::thread([name, fetch_thumbnails] {
            set_log_channel("backup");
            std::string status = "done";
            try {
                std::cout << "[SCAN] manual full scan start: account=" << name
                          << ", thumbnails=" << (fetch_thumbnails ? "on" : "off") << "\n";
                auto r = scan_account_channel(name, false, true, fetch_thumbnails);
                if (!r.ok) {
                    status = "failed";
                    std::cerr << "\033[31m[ERROR]\033[0m [SCAN] " << r.error << "\n";
                    std::lock_guard<std::mutex> lk(g_job_backup.meta_mu);
                    g_job_backup.last_error = r.error;
                } else {
                    std::cout << "[SCAN] manual full scan done: inserted=" << r.inserted << "\n";
                }
            } catch (const std::exception& e) {
                status = "crashed";
                std::cerr << "\033[31m[ERROR]\033[0m [SCAN] crashed: " << e.what() << "\n";
                std::lock_guard<std::mutex> lk(g_job_backup.meta_mu);
                g_job_backup.last_error = e.what();
            }
            {
                std::lock_guard<std::mutex> lk(g_job_backup.meta_mu);
                g_job_backup.finished_at = std::time(nullptr);
            }
            std::cout << "[SCAN] " << status << "\n";
            g_job_backup.running.store(false);
        }).detach();
        res.set_content(json{{"ok", true}, {"account", name}}.dump(), "application/json");
    });

    // ============== Backup: copy channel → backup channel ==============

    // Current backup config (redacted — never returns the bot token).
    svr.Get("/api/backup/config", [](const Request& req, Response& res) {
        if (!auth_ok(req)) return reject_unauth(res);
        res.set_content(backup_config_public().dump(), "application/json");
    });

    // Save backup config, then probe reachability/admin rights in both channels.
    svr.Post("/api/backup/config", [](const Request& req, Response& res) {
        if (!auth_ok(req)) return reject_unauth(res);
        json body;
        try { body = json::parse(req.body); }
        catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"detail", std::string("Invalid JSON: ") + e.what()}}.dump(),
                            "application/json");
            return;
        }
        // Start from the saved config so the UI can update individual fields
        // (e.g. backup channel) without having to re-enter the token.
        BackupConfig cfg = backup_config_load();
        auto getstr = [&](const char* k, std::string& dst) {
            if (body.contains(k) && body[k].is_string()) dst = body[k].get<std::string>();
        };
        getstr("bot_name", cfg.bot_name);
        getstr("source_channel_id", cfg.source_channel_id);
        getstr("backup_channel_id", cfg.backup_channel_id);
        if (body.contains("api_url") && body["api_url"].is_string() &&
            !body["api_url"].get<std::string>().empty())
            cfg.api_url = body["api_url"].get<std::string>();
        // Only overwrite the token when a non-empty one is supplied.
        if (body.contains("bot_token") && body["bot_token"].is_string() &&
            !body["bot_token"].get<std::string>().empty())
            cfg.bot_token = body["bot_token"].get<std::string>();
        if (body.contains("delay_ms") && body["delay_ms"].is_number_integer())
            cfg.delay_ms = std::max(0, std::min(60000, body["delay_ms"].get<int>()));

        std::string err;
        if (!backup_config_save(cfg, err)) {
            res.status = 500;
            res.set_content(json{{"detail", "save failed: " + err}}.dump(), "application/json");
            return;
        }
        std::cout << "[WEB] backup config saved (backup_channel=" << cfg.backup_channel_id << ")\n";
        json out;
        out["ok"] = true;
        out["config"] = backup_config_public();
        out["verify"] = backup_config_verify(cfg);
        res.set_content(out.dump(), "application/json");
    });

    // Verify the currently-saved config without changing it.
    svr.Get("/api/backup/verify", [](const Request& req, Response& res) {
        if (!auth_ok(req)) return reject_unauth(res);
        res.set_content(backup_config_verify(backup_config_load()).dump(), "application/json");
    });

    // Per-model rollup: total videos vs how many already copied to backup.
    svr.Get("/api/backup/models", [](const Request& req, Response& res) {
        if (!auth_ok(req)) return reject_unauth(res);
        BackupConfig cfg = backup_config_load();
        res.set_content(backup_models(cfg.source_channel_id, cfg.backup_channel_id).dump(), "application/json");
    });

    // Live progress.
    svr.Get("/api/backup/status", [](const Request& req, Response& res) {
        if (!auth_ok(req)) return reject_unauth(res);
        res.set_content(build_backup_status().dump(), "application/json");
    });

    // Start a backup. body: {scope:"all"|"models", models:[...]}
    svr.Post("/api/backup/start", [](const Request& req, Response& res) {
        if (!auth_ok(req)) return reject_unauth(res);
        json body;
        try { body = json::parse(req.body); } catch (...) { body = json::object(); }
        std::string scope = body.value("scope", "models");
        std::vector<std::string> models;
        if (body.contains("models") && body["models"].is_array())
            for (auto& m : body["models"]) if (m.is_string()) models.push_back(m.get<std::string>());
        if (scope != "all" && models.empty()) {
            res.status = 400;
            res.set_content(json{{"detail", "select at least one model (or scope=all)"}}.dump(),
                            "application/json");
            return;
        }
        BackupConfig cfg = backup_config_load();
        if (cfg.bot_token.empty() || cfg.backup_channel_id.empty()) {
            res.status = 400;
            res.set_content(json{{"detail", "backup not configured (set bot token + backup channel first)"}}.dump(),
                            "application/json");
            return;
        }
        if (g_job_backup.running.exchange(true)) {
            res.status = 409;
            res.set_content(json{{"detail", "a backup is already running"}}.dump(), "application/json");
            return;
        }
        launch_backup_job(scope, models);
        res.set_content(json{{"ok", true}, {"scope", scope}, {"models", models.size()}}.dump(),
                        "application/json");
    });

    // Stop the backup. It exits between messages, so this takes effect quickly.
    svr.Post("/api/backup/stop", [](const Request& req, Response& res) {
        if (!auth_ok(req)) return reject_unauth(res);
        g_cancel_backup.store(true);
        std::cout << "[BACKUP] stop requested\n";
        res.set_content(json{{"ok", true}}.dump(), "application/json");
    });

    // System net throughput — last 60 samples (1s each) for a sparkline.
    svr.Get("/api/system/net", [](const Request& req, Response& res) {
        if (!auth_ok(req)) return reject_unauth(res);
        auto& m = net_monitor();
        auto samples = m.samples(60);
        auto last = m.latest();
        json j;
        json arr = json::array();
        for (auto& s : samples) {
            json o;
            o["at"] = s.at;
            o["rx"] = s.rx_bps;
            o["tx"] = s.tx_bps;
            arr.push_back(o);
        }
        j["samples"] = arr;
        j["latest_tx"] = last.tx_bps;
        j["latest_rx"] = last.rx_bps;
        res.set_content(j.dump(), "application/json");
    });

    svr.Get("/api/job/stream", [](const Request& req, Response& res) {
        if (!auth_ok(req)) return reject_unauth(res);

        // Optional ?channel=fix-big / bot-upload / web — empty means all channels.
        std::string ch = req.has_param("channel") ? req.get_param_value("channel") : "";
        auto sub = g_bcast.subscribe(ch);
        auto history = g_bcast.snapshot(ch, 500);

        res.set_chunked_content_provider(
            "text/event-stream",
            [sub, history](size_t /*offset*/, DataSink& sink) -> bool {
                // Replay: each line carries its channel as the SSE event name
                // so frontend can demux without parsing.
                for (const auto& line : history) {
                    std::string ev = "event: replay.";
                    ev += line.channel.empty() ? "web" : line.channel;
                    ev += "\ndata: " + line.text + "\n\n";
                    if (!sink.write(ev.data(), ev.size())) return false;
                }
                {
                    std::string ready = "event: ready\ndata: \n\n";
                    if (!sink.write(ready.data(), ready.size())) return false;
                }
                // Telemetry (net throughput) and job/backup status are pushed
                // over this same SSE connection instead of the client polling
                // /api/system/net, /api/job/status and /api/backup/status. These
                // are one-directional server→client feeds, so SSE — already open
                // here — is the right fit; cpp-httplib has no WebSocket support
                // and WS would add nothing for one-way data. The periodic wakeup
                // also doubles as the keep-alive (no separate ping needed).
                //
                // NOTE: do NOT init these to time_point::min() — (now - min())
                // overflows the int64 nanosecond duration and the comparison
                // never fires. Seed slightly in the past for an immediate first
                // push instead.
                auto clk = std::chrono::steady_clock::now();
                auto last_net    = clk - std::chrono::seconds(2);
                auto last_status = clk - std::chrono::seconds(2);
                while (true) {
                    if (sub->dead.load()) break;
                    LogBroadcaster::Line line;
                    if (sub->wait_pop(line, 500)) {
                        std::string ev = "event: log.";
                        ev += line.channel.empty() ? "web" : line.channel;
                        ev += "\ndata: " + line.text + "\n\n";
                        if (!sink.write(ev.data(), ev.size())) return false;
                    }
                    auto now = std::chrono::steady_clock::now();
                    if (now - last_net >= std::chrono::seconds(1)) {
                        last_net = now;
                        auto s = net_monitor().latest();
                        json nj{{"at", s.at}, {"tx", s.tx_bps}, {"rx", s.rx_bps}};
                        std::string ev = "event: net\ndata: " + nj.dump() + "\n\n";
                        if (!sink.write(ev.data(), ev.size())) return false;
                    }
                    if (now - last_status >= std::chrono::seconds(2)) {
                        last_status = now;
                        std::string ev = "event: status\ndata: " + build_job_status().dump() + "\n\n";
                        if (!sink.write(ev.data(), ev.size())) return false;
                        std::string be = "event: backup_status\ndata: " + build_backup_status().dump() + "\n\n";
                        if (!sink.write(be.data(), be.size())) return false;
                    }
                }
                sink.done();
                return true;
            },
            [sub](bool) { g_bcast.unsubscribe(sub); });
    });

    // Video search/browse API — the C++ port of web/app.py (serves index.html's
    // /api/* calls so the Python uvicorn service can be retired).
    register_search_routes(svr);

    // ctbrec recorder control API — proxies HMAC-signed actions to the local
    // ctbrec server so a lean recorder UI can replace its sluggish web UI.
    register_recorder_routes(svr);

    // LeQu (乐趣Live) recorder — signs requests to the LeQu gateway directly,
    // auto-tracks anchors and records them with ffmpeg.
    register_lequ_routes(svr);

    svr.set_default_headers({
        {"Access-Control-Allow-Origin", "*"},
        {"Access-Control-Allow-Headers", "Authorization, Content-Type"},
        {"Access-Control-Allow-Methods", "GET, POST, OPTIONS"},
    });
    svr.Options(".*", [](const Request&, Response& res) { res.status = 204; });

    install_log_tees(&g_bcast);
    net_monitor().start();
    job_history_init();
    schedules_init();
    backup_state_init();
    lequ_start_scheduler();

    std::cout << "[WEB] listening on 0.0.0.0:" << port
              << " static=" << static_dir << " started " << now_iso8601() << "\n";

    if (!svr.listen("0.0.0.0", port)) {
        std::cerr << "[WEB] failed to bind port " << port << "\n";
        return 1;
    }
    return 0;
}
