#include "backup.h"
#include "bot_client.h"
#include "config.h"
#include "scanner.h"
#include "utils.h"
#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <fcntl.h>
#include <sqlite3.h>
#include <sys/stat.h>
#include <thread>
#include <unordered_set>
#include <unistd.h>

namespace fs = std::filesystem;
using json = nlohmann::json;

// ===================== progress singleton =====================

void BackupProgress::reset() {
    running.store(false);
    total.store(0); done.store(0); skipped.store(0); failed.store(0);
    started_at.store(0); finished_at.store(0);
    std::lock_guard<std::mutex> lk(mu);
    scope.clear(); current_model.clear(); last_error.clear();
}

BackupProgress& backup_progress() {
    static BackupProgress p;
    return p;
}

// ===================== web.db (backup_state) =====================

namespace {
std::mutex g_wmu;
sqlite3*   g_wdb = nullptr;

std::string default_source_channel();

// Caller must hold g_wmu.
bool ensure_web_db_locked() {
    if (g_wdb) return true;
    fs::create_directories(get_base_dir() + "/data");
    std::string path = get_base_dir() + "/data/web.db";
    if (sqlite3_open(path.c_str(), &g_wdb) != SQLITE_OK) {
        std::cerr << "[BACKUP] cannot open web.db: "
                  << (g_wdb ? sqlite3_errmsg(g_wdb) : "?") << "\n";
        if (g_wdb) { sqlite3_close(g_wdb); g_wdb = nullptr; }
        return false;
    }
    sqlite3_busy_timeout(g_wdb, 10000);
    sqlite3_exec(g_wdb, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(g_wdb,
        "CREATE TABLE IF NOT EXISTS backup_state ("
        "  source_channel_id TEXT NOT NULL DEFAULT '',"
        "  backup_channel_id TEXT NOT NULL,"
        "  url_id            INTEGER NOT NULL,"
        "  model_name        TEXT NOT NULL DEFAULT '',"
        "  dest_msg_id       INTEGER NOT NULL DEFAULT 0,"
        "  at                INTEGER NOT NULL DEFAULT 0,"
        "  PRIMARY KEY (source_channel_id, backup_channel_id, url_id)"
        ");", nullptr, nullptr, nullptr);
    sqlite3_stmt* info = nullptr;
    bool has_source = false;
    if (sqlite3_prepare_v2(g_wdb, "PRAGMA table_info(backup_state);", -1, &info, nullptr) == SQLITE_OK) {
        while (sqlite3_step(info) == SQLITE_ROW) {
            const unsigned char* name = sqlite3_column_text(info, 1);
            if (name && std::string(reinterpret_cast<const char*>(name)) == "source_channel_id") {
                has_source = true;
                break;
            }
        }
    }
    sqlite3_finalize(info);
    if (!has_source) {
        std::string src = default_source_channel();
        char* err = nullptr;
        sqlite3_exec(g_wdb, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr);
        sqlite3_exec(g_wdb, "ALTER TABLE backup_state RENAME TO backup_state_old;", nullptr, nullptr, &err);
        if (err) { sqlite3_free(err); err = nullptr; sqlite3_exec(g_wdb, "ROLLBACK;", nullptr, nullptr, nullptr); return false; }
        sqlite3_exec(g_wdb,
            "CREATE TABLE backup_state ("
            "  source_channel_id TEXT NOT NULL DEFAULT '',"
            "  backup_channel_id TEXT NOT NULL,"
            "  url_id            INTEGER NOT NULL,"
            "  model_name        TEXT NOT NULL DEFAULT '',"
            "  dest_msg_id       INTEGER NOT NULL DEFAULT 0,"
            "  at                INTEGER NOT NULL DEFAULT 0,"
            "  PRIMARY KEY (source_channel_id, backup_channel_id, url_id)"
            ");", nullptr, nullptr, &err);
        if (!err) {
            sqlite3_stmt* ins = nullptr;
            if (sqlite3_prepare_v2(g_wdb,
                    "INSERT OR IGNORE INTO backup_state"
                    "(source_channel_id,backup_channel_id,url_id,model_name,dest_msg_id,at) "
                    "SELECT ?,backup_channel_id,url_id,model_name,dest_msg_id,at FROM backup_state_old;",
                    -1, &ins, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(ins, 1, src.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_step(ins);
            }
            sqlite3_finalize(ins);
            sqlite3_exec(g_wdb, "DROP TABLE backup_state_old;", nullptr, nullptr, nullptr);
            sqlite3_exec(g_wdb, "COMMIT;", nullptr, nullptr, nullptr);
        } else {
            sqlite3_free(err);
            sqlite3_exec(g_wdb, "ROLLBACK;", nullptr, nullptr, nullptr);
            return false;
        }
    }
    sqlite3_exec(g_wdb,
        "CREATE INDEX IF NOT EXISTS idx_backup_state_model "
        "ON backup_state(source_channel_id, backup_channel_id, model_name);", nullptr, nullptr, nullptr);
    return true;
}

bool record_backed(const std::string& src, const std::string& ch, int64_t url_id,
                   const std::string& model, int64_t dest, int64_t at,
                   std::string& err) {
    std::lock_guard<std::mutex> lk(g_wmu);
    if (!ensure_web_db_locked()) { err = "cannot open or migrate web.db"; return false; }
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(g_wdb,
            "INSERT OR IGNORE INTO backup_state"
            "(source_channel_id,backup_channel_id,url_id,model_name,dest_msg_id,at) VALUES(?,?,?,?,?,?);",
            -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_text (st, 1, src.c_str(),   -1, SQLITE_TRANSIENT);
        sqlite3_bind_text (st, 2, ch.c_str(),    -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 3, url_id);
        sqlite3_bind_text (st, 4, model.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 5, dest);
        sqlite3_bind_int64(st, 6, at);
        int rc = sqlite3_step(st);
        if (rc != SQLITE_DONE) {
            err = sqlite3_errmsg(g_wdb);
            sqlite3_finalize(st);
            return false;
        }
    } else {
        err = sqlite3_errmsg(g_wdb);
        sqlite3_finalize(st);
        return false;
    }
    sqlite3_finalize(st);
    return true;
}

std::unordered_set<int64_t> load_backed(const std::string& src, const std::string& ch) {
    std::unordered_set<int64_t> s;
    std::lock_guard<std::mutex> lk(g_wmu);
    if (!ensure_web_db_locked()) return s;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(g_wdb,
            "SELECT url_id FROM backup_state WHERE source_channel_id=? AND backup_channel_id=?;",
            -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, src.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, ch.c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(st) == SQLITE_ROW) s.insert(sqlite3_column_int64(st, 0));
    }
    sqlite3_finalize(st);
    return s;
}

// ---- scanner.db (read-only source of truth) ----

sqlite3* open_scanner_ro() {
    sqlite3* sdb = nullptr;
    std::string sp = get_base_dir() + "/data/scanner.db";
    if (sqlite3_open_v2(sp.c_str(), &sdb, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        if (sdb) { sqlite3_close(sdb); sdb = nullptr; }
        return nullptr;
    }
    sqlite3_busy_timeout(sdb, 10000);
    return sdb;
}

std::vector<std::string> scanner_all_models(sqlite3* sdb) {
    std::vector<std::string> v;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(sdb,
            "SELECT model_name, COUNT(*) c FROM videos GROUP BY model_name ORDER BY c DESC;",
            -1, &st, nullptr) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            const unsigned char* m = sqlite3_column_text(st, 0);
            if (m) v.emplace_back(reinterpret_cast<const char*>(m));
        }
    }
    sqlite3_finalize(st);
    return v;
}

int scanner_count_model(sqlite3* sdb, const std::string& model) {
    sqlite3_stmt* st = nullptr; int n = 0;
    if (sqlite3_prepare_v2(sdb, "SELECT COUNT(*) FROM videos WHERE model_name=?;",
                           -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, model.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
    }
    sqlite3_finalize(st);
    return n;
}

std::vector<int64_t> scanner_urls_for_model(sqlite3* sdb, const std::string& model) {
    std::vector<int64_t> v;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(sdb,
            "SELECT url_id FROM videos WHERE model_name=? ORDER BY url_id ASC;",
            -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, model.c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(st) == SQLITE_ROW) v.push_back(sqlite3_column_int64(st, 0));
    }
    sqlite3_finalize(st);
    return v;
}

void isleep_ms(int ms, std::atomic<bool>& cancel) {
    const int step = 100;
    for (int e = 0; e < ms; e += step) {
        if (cancel.load()) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(std::min(step, ms - e)));
    }
}

std::string default_source_channel() {
    std::ifstream f(get_base_dir() + "/.account_configs/bots.json");
    if (!f) return "";
    try { json j; f >> j; return j.value("channel_id", ""); }
    catch (...) { return ""; }
}
}  // namespace

