#pragma once
#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Polls /proc/net/dev every second and exposes a 60-sample ring of upload
// (TX) and download (RX) byte counts so the frontend can render a real-time
// sparkline of system network throughput.
class NetMonitor {
public:
    struct Sample {
        std::time_t at = 0;
        double      rx_bps = 0;   // bytes per second
        double      tx_bps = 0;
    };

    NetMonitor();
    ~NetMonitor();

    void start();
    void stop();

    // Returns a copy of the last N samples (oldest first).
    std::vector<Sample> samples(size_t max = 60) const;

    // Current instantaneous rate (last sample).
    Sample latest() const;

private:
    void run_loop();
    bool read_counters(uint64_t& rx, uint64_t& tx) const;

    std::atomic<bool>     running_{false};
    std::thread           worker_;
    mutable std::mutex    mu_;
    std::deque<Sample>    samples_;
    static constexpr size_t CAP = 60;
};

// Singleton (started by web_server).
NetMonitor& net_monitor();
