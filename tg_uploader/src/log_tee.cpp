#include "log_tee.h"
#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <regex>
#include <sstream>

namespace {
thread_local std::string t_channel = "web";

std::string strip_ansi(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ) {
        if (s[i] == '\x1b' && i + 1 < s.size() && s[i + 1] == '[') {
            i += 2;
            while (i < s.size() && !((s[i] >= 0x40 && s[i] <= 0x7E))) i++;
            if (i < s.size()) i++;
        } else {
            out.push_back(s[i++]);
        }
    }
    return out;
}
}  // namespace

void set_log_channel(const std::string& ch) { t_channel = ch; }
std::string get_log_channel() { return t_channel; }

bool LogBroadcaster::Subscriber::wait_pop(Line& out, int timeout_ms) {
    std::unique_lock<std::mutex> lk(mu);
    if (!cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                     [this] { return !q.empty() || dead.load(); })) {
        return false;
    }
    if (dead.load() && q.empty()) return false;
    out = std::move(q.front());
    q.pop_front();
    return true;
}

void LogBroadcaster::push_line(const std::string& raw) {
    push_line(get_log_channel(), raw);
}

void LogBroadcaster::push_line(const std::string& channel, const std::string& raw) {
    std::string clean = strip_ansi(raw);
    bool is_progress = !clean.empty() && clean.back() == '\r';
    while (!clean.empty() && (clean.back() == '\n' || clean.back() == '\r')) {
        clean.pop_back();
    }
    if (clean.empty()) return;

    // Prepend timestamp
    auto now = std::chrono::system_clock::now();
    auto now_t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now.time_since_epoch()).count() % 1000;
    std::tm tm_buf{};
    localtime_r(&now_t, &tm_buf);
    std::ostringstream ts;
    ts << std::put_time(&tm_buf, "%H:%M:%S")
       << '.' << std::setw(3) << std::setfill('0') << ms;

    std::string text = is_progress
        ? "\r" + ts.str() + " " + clean
        :        ts.str() + " " + clean;

    Line line{channel.empty() ? "web" : channel, std::move(text)};

    std::list<std::shared_ptr<Subscriber>> subs_copy;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto& bucket = history_[line.channel];
        bucket.push_back(line);
        while (bucket.size() > HISTORY_CAP) bucket.pop_front();
        subs_copy = subs_;
    }
    for (auto& s : subs_copy) {
        if (s->dead.load()) continue;
        if (!s->channel.empty() && s->channel != line.channel) continue;
        {
            std::lock_guard<std::mutex> lk(s->mu);
            s->q.push_back(line);
            while (s->q.size() > 4000) s->q.pop_front();
        }
        s->cv.notify_one();
    }
}

std::shared_ptr<LogBroadcaster::Subscriber>
LogBroadcaster::subscribe(const std::string& channel_filter) {
    auto s = std::make_shared<Subscriber>();
    s->channel = channel_filter;
    std::lock_guard<std::mutex> lk(mu_);
    subs_.push_back(s);
    return s;
}

void LogBroadcaster::unsubscribe(const std::shared_ptr<Subscriber>& s) {
    if (!s) return;
    s->dead.store(true);
    s->cv.notify_all();
    std::lock_guard<std::mutex> lk(mu_);
    subs_.remove(s);
}

std::vector<LogBroadcaster::Line>
LogBroadcaster::snapshot(const std::string& channel, size_t max_lines) const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<Line> out;
    if (!channel.empty()) {
        auto it = history_.find(channel);
        if (it == history_.end()) return out;
        const auto& bucket = it->second;
        size_t start = bucket.size() > max_lines ? bucket.size() - max_lines : 0;
        out.reserve(bucket.size() - start);
        for (size_t i = start; i < bucket.size(); i++) out.push_back(bucket[i]);
        return out;
    }
    // All channels — merge in (approximately) insertion order. Lines within a
    // channel are ordered, but cross-channel ordering is timestamp-based via
    // the leading HH:MM:SS.mmm prefix we already prepended.
    for (auto& [_, bucket] : history_) {
        for (auto& line : bucket) out.push_back(line);
    }
    std::sort(out.begin(), out.end(),
              [](const Line& a, const Line& b) { return a.text < b.text; });
    if (out.size() > max_lines) out.erase(out.begin(), out.end() - max_lines);
    return out;
}

size_t LogBroadcaster::history_count() const {
    std::lock_guard<std::mutex> lk(mu_);
    size_t total = 0;
    for (auto& [_, bucket] : history_) total += bucket.size();
    return total;
}

std::map<std::string, size_t> LogBroadcaster::channel_counts() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::map<std::string, size_t> out;
    for (auto& [ch, bucket] : history_) out[ch] = bucket.size();
    return out;
}

// -- TeeStreambuf --------------------------------------------------------

TeeStreambuf::TeeStreambuf(std::streambuf* original, LogBroadcaster* bcast)
    : original_(original), bcast_(bcast) {}

TeeStreambuf::int_type TeeStreambuf::overflow(int_type c) {
    if (c == traits_type::eof()) return c;
    std::lock_guard<std::mutex> lk(mu_);
    char ch = static_cast<char>(c);
    if (original_) original_->sputc(ch);
    line_buf_.push_back(ch);
    if (ch == '\n' || ch == '\r') {
        flush_line_locked();
    } else if (line_buf_.size() > 8192) {
        flush_line_locked();
    }
    return c;
}

int TeeStreambuf::sync() {
    std::lock_guard<std::mutex> lk(mu_);
    if (!line_buf_.empty()) flush_line_locked();
    return original_ ? original_->pubsync() : 0;
}

void TeeStreambuf::flush_line_locked() {
    if (line_buf_.empty()) return;
    if (bcast_) bcast_->push_line(get_log_channel(), line_buf_);
    line_buf_.clear();
}

void install_log_tees(LogBroadcaster* bcast) {
    static TeeStreambuf* tee_out = new TeeStreambuf(std::cout.rdbuf(), bcast);
    static TeeStreambuf* tee_err = new TeeStreambuf(std::cerr.rdbuf(), bcast);
    std::cout.rdbuf(tee_out);
    std::cerr.rdbuf(tee_err);
}
