#pragma once
#include <ctime>
#include <string>
#include <vector>

// Cron-style scheduled task: runs `type` (fix-big / bot-upload) at the
// time(s) matched by the 5-field cron expression. Persisted in web.db.
// Cron parsing supports: minutes, hours, day-of-month, month, day-of-week.
// Each field can be:
//   *           — every value
//   N           — single value
//   N,M,...     — comma list
//   N-M         — inclusive range
//   */K         — every K (step over full range)
//   N-M/K       — step within range
struct ScheduleRow {
    int64_t id = 0;
    std::string name;
    std::string cron;          // 5 fields, space-separated
    std::string type;          // "fix-big" | "bot-upload"
    std::string dirs_json;     // JSON array; empty array = default dir
    bool        enabled = true;
    std::time_t last_run_at = 0;
    std::time_t next_run_at = 0;     // computed best-effort, only for display
};

void schedules_init();        // create table + start scheduler thread
void schedules_stop();

std::vector<ScheduleRow> schedules_list();
int64_t schedules_create(const std::string& name, const std::string& cron,
                          const std::string& type, const std::string& dirs_json,
                          bool enabled);
bool schedules_update(int64_t id, const std::string& name, const std::string& cron,
                       const std::string& type, const std::string& dirs_json,
                       bool enabled);
bool schedules_delete(int64_t id);

// Validate a cron expression — returns empty string if OK, error msg otherwise.
std::string cron_validate(const std::string& expr);

// Test helper: does this expression fire at this timestamp?
bool cron_matches(const std::string& expr, std::time_t when);