void backup_state_init() {
    std::lock_guard<std::mutex> lk(g_wmu);
    ensure_web_db_locked();
}

// ===================== config =====================

BackupConfig backup_config_load() {
    BackupConfig c;
    c.source_channel_id = default_source_channel();   // sensible default
    std::ifstream f(get_base_dir() + "/.account_configs/backup.json");
    if (!f) return c;
    try {
        json j; f >> j;
        c.api_url           = j.value("api_url", c.api_url);
        c.bot_name          = j.value("bot_name", "");
        c.bot_token         = j.value("bot_token", "");
        c.source_channel_id = j.value("source_channel_id", c.source_channel_id);
        c.backup_channel_id = j.value("backup_channel_id", "");
        c.delay_ms          = j.value("delay_ms", 1500);
        c.loaded            = true;
    } catch (...) {}
    return c;
}

bool backup_config_save(const BackupConfig& cfg, std::string& err) {
    json j;
    j["api_url"]           = cfg.api_url;
    j["bot_name"]          = cfg.bot_name;
    j["bot_token"]         = cfg.bot_token;
    j["source_channel_id"] = cfg.source_channel_id;
    j["backup_channel_id"] = cfg.backup_channel_id;
    j["delay_ms"]          = cfg.delay_ms;

    std::string dir = get_base_dir() + "/.account_configs";
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::string path = dir + "/backup.json";
    std::string tmp  = path + ".tmp";
    std::string body = j.dump(2);
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { err = "cannot write " + tmp; return false; }
    const char* p = body.data();
    size_t left = body.size();
    while (left > 0) {
        ssize_t n = ::write(fd, p, left);
        if (n <= 0) {
            ::close(fd);
            err = "write failed: " + tmp;
            return false;
        }
        p += n;
        left -= static_cast<size_t>(n);
    }
    if (::close(fd) != 0) { err = "close failed: " + tmp; return false; }
    fs::rename(tmp, path, ec);
    if (ec) { err = "rename failed: " + ec.message(); return false; }
    ::chmod(path.c_str(), 0600);   // contains a bot token
    return true;
}

