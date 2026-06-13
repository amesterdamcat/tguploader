#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

// Channel backup: copy already-uploaded videos from the source channel into a
// separate "backup" channel using the Bot API's server-side copyMessage. The
// list of messages to copy comes from scanner.db (its `url_id` column is the
// Bot-API message id in the source channel). Progress is persisted in web.db so
// a run can be stopped and resumed without re-copying.

struct BackupConfig {
    std::string api_url = "http://127.0.0.1:8081";
    std::string bot_name;             // cosmetic label
    std::string bot_token;            // SECRET — never returned to the client
    std::string source_channel_id;    // e.g. -1001810923743 (defaults from bots.json)
    std::string backup_channel_id;    // the new channel to copy into
    int         delay_ms = 1500;      // throttle between copies (~ rate limit guard)
    bool        loaded = false;
};

BackupConfig    backup_config_load();
bool            backup_config_save(const BackupConfig& cfg, std::string& err);
nlohmann::json  backup_config_public();                 // redacted (no token)
nlohmann::json  backup_config_verify(const BackupConfig& cfg);  // probe both channels

void            backup_state_init();                    // create web.db table
nlohmann::json  backup_models(const std::string& source_channel_id,
                              const std::string& backup_channel_id);  // per-model rollup

// Live progress for /api/backup/status. Singleton.
struct BackupProgress {
    std::atomic<bool>    running{false};
    std::atomic<int>     total{0};
    std::atomic<int>     done{0};       // newly copied this run
    std::atomic<int>     skipped{0};    // already backed up (resume)
    std::atomic<int>     failed{0};
    std::atomic<int64_t> started_at{0};
    std::atomic<int64_t> finished_at{0};
    std::mutex           mu;            // guards the strings below
    std::string          scope;
    std::string          current_model;
    std::string          last_error;
    void reset();
};
BackupProgress& backup_progress();

// Engine. Blocks until finished or `cancel` is set. Logs via std::cout, so the
// caller should set_log_channel("backup") on the running thread first.
// scope: "all" (every model) or "models" (only those in `models`).
// Returns true when the run completed or was canceled cleanly after doing work.
// Returns false for fatal setup/runtime failures that should be recorded as a
// failed job rather than a successful no-op.
bool run_backup(const std::string& scope, const std::vector<std::string>& models,
                std::atomic<bool>& cancel);
