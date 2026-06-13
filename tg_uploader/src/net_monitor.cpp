#include "net_monitor.h"
#include <chrono>
#include <fstream>
#include <sstream>
#include <string>

NetMonitor::NetMonitor() = default;

NetMonitor::~NetMonitor() { stop(); }

void NetMonitor::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) return;
    worker_ = std::thread(&NetMonitor::run_loop, this);
}

void NetMonitor::stop() {
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) return;
    if (worker_.joinable()) worker_.join();
}

bool NetMonitor::read_counters(uint64_t& rx_out, uint64_t& tx_out) const {
    std::ifstream f("/proc/net/dev");
    if (!f.is_open()) return false;
    std::string line;
    uint64_t total_rx = 0, total_tx = 0;
    int line_no = 0;
    while (std::getline(f, line)) {
        line_no++;
        if (line_no <= 2) continue;          // header rows
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string iface = line.substr(0, colon);
        // Skip leading whitespace
        size_t s = 0;
        while (s < iface.size() && std::isspace(static_cast<unsigned char>(iface[s]))) s++;
        iface = iface.substr(s);
        // Skip loopback and docker/veth bridges — we want real outbound.
        if (iface == "lo") continue;
        if (iface.rfind("docker", 0) == 0) continue;
        if (iface.rfind("veth", 0) == 0) continue;
        if (iface.rfind("br-", 0) == 0) continue;

        std::istringstream iss(line.substr(colon + 1));
        uint64_t rx_b, rx_pkts, rx_errs, rx_drop, rx_fifo, rx_frame, rx_comp, rx_mcast;
        uint64_t tx_b, tx_pkts, tx_errs, tx_drop, tx_fifo, tx_colls, tx_carrier, tx_comp;
        if (iss >> rx_b >> rx_pkts >> rx_errs >> rx_drop >> rx_fifo >> rx_frame >> rx_comp >> rx_mcast
                >> tx_b >> tx_pkts >> tx_errs >> tx_drop >> tx_fifo >> tx_colls >> tx_carrier >> tx_comp) {
            total_rx += rx_b;
            total_tx += tx_b;
        }
    }
    rx_out = total_rx;
    tx_out = total_tx;
    return true;
}

void NetMonitor::run_loop() {
    uint64_t prev_rx = 0, prev_tx = 0;
    auto prev_t = std::chrono::steady_clock::now();
    bool primed = false;

    while (running_.load()) {
        uint64_t rx = 0, tx = 0;
        bool ok = read_counters(rx, tx);
        auto now_t = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now_t - prev_t).count();

        if (ok && primed && dt > 0) {
            Sample s;
            s.at = std::time(nullptr);
            // Counters can wrap (rare on 64-bit but possible on virtual ifaces) — clamp at 0
            s.rx_bps = rx >= prev_rx ? (rx - prev_rx) / dt : 0;
            s.tx_bps = tx >= prev_tx ? (tx - prev_tx) / dt : 0;
            std::lock_guard<std::mutex> lk(mu_);
            samples_.push_back(s);
            while (samples_.size() > CAP) samples_.pop_front();
        }

        prev_rx = rx;
        prev_tx = tx;
        prev_t = now_t;
        primed = ok;

        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

std::vector<NetMonitor::Sample> NetMonitor::samples(size_t max) const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<Sample> out;
    size_t start = samples_.size() > max ? samples_.size() - max : 0;
    out.reserve(samples_.size() - start);
    for (size_t i = start; i < samples_.size(); i++) out.push_back(samples_[i]);
    return out;
}

NetMonitor::Sample NetMonitor::latest() const {
    std::lock_guard<std::mutex> lk(mu_);
    if (samples_.empty()) return Sample{};
    return samples_.back();
}

NetMonitor& net_monitor() {
    static NetMonitor instance;
    return instance;
}
