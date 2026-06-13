#include "scheduler.h"
#include "config.h"
#include "log_tee.h"
#include "web_server.h"
#include <atomic>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <set>
#include <sqlite3.h>
#include <sstream>
#include <thread>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using json = nlohmann::json;

// Job launcher exposed by web_server.cpp for the scheduler to call.
// Re-declared here to avoid pulling in heavy headers.
extern void schedule_launch_job(const std::string& type,
                                const std::vector<std::string>& dirs,
                                const std::string& trigger);

namespace {
std::mutex   g_mu;
sqlite3*     g_db = nullptr;
std::atomic<bool> g_running{false};
std::thread  g_worker;

bool exec(const char* sql) {
    char* err = nullptr;
    if (sqlite3_exec(g_db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::cerr << "[SCHED] sql: " << (err ? err : "?") << "\n";
        if (err) sqlite3_free(err);
        return false;
    }
    return true;
}

// Parse one cron field into a set of valid values within [lo, hi].
bool parse_field(const std::string& field, int lo, int hi, std::set<int>& out) {
    out.clear();
    // Split by comma
    std::stringstream ss(field);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        if (tok.empty()) return false;
        int step = 1;
        auto slash = tok.find('/');
        if (slash != std::string::npos) {
            try { step = std::stoi(tok.substr(slash + 1)); }
            catch (...) { return false; }
            if (step <= 0) return false;
            tok = tok.substr(0, slash);
        }
        int a = lo, b = hi;
        if (tok == "*") {
            // a, b already set
        } else if (tok.find('-') != std::string::npos) {
            auto dash = tok.find('-');
            try {
                a = std::stoi(tok.substr(0, dash));
                b = std::stoi(tok.substr(dash + 1));
            } catch (...) { return false; }
        } else {
            try { a = b = std::stoi(tok); }
            catch (...) { return false; }
        }
        if (a < lo || b > hi || a > b) return false;
        for (int v = a; v <= b; v += step) out.insert(v);
    }
    return !out.empty();
}

struct CronParts {
    std::set<int> minute, hour, dom, month, dow;
};

bool parse_cron(const std::string& expr, CronParts& out) {
    std::stringstream ss(expr);
    std::string a, b, c, d, e, extra;
    if (!(ss >> a >> b >> c >> d >> e)) return false;
    if (ss >> extra) return false;        // 6th field — reject
    if (!parse_field(a, 0, 59, out.minute)) return false;
    if (!parse_field(b, 0, 23, out.hour))   return false;
    if (!parse_field(c, 1, 31, out.dom))    return false;
    if (!parse_field(d, 1, 12, out.month))  return false;
    if (!parse_field(e, 0, 7,  out.dow))    return false;
    // Sun is both 0 and 7 in cron — normalize to 0
    if (out.dow.count(7)) { out.dow.erase(7); out.dow.insert(0); }
    return true;
}
}  // namespace

std::string cron_validate(const std::string& expr) {
    CronParts p;
    if (!parse_cron(expr, p))
        return "invalid cron — expected 5 fields (m h dom mon dow)";
    return "";
}

bool cron_matches(const std::string& expr, std::time_t when) {
    CronParts p;
    if (!parse_cron(expr, p)) return false;
    std::tm tm_buf{};
    localtime_r(&when, &tm_buf);
    if (!p.minute.count(tm_buf.tm_min)) return false;
    if (!p.hour.count(tm_buf.tm_hour)) return false;
    if (!p.month.count(tm_buf.tm_mon + 1)) return false;
    // Standard cron quirk: if BOTH dom and dow are restricted (i.e. not "*"),
    // the trigger fires when EITHER matches. We treat unrestricted as all-set
    // (size==full-range) — close enough for our purposes.
    bool dom_ok = p.dom.count(tm_buf.tm_mday);
    bool dow_ok = p.dow.count(tm_buf.tm_wday);
    bool dom_unr = (p.dom.size() == 31);
    bool dow_unr = (p.dow.size() == 7);
    if (dom_unr && dow_unr) return true;
    if (dom_unr) return dow_ok;
    if (dow_unr) return dom_ok;
    return dom_ok || dow_ok;
}

