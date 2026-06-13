#pragma once
#include <atomic>
#include <initializer_list>
#include <mutex>
#include <set>
#include <string>
#include <vector>

// Per-type cancel flags — set by /api/job/stop?type=...
extern std::atomic<bool> g_cancel_fixbig;
extern std::atomic<bool> g_cancel_botupload;
extern std::atomic<bool> g_cancel_backup;

// Files currently being modified by fix-big. bot-upload consults this set and
// skips matching paths so the two jobs can run in parallel without races.
//
// fix-big calls fixbig_mark_active() when it starts processing a video and
// fixbig_unmark_active() when it's done (use ActivePathGuard for RAII).
extern std::set<std::string> g_fixbig_active;
extern std::mutex             g_fixbig_active_mu;

void fixbig_mark_active(const std::string& p);
void fixbig_unmark_active(const std::string& p);
bool fixbig_is_active(const std::string& p);

// RAII helper: locks several paths for the lifetime of the object.
struct ActivePathGuard {
    std::vector<std::string> paths;
    explicit ActivePathGuard(std::initializer_list<std::string> ps);
    ~ActivePathGuard();
    ActivePathGuard(const ActivePathGuard&) = delete;
    ActivePathGuard& operator=(const ActivePathGuard&) = delete;
};

// Run the embedded HTTP server. Blocks until shutdown signal.
int run_web_server(int port, const std::string& static_dir);
