#include "job_history.h"
#include "config.h"
#include <filesystem>
#include <iostream>
#include <mutex>
#include <sqlite3.h>

namespace fs = std::filesystem;

namespace {
std::mutex g_mu;
sqlite3*   g_db = nullptr;

bool exec(const char* sql) {
    char* err = nullptr;
    if (sqlite3_exec(g_db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::cerr << "[JOB-HIST] sql error: " << (err ? err : "?") << "\n";
        if (err) sqlite3_free(err);
        return false;
    }
    return true;
}
}  // namespace

void job_history_init() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_db) return;
    fs::create_directories(get_base_dir() + "/data");
    std::string path = get_base_dir() + "/data/web.db";
    if (sqlite3_open(path.c_str(), &g_db) != SQLITE_OK) {
        std::cerr << "[JOB-HIST] cannot open " << path << ": " << sqlite3_errmsg(g_db) << "\n";
        sqlite3_close(g_db);
        g_db = nullptr;
        return;
    }
    sqlite3_busy_timeout(g_db, 5000);
    exec("PRAGMA journal_mode=WAL;");
    exec(
        "CREATE TABLE IF NOT EXISTS job_history ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  type TEXT NOT NULL,"
        "  trigger TEXT NOT NULL DEFAULT 'manual',"
        "  started_at INTEGER NOT NULL,"
        "  finished_at INTEGER NOT NULL DEFAULT 0,"
        "  status TEXT NOT NULL DEFAULT 'running',"
        "  success_count INTEGER NOT NULL DEFAULT 0,"
        "  failed_count INTEGER NOT NULL DEFAULT 0,"
        "  dirs_json TEXT NOT NULL DEFAULT '[]',"
        "  note TEXT NOT NULL DEFAULT ''"
        ");"
    );
    exec("CREATE INDEX IF NOT EXISTS idx_job_history_started ON job_history(started_at DESC);");
    std::cout << "[JOB-HIST] db ready at " << path << "\n";
}

int64_t job_history_start(const std::string& type, const std::string& trigger,
                           const std::string& dirs_json) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_db) return 0;
    sqlite3_stmt* st = nullptr;
    const char* sql = "INSERT INTO job_history(type,trigger,started_at,status,dirs_json) "
                      "VALUES(?,?,?,'running',?);";
    if (sqlite3_prepare_v2(g_db, sql, -1, &st, nullptr) != SQLITE_OK) return 0;
    sqlite3_bind_text(st, 1, type.c_str(),    -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, trigger.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 3, static_cast<sqlite3_int64>(std::time(nullptr)));
    sqlite3_bind_text(st, 4, dirs_json.c_str(), -1, SQLITE_TRANSIENT);
    int64_t id = 0;
    if (sqlite3_step(st) == SQLITE_DONE) {
        id = sqlite3_last_insert_rowid(g_db);
    }
    sqlite3_finalize(st);
    return id;
}

void job_history_finish(int64_t id, const std::string& status, int success, int failed,
                        const std::string& note) {
    if (id == 0) return;
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_db) return;
    sqlite3_stmt* st = nullptr;
    const char* sql = "UPDATE job_history SET finished_at=?,status=?,success_count=?,"
                      "failed_count=?,note=? WHERE id=?;";
    if (sqlite3_prepare_v2(g_db, sql, -1, &st, nullptr) != SQLITE_OK) return;
    sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(std::time(nullptr)));
    sqlite3_bind_text (st, 2, status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int  (st, 3, success);
    sqlite3_bind_int  (st, 4, failed);
    sqlite3_bind_text (st, 5, note.c_str(),   -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 6, id);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

std::vector<JobHistoryRow> job_history_list(int limit) {
    std::vector<JobHistoryRow> out;
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_db) return out;
    sqlite3_stmt* st = nullptr;
    const char* sql = "SELECT id,type,trigger,started_at,finished_at,status,"
                      "success_count,failed_count,dirs_json,note "
                      "FROM job_history ORDER BY started_at DESC LIMIT ?;";
    if (sqlite3_prepare_v2(g_db, sql, -1, &st, nullptr) != SQLITE_OK) return out;
    sqlite3_bind_int(st, 1, limit);
    while (sqlite3_step(st) == SQLITE_ROW) {
        JobHistoryRow r;
        r.id            = sqlite3_column_int64(st, 0);
        r.type          = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
        r.trigger       = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
        r.started_at    = sqlite3_column_int64(st, 3);
        r.finished_at   = sqlite3_column_int64(st, 4);
        r.status        = reinterpret_cast<const char*>(sqlite3_column_text(st, 5));
        r.success_count = sqlite3_column_int(st, 6);
        r.failed_count  = sqlite3_column_int(st, 7);
        r.dirs_json     = reinterpret_cast<const char*>(sqlite3_column_text(st, 8));
        r.note          = reinterpret_cast<const char*>(sqlite3_column_text(st, 9));
        out.push_back(std::move(r));
    }
    sqlite3_finalize(st);
    return out;
}