void schedules_init() {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_db) return;
        fs::create_directories(get_base_dir() + "/data");
        std::string path = get_base_dir() + "/data/web.db";
        if (sqlite3_open(path.c_str(), &g_db) != SQLITE_OK) {
            std::cerr << "[SCHED] cannot open db: " << sqlite3_errmsg(g_db) << "\n";
            sqlite3_close(g_db); g_db = nullptr;
            return;
        }
        sqlite3_busy_timeout(g_db, 5000);
        exec(
            "CREATE TABLE IF NOT EXISTS schedules ("
            "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  name TEXT NOT NULL,"
            "  cron TEXT NOT NULL,"
            "  type TEXT NOT NULL,"
            "  dirs_json TEXT NOT NULL DEFAULT '[]',"
            "  enabled INTEGER NOT NULL DEFAULT 1,"
            "  last_run_at INTEGER NOT NULL DEFAULT 0"
            ");"
        );
    }

    // Start scheduler thread
    if (g_running.exchange(true)) return;
    g_worker = std::thread([] {
        set_log_channel("web");
        std::time_t last_check = 0;
        while (g_running.load()) {
            std::time_t now = std::time(nullptr);
            // Tick at the next whole minute boundary
            std::time_t minute_start = (now / 60) * 60;
            if (minute_start != last_check) {
                last_check = minute_start;
                std::vector<ScheduleRow> rows = schedules_list();
                for (auto& s : rows) {
                    if (!s.enabled) continue;
                    if (!cron_matches(s.cron, minute_start)) continue;
                    if (s.last_run_at >= minute_start) continue;  // already fired

                    std::cout << "[SCHED] firing '" << s.name << "' ("
                              << s.cron << ") → " << s.type << "\n";
                    std::vector<std::string> dirs;
                    try {
                        auto j = json::parse(s.dirs_json);
                        if (j.is_array()) {
                            for (auto& d : j) if (d.is_string()) dirs.push_back(d.get<std::string>());
                        }
                    } catch (...) {}
                    schedule_launch_job(s.type, dirs, "schedule:" + s.name);

                    // Mark fired
                    std::lock_guard<std::mutex> lk(g_mu);
                    if (g_db) {
                        sqlite3_stmt* st = nullptr;
                        sqlite3_prepare_v2(g_db, "UPDATE schedules SET last_run_at=? WHERE id=?;",
                                          -1, &st, nullptr);
                        sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(minute_start));
                        sqlite3_bind_int64(st, 2, s.id);
                        sqlite3_step(st);
                        sqlite3_finalize(st);
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::seconds(5));
        }
    });
}

void schedules_stop() {
    g_running.store(false);
    if (g_worker.joinable()) g_worker.join();
}

std::vector<ScheduleRow> schedules_list() {
    std::vector<ScheduleRow> out;
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_db) return out;
    sqlite3_stmt* st = nullptr;
    const char* sql = "SELECT id,name,cron,type,dirs_json,enabled,last_run_at "
                      "FROM schedules ORDER BY id;";
    if (sqlite3_prepare_v2(g_db, sql, -1, &st, nullptr) != SQLITE_OK) return out;
    while (sqlite3_step(st) == SQLITE_ROW) {
        ScheduleRow r;
        r.id          = sqlite3_column_int64(st, 0);
        r.name        = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
        r.cron        = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
        r.type        = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
        r.dirs_json   = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
        r.enabled     = sqlite3_column_int(st, 5) != 0;
        r.last_run_at = sqlite3_column_int64(st, 6);
        out.push_back(std::move(r));
    }
    sqlite3_finalize(st);
    return out;
}

int64_t schedules_create(const std::string& name, const std::string& cron_expr,
                          const std::string& type, const std::string& dirs_json,
                          bool enabled) {
    if (!cron_validate(cron_expr).empty()) return 0;
    if (type != "fix-big" && type != "bot-upload") return 0;
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_db) return 0;
    sqlite3_stmt* st = nullptr;
    const char* sql = "INSERT INTO schedules(name,cron,type,dirs_json,enabled) VALUES(?,?,?,?,?);";
    if (sqlite3_prepare_v2(g_db, sql, -1, &st, nullptr) != SQLITE_OK) return 0;
    sqlite3_bind_text(st, 1, name.c_str(),     -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, cron_expr.c_str(),-1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, type.c_str(),     -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, dirs_json.c_str(),-1, SQLITE_TRANSIENT);
    sqlite3_bind_int (st, 5, enabled ? 1 : 0);
    int64_t id = 0;
    if (sqlite3_step(st) == SQLITE_DONE) id = sqlite3_last_insert_rowid(g_db);
    sqlite3_finalize(st);
    return id;
}

bool schedules_update(int64_t id, const std::string& name, const std::string& cron_expr,
                       const std::string& type, const std::string& dirs_json,
                       bool enabled) {
    if (!cron_validate(cron_expr).empty()) return false;
    if (type != "fix-big" && type != "bot-upload") return false;
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_db) return false;
    sqlite3_stmt* st = nullptr;
    const char* sql = "UPDATE schedules SET name=?,cron=?,type=?,dirs_json=?,enabled=? WHERE id=?;";
    if (sqlite3_prepare_v2(g_db, sql, -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(st, 1, name.c_str(),     -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, cron_expr.c_str(),-1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, type.c_str(),     -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, dirs_json.c_str(),-1, SQLITE_TRANSIENT);
    sqlite3_bind_int (st, 5, enabled ? 1 : 0);
    sqlite3_bind_int64(st, 6, id);
    bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(g_db) > 0;
    sqlite3_finalize(st);
    return ok;
}

bool schedules_delete(int64_t id) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_db) return false;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(g_db, "DELETE FROM schedules WHERE id=?;", -1, &st, nullptr) != SQLITE_OK)
        return false;
    sqlite3_bind_int64(st, 1, id);
    bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(g_db) > 0;
    sqlite3_finalize(st);
    return ok;
}