json backup_config_public() {
    BackupConfig c = backup_config_load();
    return json{
        {"api_url",           c.api_url},
        {"bot_name",          c.bot_name},
        {"has_token",         !c.bot_token.empty()},
        {"source_channel_id", c.source_channel_id},
        {"backup_channel_id", c.backup_channel_id},
        {"delay_ms",          c.delay_ms},
        {"configured",        c.loaded && !c.bot_token.empty() && !c.backup_channel_id.empty()},
    };
}

json backup_config_verify(const BackupConfig& cfg) {
    json out;
    out["ok"] = false;
    if (cfg.bot_token.empty()) { out["error"] = "no bot token"; return out; }

    BotClient client(cfg.api_url, cfg.bot_token);
    BotIdentity me = client.get_me();
    out["bot"] = json{{"ok", me.ok}, {"id", me.id}, {"username", me.username}, {"error", me.error}};
    if (!me.ok) { out["error"] = "getMe failed: " + me.error; return out; }

    auto probe = [&](const std::string& chat_id) -> json {
        json c;
        if (chat_id.empty()) { c["ok"] = false; c["error"] = "empty chat_id"; return c; }
        ChatAccess ca = client.get_chat(chat_id);
        c["reachable"] = ca.ok;
        c["title"] = ca.title;
        c["type"]  = ca.type;
        if (!ca.ok) { c["ok"] = false; c["error"] = ca.error; return c; }
        MemberStatus ms = client.get_chat_member(chat_id, me.id);
        c["status"]   = ms.status;
        c["can_post"] = ms.can_post;
        if (!ms.ok) { c["ok"] = false; c["error"] = ms.error; return c; }
        c["ok"] = ms.can_post;
        if (!ms.can_post) c["error"] = "bot is not an admin with post rights here";
        return c;
    };
    json src = probe(cfg.source_channel_id);
    json dst = probe(cfg.backup_channel_id);
    out["source"] = src;
    out["backup"] = dst;
    out["ok"] = src.value("ok", false) && dst.value("ok", false);
    return out;
}

