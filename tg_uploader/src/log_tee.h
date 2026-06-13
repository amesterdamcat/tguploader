#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <streambuf>
#include <string>
#include <unordered_map>
#include <vector>

// Channel name set/get for the current thread. Lines emitted by cout/cerr
// while this thread is running will be tagged with this channel — letting
// the frontend split fix-big logs from bot-upload logs from misc web logs.
//   web         (default) — server lifecycle, auth, etc.
//   fix-big     — set on the task thread + ffmpeg subprocess output
//   bot-upload  — set on the bot worker threads
void        set_log_channel(const std::string& ch);
std::string get_log_channel();

// LogBroadcaster — multi-channel history + fan-out queue for SSE clients.
class LogBroadcaster {
public:
    struct Line {
        std::string channel;   // "web", "fix-big", "bot-upload"
        std::string text;      // already ANSI-stripped, optionally \r-prefixed for progress
    };

    struct Subscriber {
        // Empty channel set = subscribe to *all* channels.
        std::string             channel;
        std::mutex              mu;
        std::condition_variable cv;
        std::deque<Line>        q;
        std::atomic<bool>       dead{false};

        bool wait_pop(Line& out, int timeout_ms);
    };

    // Push a raw line. `channel` defaults to current thread's channel.
    void push_line(const std::string& raw);
    void push_line(const std::string& channel, const std::string& raw);

    std::shared_ptr<Subscriber> subscribe(const std::string& channel_filter = "");
    void unsubscribe(const std::shared_ptr<Subscriber>& s);

    // History of a single channel (or all if channel.empty()).
    std::vector<Line> snapshot(const std::string& channel, size_t max_lines = 500) const;

    // Stats: total lines stored, plus per-channel counts.
    size_t history_count() const;
    std::map<std::string, size_t> channel_counts() const;

private:
    mutable std::mutex                                  mu_;
    std::unordered_map<std::string, std::deque<Line>>   history_;   // per-channel
    std::list<std::shared_ptr<Subscriber>>              subs_;
    static constexpr size_t HISTORY_CAP = 2000;     // per channel
};

// TeeStreambuf — intercepts cout/cerr; channel is read from thread_local on flush.
class TeeStreambuf : public std::streambuf {
public:
    TeeStreambuf(std::streambuf* original, LogBroadcaster* bcast);
protected:
    int_type overflow(int_type c) override;
    int sync() override;
private:
    void flush_line_locked();
    std::streambuf* original_;
    LogBroadcaster* bcast_;
    std::string line_buf_;
    std::mutex mu_;
};

void install_log_tees(LogBroadcaster* bcast);
