#pragma once
#include <ctime>
#include <string>
#include <vector>

// Persistent record of jobs run by the web control panel. Stored in
// $CTG_BASE_DIR/data/web.db (separate from scanner.db so we never lock
// the scanner side).
struct JobHistoryRow {
    int64_t id = 0;
    std::string type;          // "fix-big" | "bot-upload"
    std::string trigger;       // "manual" | "schedule:<name>"
    std::time_t started_at = 0;
    std::time_t finished_at = 0;
    std::string status;        // "running" | "done" | "canceled" | "crashed"
    int   success_count = 0;
    int   failed_count  = 0;
    std::string dirs_json;     // JSON array of dirs (or single dir as one-elem)
    std::string note;          // optional extra info / error message
};

void   job_history_init();
int64_t job_history_start(const std::string& type, const std::string& trigger,
                          const std::string& dirs_json);
void   job_history_finish(int64_t id, const std::string& status,
                          int success, int failed, const std::string& note);
std::vector<JobHistoryRow> job_history_list(int limit = 50);