// ===================== per-model rollup =====================

json backup_models(const std::string& source_channel_id,
                   const std::string& backup_channel_id) {
    json arr = json::array();

    sqlite3* sdb = open_scanner_ro();
    if (!sdb) return arr;
    std::map<std::string, int> total;
    {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(sdb,
                "SELECT model_name, COUNT(*) FROM videos GROUP BY model_name;",
                -1, &st, nullptr) == SQLITE_OK) {
            while (sqlite3_step(st) == SQLITE_ROW) {
                const unsigned char* m = sqlite3_column_text(st, 0);
                total[m ? reinterpret_cast<const char*>(m) : ""] = sqlite3_column_int(st, 1);
            }
        }
        sqlite3_finalize(st);
    }
    sqlite3_close(sdb);

    std::map<std::string, int> done;
    {
        std::lock_guard<std::mutex> lk(g_wmu);
        if (ensure_web_db_locked() && !backup_channel_id.empty()) {
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(g_wdb,
                    "SELECT model_name, COUNT(*) FROM backup_state "
                    "WHERE source_channel_id=? AND backup_channel_id=? GROUP BY model_name;",
                    -1, &st, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(st, 1, source_channel_id.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(st, 2, backup_channel_id.c_str(), -1, SQLITE_TRANSIENT);
                while (sqlite3_step(st) == SQLITE_ROW) {
                    const unsigned char* m = sqlite3_column_text(st, 0);
                    done[m ? reinterpret_cast<const char*>(m) : ""] = sqlite3_column_int(st, 1);
                }
            }
            sqlite3_finalize(st);
        }
    }

    std::vector<std::tuple<std::string, int, int>> rows;
    rows.reserve(total.size());
    for (auto& kv : total) {
        auto it = done.find(kv.first);
        rows.emplace_back(kv.first, kv.second, it == done.end() ? 0 : it->second);
    }
    std::sort(rows.begin(), rows.end(),
              [](const auto& a, const auto& b) { return std::get<1>(a) > std::get<1>(b); });
    for (auto& r : rows) {
        arr.push_back(json{
            {"model",     std::get<0>(r)},
            {"total",     std::get<1>(r)},
            {"backed_up", std::get<2>(r)},
        });
    }
    return arr;
}

// ===================== engine =====================

bool run_backup(const std::string& scope, const std::vector<std::string>& models,
                std::atomic<bool>& cancel) {
    auto& P = backup_progress();
    P.reset();
    P.running.store(true);
    P.started_at.store(static_cast<int64_t>(std::time(nullptr)));
    { std::lock_guard<std::mutex> lk(P.mu); P.scope = scope; }

    auto fail = [&](const std::string& msg) -> bool {
        std::cerr << "\033[31m[ERROR]\033[0m [BACKUP] " << msg << "\n";
        { std::lock_guard<std::mutex> lk(P.mu); P.last_error = msg; }
        P.finished_at.store(static_cast<int64_t>(std::time(nullptr)));
        P.running.store(false);
        return false;
    };

    BackupConfig cfg = backup_config_load();
    if (cfg.bot_token.empty())         return fail("backup bot not configured");
    if (cfg.source_channel_id.empty()) return fail("source channel not set");
    if (cfg.backup_channel_id.empty()) return fail("backup channel not set");

    std::cout << "[BACKUP] preflight: full source-channel scan before backup...\n";
    ChannelScanResult scan = scan_source_channel_for_backup(cfg.source_channel_id, true, false);
    if (!scan.ok) return fail("pre-backup scan failed: " + scan.error);
    std::cout << "[BACKUP] preflight scan complete via account=" << scan.account_name
              << ", inserted=" << scan.inserted << "\n";

    sqlite3* sdb = open_scanner_ro();
    if (!sdb) return fail("cannot open scanner.db (read-only)");

    std::vector<std::string> todo = (scope == "all") ? scanner_all_models(sdb) : models;
    if (todo.empty()) { sqlite3_close(sdb); return fail("no models selected"); }

    int total = 0;
    for (auto& m : todo) total += scanner_count_model(sdb, m);
    P.total.store(total);

    auto backed = load_backed(cfg.source_channel_id, cfg.backup_channel_id);

    std::cout << "[BACKUP] start scope=" << scope << " models=" << todo.size()
              << " videos=" << total
              << " (already in backup channel: " << backed.size() << ")\n";
    std::cout << "[BACKUP] source=" << cfg.source_channel_id
              << " → backup=" << cfg.backup_channel_id
              << " throttle=" << cfg.delay_ms << "ms\n";

    BotClient client(cfg.api_url, cfg.bot_token);

    for (auto& model : todo) {
        if (cancel.load()) break;
        { std::lock_guard<std::mutex> lk(P.mu); P.current_model = model; }
        auto urls = scanner_urls_for_model(sdb, model);
        std::cout << "[BACKUP] model " << model << " (" << urls.size() << " videos)\n";

        for (int64_t url_id : urls) {
            if (cancel.load()) break;
            if (backed.count(url_id)) { P.skipped.fetch_add(1); continue; }

            bool accounted = false;
            std::string last_error;
            for (int attempt = 0; attempt < 6 && !cancel.load(); attempt++) {
                BotApiResponse resp =
                    client.copy_message(cfg.source_channel_id, url_id, cfg.backup_channel_id);
                if (resp.ok) {
                    std::string db_err;
                    if (!record_backed(cfg.source_channel_id, cfg.backup_channel_id, url_id,
                                       model, resp.message_id,
                                       static_cast<int64_t>(std::time(nullptr)), db_err)) {
                        last_error = "state write failed: " + db_err;
                        std::cerr << "\033[31m[ERROR]\033[0m [BACKUP] ✗ " << model
                                  << " #" << url_id << ": " << last_error << "\n";
                        P.failed.fetch_add(1);
                        { std::lock_guard<std::mutex> lk(P.mu); P.last_error = last_error; }
                        accounted = true;
                        break;
                    }
                    backed.insert(url_id);
                    P.done.fetch_add(1);
                    accounted = true;
                    std::cout << "[BACKUP] ✓ " << model << " #" << url_id
                              << " → " << resp.message_id
                              << "  [" << (P.done.load() + P.skipped.load())
                              << "/" << P.total.load() << "]\n";
                    break;
                } else if (resp.retry_after > 0) {
                    last_error = resp.error.empty() ? "FLOOD_WAIT retry exhausted" : resp.error;
                    std::cout << "\033[33m[WARN]\033[0m [BACKUP] FLOOD_WAIT "
                              << resp.retry_after << "s (" << model << " #" << url_id << ")\n";
                    isleep_ms((resp.retry_after + 1) * 1000, cancel);
                    continue;  // retry same message
                } else {
                    std::cerr << "\033[31m[ERROR]\033[0m [BACKUP] ✗ " << model
                              << " #" << url_id << ": " << resp.error << "\n";
                    P.failed.fetch_add(1);
                    { std::lock_guard<std::mutex> lk(P.mu); P.last_error = resp.error; }
                    accounted = true;
                    break;  // give up on this one this run
                }
            }
            if (!accounted && !cancel.load()) {
                if (last_error.empty()) last_error = "copy retry exhausted";
                std::cerr << "\033[31m[ERROR]\033[0m [BACKUP] ✗ " << model
                          << " #" << url_id << ": " << last_error << "\n";
                P.failed.fetch_add(1);
                { std::lock_guard<std::mutex> lk(P.mu); P.last_error = last_error; }
            }
            if (!cancel.load()) isleep_ms(cfg.delay_ms, cancel);
        }
    }

    sqlite3_close(sdb);

    double el = static_cast<double>(std::time(nullptr) - P.started_at.load());
    std::cout << "[BACKUP] " << (cancel.load() ? "stopped" : "done")
              << ": copied " << P.done.load()
              << ", skipped " << P.skipped.load()
              << ", failed " << P.failed.load()
              << " of " << P.total.load()
              << " in " << fmt_elapsed(el) << "\n";

    P.finished_at.store(static_cast<int64_t>(std::time(nullptr)));
    { std::lock_guard<std::mutex> lk(P.mu); P.current_model.clear(); }
    P.running.store(false);
    return true;
}
