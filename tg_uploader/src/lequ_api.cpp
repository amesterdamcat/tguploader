#include "lequ_api.h"
#include "lequ_sign.h"
#include "jwt_verify.h"
#include "config.h"
#include "utils.h"
#include "log_tee.h"
#include "../include/httplib.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <openssl/rand.h>

#include <netdb.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <csignal>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using json = nlohmann::json;
using httplib::Request;
using httplib::Response;
namespace fs = std::filesystem;

namespace {

// ── config (env-overridable) ─────────────────────────────────────────────────
// The Zeus API is served by an origin IP that bypasses the Aliyun CDN. Public
// DNS for gw.lequ.com returns the CDN node (an HTML redirect page, NOT the API),
// so we connect to a direct origin IP and send Host: gw.lequ.com. That origin IP
// does not present a matching cert, hence TLS verification is OFF by default
// (matches the reference lequ_backend.py / Frida-captured behaviour). The IP can
// rotate, so it is discovered + cached at runtime (see resolve_backend).
std::string env_or(const char* k, const std::string& def) {
    const char* v = std::getenv(k);
    return (v && *v) ? std::string(v) : def;
}
std::string gw_host()     { return env_or("LEQU_GW_HOST", "gw.lequ.com"); }
std::string gw_ip()       { return env_or("LEQU_GW_IP", ""); }            // pin: skip discovery
std::string fallback_ip() { return env_or("LEQU_GW_FALLBACK_IP", "81.70.32.189"); }
std::string media_dir()   { return env_or("LEQU_MEDIA_DIR", "/mnt/storage/lequ/media"); }
bool        tls_verify()  { return env_or("LEQU_TLS_VERIFY", "false") == "true"; }
int         poll_secs()   { try { return std::max(10, std::stoi(env_or("LEQU_POLL_SECONDS", "30"))); } catch (...) { return 30; } }
int record_read_timeout_secs() {
    try { return std::max(15, std::stoi(env_or("LEQU_RECORD_READ_TIMEOUT_SECONDS", "30"))); }
    catch (...) { return 30; }
}
int record_reconnect_grace_secs() {
    try { return std::max(15, std::stoi(env_or("LEQU_RECORD_RECONNECT_GRACE_SECONDS", "45"))); }
    catch (...) { return 45; }
}
int record_heartbeat_secs() {
    try { return std::clamp(std::stoi(env_or("LEQU_RECORD_HEARTBEAT_SECONDS", "2")), 2, 30); }
    catch (...) { return 2; }
}
bool record_heartbeat_enabled() {
    std::string v = env_or("LEQU_RECORD_HEARTBEAT", "auto");
    std::transform(v.begin(), v.end(), v.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return v != "0" && v != "false" && v != "off" && v != "no";
}
int record_retry_cooldown_secs() {
    try { return std::max(60, std::stoi(env_or("LEQU_RECORD_RETRY_COOLDOWN_SECONDS", "180"))); }
    catch (...) { return 180; }
}
std::string user_agent()  {
    return env_or("LEQU_USER_AGENT",
        "Dalvik/2.1.0 (Linux; U; Android 9; BRA-AL00 Build/PQ3A.190705.05150936)"
        "; lequ android v6.25.0.202604221027-lequ");
}
std::string cfg_dir()     { return get_base_dir() + "/.account_configs"; }
std::string auth_file()   { return cfg_dir() + "/lequ_auth.json"; }
std::string tracked_file(){ return cfg_dir() + "/lequ_tracked.json"; }

// Spawn a detached background thread whose exceptions can never abort the
// process (an uncaught throw in a std::thread calls std::terminate). Any escape
// is logged and swallowed so a malformed upstream reply only kills that thread.
struct ScopedLogChannel {
    std::string previous;
    explicit ScopedLogChannel(const std::string& channel) : previous(get_log_channel()) {
        set_log_channel(channel);
    }
    ~ScopedLogChannel() { set_log_channel(previous); }
};

template <class F>
void spawn_guarded(const char* tag, F&& f) {
    std::thread([tag, fn = std::forward<F>(f)]() mutable {
        ScopedLogChannel log_channel("lequ");
        try { fn(); }
        catch (const std::exception& e) { std::cerr << "[LeQu] thread " << tag << " error: " << e.what() << "\n"; }
        catch (...) { std::cerr << "[LeQu] thread " << tag << " error (unknown)\n"; }
    }).detach();
}

// ── HTTP client ─────────────────────────────────────────────────────────────
size_t wcb(char* p, size_t s, size_t n, void* u) {
    static_cast<std::string*>(u)->append(p, s * n);
    return s * n;
}
struct Resp { long status = 0; std::string body; bool transport_ok = false; };
std::atomic<bool> g_lequ_auth_expired{false};

struct DebugEntry {
    unsigned long long seq = 0;
    json data;
};
std::mutex g_debug_mu;
std::deque<DebugEntry> g_debug_log;
std::atomic<unsigned long long> g_debug_seq{0};
constexpr size_t DEBUG_LOG_LIMIT = 400;

void append_debug_log(const std::string& path, const LequSigned& sig,
                      const std::string& auth, const std::string& request_plain,
                      const std::string& backend_ip, const Resp& r,
                      long long elapsed_ms) {
    const auto seq = g_debug_seq.fetch_add(1) + 1;
    json item{{"seq", seq}, {"at", (long long)std::time(nullptr)}, {"path", path},
              {"backend_ip", backend_ip}, {"el_auth", auth},
              {"transport_ok", r.transport_ok}, {"http_status", r.status},
              {"elapsed_ms", elapsed_ms}, {"el_ver", sig.el_ver},
              {"el_ect", sig.el_ect}, {"el_ns", sig.el_ns},
              {"el_sign", sig.el_sign}, {"request_plain", request_plain},
              {"request_encrypted", sig.body}, {"request_bytes", sig.body.size()},
              {"response_bytes", r.body.size()}, {"response_body", r.body}};
    std::lock_guard<std::mutex> lk(g_debug_mu);
    g_debug_log.push_back({seq, std::move(item)});
    while (g_debug_log.size() > DEBUG_LOG_LIMIT) g_debug_log.pop_front();
}

std::vector<json> debug_logs_since(unsigned long long after) {
    std::vector<json> out;
    std::lock_guard<std::mutex> lk(g_debug_mu);
    for (const auto& e : g_debug_log)
        if (e.seq > after) out.push_back(e.data);
    return out;
}

// ── backend IP discovery (CDN bypass) ────────────────────────────────────────
// Port of lequ_backend.py: cache a working origin IP, probe it with the
// unsigned /app/init/start endpoint, fall back to DNS candidates + a known IP.
std::mutex  g_be_mu;
std::string g_be_ip;
std::time_t g_be_ts = 0;
constexpr int BE_TTL = 6 * 3600;
std::string be_cache_file() { return cfg_dir() + "/.lequ_backend_ip.json"; }

// Unsigned reachability probe: a real API origin returns JSON; the CDN/redirect
// node returns HTML. Returns true only on parseable JSON.
bool probe_ip(const std::string& ip) {
    CURL* c = curl_easy_init();
    if (!c) return false;
    std::string body;
    struct curl_slist* hdr = nullptr;
    hdr = curl_slist_append(hdr, ("Host: " + gw_host()).c_str());
    hdr = curl_slist_append(hdr, "Content-Type: text/plain;charset=utf-8");
    hdr = curl_slist_append(hdr, "Charset: utf-8");
    hdr = curl_slist_append(hdr, "app_device_type: android");
    std::string url = "https://" + ip + "/app/init/start";
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdr);
    curl_easy_setopt(c, CURLOPT_USERAGENT, user_agent().c_str());
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "gzip");
    curl_easy_setopt(c, CURLOPT_POST, 1L);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, "");
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, 0L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, wcb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 6L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, tls_verify() ? 1L : 0L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, tls_verify() ? 2L : 0L);
    CURLcode rc = curl_easy_perform(c);
    curl_slist_free_all(hdr);
    curl_easy_cleanup(c);
    if (rc != CURLE_OK) return false;
    try { auto j = json::parse(body); (void)j; return true; } catch (...) { return false; }
}

std::vector<std::string> dns_resolve(const std::string& host) {
    std::vector<std::string> ips;
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), "443", &hints, &res) == 0) {
        for (auto* p = res; p; p = p->ai_next) {
            char buf[INET_ADDRSTRLEN];
            auto* sa = reinterpret_cast<sockaddr_in*>(p->ai_addr);
            if (inet_ntop(AF_INET, &sa->sin_addr, buf, sizeof buf)) {
                std::string ip = buf;
                if (std::find(ips.begin(), ips.end(), ip) == ips.end()) ips.push_back(ip);
            }
        }
        freeaddrinfo(res);
    }
    return ips;
}

void load_be_cache() {
    std::ifstream f(be_cache_file());
    if (!f) return;
    try { json j; f >> j; g_be_ip = j.value("ip", ""); g_be_ts = j.value("ts", (std::time_t)0); } catch (...) {}
}
void save_be_cache_locked() {
    std::ofstream f(be_cache_file());
    if (f) f << json{{"ip", g_be_ip}, {"ts", (long long)g_be_ts}}.dump();
}

// Return a usable backend IP. Trusts the cached IP within its TTL; otherwise
// probes DNS candidates + the fallback + last-known, caching the first that works.
std::string resolve_backend(bool force = false) {
    if (!gw_ip().empty()) return gw_ip();   // operator pin
    std::lock_guard<std::mutex> lk(g_be_mu);
    std::time_t now = std::time(nullptr);
    if (!force && !g_be_ip.empty() && now - g_be_ts < BE_TTL) return g_be_ip;

    std::vector<std::string> cands = dns_resolve(gw_host());
    auto add = [&](const std::string& ip) {
        if (!ip.empty() && std::find(cands.begin(), cands.end(), ip) == cands.end()) cands.push_back(ip);
    };
    add(fallback_ip());
    add(g_be_ip);
    for (const auto& ip : cands) {
        if (probe_ip(ip)) {
            g_be_ip = ip; g_be_ts = now; save_be_cache_locked();
            std::cout << "[LeQu] backend IP = " << ip << "\n";
            return ip;
        }
    }
    std::cerr << "[LeQu] no working backend IP among " << cands.size() << " candidates\n";
    return g_be_ip;   // last-known (possibly stale) or empty
}
void invalidate_backend() { std::lock_guard<std::mutex> lk(g_be_mu); g_be_ts = 0; }

// Looks like an HTML page (CDN/redirect node) rather than an API JSON response.
bool looks_like_html(const std::string& b) {
    size_t i = b.find_first_not_of(" \t\r\n");
    return i != std::string::npos && (b[i] == '<');
}

// One signed POST to a specific backend IP (Host: gw.lequ.com, TLS verify off).
Resp lequ_post_once(const std::string& ip, const LequSigned& sig,
                    const std::string& path, const std::string& auth,
                    const std::string& cookie_file,
                    const std::string& request_plain) {
    Resp r;
    CURL* c = curl_easy_init();
    if (!c) return r;
    struct curl_slist* hdr = nullptr;
    hdr = curl_slist_append(hdr, ("Host: " + gw_host()).c_str());
    hdr = curl_slist_append(hdr, "Content-Type: text/plain;charset=utf-8");
    hdr = curl_slist_append(hdr, "Charset: utf-8");
    hdr = curl_slist_append(hdr, "app_device_type: android");
    hdr = curl_slist_append(hdr, "Connection: Keep-Alive");
    hdr = curl_slist_append(hdr, ("EL-SIGN: " + sig.el_sign).c_str());
    hdr = curl_slist_append(hdr, ("EL-NS: " + sig.el_ns).c_str());
    hdr = curl_slist_append(hdr, ("EL-VER: " + sig.el_ver).c_str());
    hdr = curl_slist_append(hdr, ("EL-ECT: " + sig.el_ect).c_str());
    if (!auth.empty()) hdr = curl_slist_append(hdr, ("EL-AUTH: " + auth).c_str());

    const std::string url = "https://" + ip + path;
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdr);
    curl_easy_setopt(c, CURLOPT_USERAGENT, user_agent().c_str());
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "gzip");   // auto-decompress
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, wcb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &r.body);
    curl_easy_setopt(c, CURLOPT_POST, 1L);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, sig.body.c_str());
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, static_cast<long>(sig.body.size()));
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, tls_verify() ? 1L : 0L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, tls_verify() ? 2L : 0L);
    if (!cookie_file.empty()) {
        curl_easy_setopt(c, CURLOPT_COOKIEFILE, cookie_file.c_str());
        curl_easy_setopt(c, CURLOPT_COOKIEJAR, cookie_file.c_str());
    }
    auto started = std::chrono::steady_clock::now();
    CURLcode rc = curl_easy_perform(c);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();
    r.transport_ok = (rc == CURLE_OK);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r.status);
    curl_slist_free_all(hdr);
    curl_easy_cleanup(c);
    append_debug_log(path, sig, auth, request_plain, ip, r, elapsed);
    return r;
}

// POST a signed request to the gateway. encrypt is always attempted; the signer
// downgrades to M1.0/raw automatically when body is empty. cookie_file, when
// set, is used as both jar and source so the 3-step login can share state. On a
// transport failure or HTML (rotated/dead backend IP) it re-discovers once.
// A fresh nonce/signature is generated per attempt.
Resp lequ_post(const std::string& path, const std::string& auth,
               const std::string& body, const std::string& cookie_file = "") {
    for (int attempt = 0; attempt < 2; ++attempt) {
        std::string ip = resolve_backend(/*force=*/attempt > 0);
        if (ip.empty()) return Resp{};
        LequSigned sig = lequ_sign(path, auth, body, /*ect=*/1, /*encrypt=*/true);
        Resp r = lequ_post_once(ip, sig, path, auth, cookie_file, body);
        if (r.transport_ok && !looks_like_html(r.body)) return r;
        if (attempt == 0) { invalidate_backend(); continue; }   // IP rotated → retry once
        return r;
    }
    return Resp{};
}

// Parse upstream body to JSON; returns null json on failure. Logs gateway-level
// error codes (code < 0) without leaking the request.
bool parse_ok(const Resp& r, json& out, const std::string& ctx) {
    if (!r.transport_ok) { std::cerr << "[LeQu] " << ctx << ": upstream unreachable\n"; return false; }
    try { out = json::parse(r.body); }
    catch (...) { std::cerr << "[LeQu] " << ctx << ": non-JSON HTTP " << r.status << "\n"; return false; }
    if (!out.is_object()) {   // never let callers .value() on an array/scalar (→ terminate)
        std::cerr << "[LeQu] " << ctx << ": non-object response (" << out.type_name() << ")\n";
        return false;
    }
    if (out.contains("code") && out["code"].is_number_integer()
        && out["code"].get<int>() < 0) {
        int code = out["code"].get<int>();
        if (code == -10) g_lequ_auth_expired.store(true);
        std::cerr << "[LeQu] " << ctx << ": gateway code=" << code
                  << (code == -10 ? " (auth expired)" : "") << "\n";
        return false;
    }
    return true;
}

// ── anchor extraction (lists may be {"list":[...]}, a bare array, or nested) ──
struct Anchor {
    std::string name, nickname, vid, logo, thumb;
    bool living = false;
    bool followed = false;
    bool follow_known = false;
    int permission = 0;
    long long watch_count = 0;
};
bool fetch_list(const std::string& kind, std::vector<Anchor>& out);

struct RoomStats {
    long long watching = 0;
    long long total = 0;
    bool followed = false;
    std::string hcs_ip;        // comment/heat server (from /app/video/join)
    int hcs_port = 0;
    std::time_t fetched_at = 0;
};
std::mutex g_room_stats_mu;
std::map<std::string, RoomStats> g_room_stats;
constexpr int ROOM_STATS_TTL = 10;
std::mutex g_account_watch_mu;
std::mutex g_record_heartbeat_mu;
std::atomic<int> g_farm_heartbeat_users{0};
bool fetch_room_stats(const std::string& vid, RoomStats& out);
bool fetch_status(const std::string& ip, int port, const std::string& vid,
                  long long cid, long long gid, long long aid, long long lt, json& out);

std::string jstr(const json& o, const char* k) {
    if (!o.contains(k) || o[k].is_null()) return "";
    if (o[k].is_string()) return o[k].get<std::string>();
    if (o[k].is_number_integer()) return std::to_string(o[k].get<long long>());
    return "";
}

void collect_anchors(const json& v, std::vector<Anchor>& out) {
    if (v.is_array()) {
        for (const auto& e : v) collect_anchors(e, out);
    } else if (v.is_object()) {
        std::string vid = jstr(v, "vid");
        std::string name = jstr(v, "name");
        std::string nick = v.contains("nickName") ? jstr(v, "nickName") : jstr(v, "nickname");
        if (!vid.empty() && (!name.empty() || !nick.empty())) {
            Anchor a;
            a.name = name; a.nickname = nick; a.vid = vid;
            a.logo = jstr(v, "logoUrl"); a.thumb = jstr(v, "thumb");
            if (v.contains("isLiving")) a.living = v["isLiving"].is_boolean() ? v["isLiving"].get<bool>()
                                                                             : (jstr(v, "isLiving") == "true");
            if (v.contains("followed") && v["followed"].is_boolean()) {
                a.followed = v["followed"].get<bool>();
                a.follow_known = true;
            } else if (v.contains("follow") && v["follow"].is_boolean()) {
                a.followed = v["follow"].get<bool>();
                a.follow_known = true;
            }
            if (v.contains("permission") && v["permission"].is_number_integer())
                a.permission = v["permission"].get<int>();
            if (v.contains("watchCount") && v["watchCount"].is_number_integer())
                a.watch_count = v["watchCount"].get<long long>();
            out.push_back(std::move(a));
        }
        for (auto it = v.begin(); it != v.end(); ++it) collect_anchors(it.value(), out);
    }
}

json anchor_to_json(const Anchor& a) {
    return json{{"name", a.name}, {"nickname", a.nickname}, {"vid", a.vid},
                {"logo", a.logo}, {"thumb", a.thumb},
                {"living", a.living}, {"permission", a.permission},
                {"followed", a.followed}, {"follow_known", a.follow_known},
                {"watch_count", a.watch_count}};
}

// ── auth state (sessionId == EL-AUTH) ───────────────────────────────────────
std::mutex  g_auth_mu;
std::string g_session, g_user, g_phone;

void load_auth() {
    std::lock_guard<std::mutex> lk(g_auth_mu);
    std::ifstream f(auth_file());
    if (!f) return;
    try { json j; f >> j; g_session = j.value("sessionId", ""); g_user = j.value("name", ""); g_phone = j.value("phone", ""); }
    catch (...) {}
}
void save_auth() {
    std::lock_guard<std::mutex> lk(g_auth_mu);
    json j{{"sessionId", g_session}, {"name", g_user}, {"phone", g_phone}};
    std::ofstream f(auth_file());
    if (f) f << j.dump(2);
}
std::string current_auth() { std::lock_guard<std::mutex> lk(g_auth_mu); return g_session; }

// ── tracked anchors ─────────────────────────────────────────────────────────
std::mutex g_track_mu;
json       g_tracked = json::array();   // [{name, nickname}]

void load_tracked() {
    std::lock_guard<std::mutex> lk(g_track_mu);
    std::ifstream f(tracked_file());
    if (!f) return;
    try { json j; f >> j; if (j.is_array()) g_tracked = j; } catch (...) {}
}
void save_tracked_locked() {
    std::ofstream f(tracked_file());
    if (f) f << g_tracked.dump(2);
}

// ── LeQu runtime settings ────────────────────────────────────────────────────
struct LequSettings {
    // farm  : farm owns the single-account watch heartbeat whenever it is active.
    // record: recording heartbeat wins; farm waits while any recording is active.
    // ab    : deterministic A/B test; half of recordings get heartbeat, half do not.
    // off   : recording heartbeat is disabled; farm can still heartbeat normally.
    std::string watch_priority = "farm";
};
std::mutex g_settings_mu;
LequSettings g_settings;

std::string settings_file() { return cfg_dir() + "/lequ_settings.json"; }
std::string normalize_watch_priority(const std::string& v) {
    if (v == "record" || v == "recording") return "record";
    if (v == "ab" || v == "record_ab" || v == "compare") return "ab";
    if (v == "off" || v == "none" || v == "disabled") return "off";
    return "farm";
}
void load_lequ_settings() {
    std::ifstream f(settings_file());
    if (!f) return;
    try {
        json j; f >> j;
        std::lock_guard<std::mutex> lk(g_settings_mu);
        g_settings.watch_priority = normalize_watch_priority(j.value("watch_priority", "farm"));
    } catch (...) {}
}
void save_settings_locked() {
    std::ofstream f(settings_file());
    if (f) f << json{{"watch_priority", g_settings.watch_priority}}.dump(2);
}
std::string watch_priority() {
    std::lock_guard<std::mutex> lk(g_settings_mu);
    return g_settings.watch_priority;
}
bool ab_record_heartbeat_on(const std::string& name) {
    unsigned long long h = 1469598103934665603ULL;
    for (unsigned char c : name.empty() ? std::string("unknown") : name) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return (h & 1ULL) == 0ULL;
}
json settings_json() {
    std::lock_guard<std::mutex> lk(g_settings_mu);
    return json{{"watch_priority", g_settings.watch_priority},
                {"record_heartbeat_enabled", g_settings.watch_priority != "off"},
                {"record_heartbeat_ab", g_settings.watch_priority == "ab"}};
}

// ── recordings ───────────────────────────────────────────────────────────────
struct RecJob {
    std::string name, nickname, vid, file, heartbeat_group = "unknown";
    std::atomic<bool> cancel{false};
    std::atomic<int>  state{0};   // 0 resolving 1 recording 2 remux 3 done 4 failed 5 stopped
    std::atomic<long long> last_heartbeat_at{0};
    std::atomic<long long> heartbeat_ok{0};
    std::atomic<long long> heartbeat_failed{0};
    std::atomic<long long> heartbeat_skipped{0};
    std::time_t started_at = 0;
};
std::mutex g_rec_mu;
std::map<std::string, std::shared_ptr<RecJob>> g_recs;   // key = vid
std::map<std::string, std::time_t> g_rec_retry_after;     // key = stable anchor name
std::mutex g_rec_history_mu;
json g_rec_history = json::array();
std::atomic<bool> g_recovery_scanner_running{false};
struct ContactTask { std::string id, path; };
std::mutex g_contact_mu;
std::condition_variable g_contact_cv;
std::deque<ContactTask> g_contact_queue;
std::atomic<bool> g_contact_workers_started{false};
std::atomic<bool> g_contact_auto_running{false};
std::atomic<int> g_contact_active{0};

struct PlayJob {
    std::string id, vid, name, nickname, dir;
    std::atomic<bool> cancel{false};
    std::atomic<bool> ready{false};
    std::atomic<bool> failed{false};
    std::atomic<std::time_t> touched_at{0};
    std::time_t started_at = 0;
};
std::mutex g_play_mu;
std::map<std::string, std::shared_ptr<PlayJob>> g_players;
std::string rand_hex(int n);
const char* state_str(int s);

std::string player_root() { return "/tmp/tg_lequ_hls"; }
std::string recordings_file() { return cfg_dir() + "/lequ_recordings.json"; }

long long file_mtime(const std::string& path) {
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0 ? static_cast<long long>(st.st_mtime) : 0;
}
long long file_bytes(const std::string& path) {
    std::error_code ec;
    auto n = fs::file_size(path, ec);
    return ec ? 0 : static_cast<long long>(n);
}
void save_recordings_locked() {
    std::ofstream f(recordings_file());
    if (f) f << g_rec_history.dump(2);
}

std::string recovery_stem(const std::string& path) {
    static const std::string marker = ".part";
    size_t pos = path.rfind(marker);
    if (pos != std::string::npos && path.size() > pos + marker.size() + 4
        && path.substr(path.size() - 4) == ".flv") {
        bool digits = true;
        for (size_t i = pos + marker.size(); i + 4 < path.size(); ++i)
            digits = digits && std::isdigit(static_cast<unsigned char>(path[i]));
        if (digits) return path.substr(0, pos);
    }
    if (path.size() > 4 && path.substr(path.size() - 4) == ".flv")
        return path.substr(0, path.size() - 4);
    return path;
}

std::vector<std::string> recording_sources(const json& r) {
    std::vector<std::string> sources;
    if (r.contains("source_paths") && r["source_paths"].is_array()) {
        for (const auto& p : r["source_paths"])
            if (p.is_string() && !p.get<std::string>().empty()) sources.push_back(p.get<std::string>());
    }
    if (sources.empty()) {
        std::string path = r.value("path", "");
        if (!path.empty()) sources.push_back(path);
    }
    return sources;
}

bool source_is_stale(const std::string& path, std::time_t now) {
    long long mtime = file_mtime(path);
    return mtime > 0 && now - mtime >= 180;
}

int scan_recoverable_recordings() {
    std::error_code ec;
    if (!fs::exists(media_dir(), ec)) return 0;

    const std::time_t now = std::time(nullptr);
    std::map<std::string, std::vector<std::string>> groups;
    for (const auto& entry : fs::recursive_directory_iterator(
             media_dir(), fs::directory_options::skip_permission_denied, ec)) {
        if (ec || !entry.is_regular_file()) continue;
        std::string path = entry.path().string();
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c){ return std::tolower(c); });
        if (ext != ".flv" || file_bytes(path) <= 0 || !source_is_stale(path, now)) continue;
        groups[recovery_stem(path)].push_back(path);
    }

    int added = 0;
    std::lock_guard<std::mutex> lk(g_rec_history_mu);
    std::set<std::string> known_sources;
    std::set<std::string> known_stems;
    for (const auto& r : g_rec_history) {
        if (!r.is_object()) continue;
        for (const auto& path : recording_sources(r)) {
            known_sources.insert(path);
            known_stems.insert(recovery_stem(path));
        }
    }
    for (auto& [stem, sources] : groups) {
        std::sort(sources.begin(), sources.end());
        if (known_stems.count(stem)) continue;
        bool known = std::any_of(sources.begin(), sources.end(),
            [&](const std::string& path){ return known_sources.count(path) != 0; });
        if (known) continue;

        fs::path first(sources.front());
        std::string name = first.parent_path().filename().string();
        std::string filename_stem = fs::path(stem).filename().string();
        std::string marker = "_" + name + "_";
        size_t pos = filename_stem.rfind(marker);
        std::string nickname = pos == std::string::npos ? name : filename_stem.substr(0, pos);
        long long total = 0;
        long long started = file_mtime(sources.front());
        long long ended = started;
        for (const auto& path : sources) {
            total += file_bytes(path);
            started = std::min(started, file_mtime(path));
            ended = std::max(ended, file_mtime(path));
        }
        json recovery_entry = {
            {"id", rand_hex(16)}, {"name", name}, {"nickname", nickname}, {"vid", ""},
            {"state", "interrupted"}, {"started_at", started}, {"ended_at", ended},
            {"path", sources.front()}, {"source_paths", sources}, {"thumb_path", ""},
            {"size", total}, {"recoverable", true}, {"remuxing", false}
        };
        g_rec_history.insert(g_rec_history.begin(), std::move(recovery_entry));
        ++added;
    }
    if (added) {
        save_recordings_locked();
        std::cout << "[LeQu] recovery scan found " << added << " interrupted recording(s)\n";
    }
    return added;
}

void load_recordings() {
    {
        std::ifstream f(recordings_file());
        if (f) {
            try { f >> g_rec_history; } catch (...) { g_rec_history = json::array(); }
        }
        if (!g_rec_history.is_array()) g_rec_history = json::array();
        // drop malformed (non-object) entries — they crash .value() readers
        json clean = json::array();
        for (auto& r : g_rec_history) {
            if (!r.is_object()) continue;
            std::string path = r.value("path", "");
            if (path.size() > 4 && path.substr(path.size() - 4) == ".flv") {
                r["state"] = "interrupted";
                r["recoverable"] = true;
                if (!r.contains("source_paths")) r["source_paths"] = json::array({path});
                if (!r.contains("remuxing")) r["remuxing"] = false;
            }
            clean.push_back(std::move(r));
        }
        g_rec_history = std::move(clean);
    }
    std::set<std::string> known;
    for (const auto& r : g_rec_history)
        if (r.is_object()) for (const auto& p : recording_sources(r)) known.insert(p);
    std::error_code ec;
    if (!fs::exists(media_dir(), ec)) return;
    for (const auto& entry : fs::recursive_directory_iterator(
             media_dir(), fs::directory_options::skip_permission_denied, ec)) {
        if (ec || !entry.is_regular_file()) continue;
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c){ return std::tolower(c); });
        if (ext != ".mp4") continue;
        std::string path = entry.path().string();
        if (known.count(path)) continue;
        std::string name = entry.path().parent_path().filename().string();
        std::string stem = entry.path().stem().string();
        std::string marker = "_" + name + "_";
        size_t pos = stem.rfind(marker);
        std::string nickname = pos == std::string::npos ? name : stem.substr(0, pos);
        fs::path thumb_path = entry.path();
        thumb_path.replace_extension(".jpg");
        std::string thumb = thumb_path.string();
        g_rec_history.push_back({
            {"id", rand_hex(16)}, {"name", name}, {"nickname", nickname}, {"vid", ""},
            {"state", "done"}, {"started_at", file_mtime(path)}, {"ended_at", file_mtime(path)},
            {"path", path}, {"thumb_path", fs::exists(thumb, ec) ? thumb : ""},
            {"size", file_bytes(path)}, {"thumb_generating", false}
        });
    }
    save_recordings_locked();
    scan_recoverable_recordings();
}

double parse_ff_rate(const std::string& rate) {
    if (rate.empty() || rate == "0/0") return 0;
    size_t slash = rate.find('/');
    try {
        if (slash == std::string::npos) return std::stod(rate);
        double num = std::stod(rate.substr(0, slash));
        double den = std::stod(rate.substr(slash + 1));
        return den > 0 ? num / den : 0;
    } catch (...) {
        return 0;
    }
}

struct MediaProbe { double duration = 0; double fps = 0; int width = 0; int height = 0; };
MediaProbe probe_media(const std::string& path) {
    std::string cmd = "ffprobe -v error -select_streams v:0 "
                      "-show_entries stream=width,height,avg_frame_rate,r_frame_rate:format=duration -of json "
                      + shell_escape(path);
    FILE* p = ::popen(cmd.c_str(), "r");
    if (!p) return {};
    char buf[4096]{};
    std::string out;
    while (std::fgets(buf, sizeof buf, p)) out += buf;
    ::pclose(p);
    try {
        json j = json::parse(out);
        MediaProbe r;
        if (j.contains("format")) {
            std::string d = j["format"].value("duration", "0");
            try { r.duration = std::stod(d); } catch (...) {}
        }
        if (j.contains("streams") && j["streams"].is_array() && !j["streams"].empty()) {
            r.width = j["streams"][0].value("width", 0);
            r.height = j["streams"][0].value("height", 0);
            r.fps = parse_ff_rate(j["streams"][0].value("avg_frame_rate", ""));
            if (r.fps <= 0) r.fps = parse_ff_rate(j["streams"][0].value("r_frame_rate", ""));
        }
        return r;
    } catch (...) { return {}; }
}

bool make_lequ_contact_sheet(const std::string& video, const std::string& jpg) {
    MediaProbe probe = probe_media(video);
    const bool portrait = probe.height > probe.width && probe.width > 0;
    const int cols = portrait ? 8 : 12;
    const int rows = portrait ? 15 : 10;
    const int tile_width = portrait ? 400 : 480;
    const int frames = cols * rows;
    double interval = probe.duration > 0 ? probe.duration / (frames + 1) : 5.0;
    std::string filter =
        "fps=1/" + std::to_string(interval) +
        ",scale=-1:720"
        ",drawtext=font=sans-serif:fontcolor=white:fontsize=60"
        ":text='%{pts\\:hms}'"
        ":box=1:boxcolor=black@0.5:boxborderw=5:x=20:y=h-th-20"
        ",scale=" + std::to_string(tile_width) + ":-1"
        ",tile=" + std::to_string(cols) + "x" + std::to_string(rows)
        + ":padding=2:color=0x242424";
    std::string cmd = "ffmpeg -nostdin -hide_banner -loglevel error -y -threads 2 -i "
        + shell_escape(video) + " -vf " + shell_escape(filter)
        + " -frames:v 1 -q:v 1 " + shell_escape(jpg);
    std::atomic<bool> never{false};
    int rc = cancellable_system(cmd, never);
    std::error_code ec;
    return rc == 0 && fs::exists(jpg, ec) && fs::file_size(jpg, ec) > 0;
}

std::string contact_preview_path(const std::string& jpg) {
    fs::path p(jpg);
    return (p.parent_path() / (p.stem().string() + ".preview.jpg")).string();
}

bool make_contact_preview(const std::string& jpg, const std::string& preview) {
    std::string cmd = "ffmpeg -nostdin -hide_banner -loglevel error -y -threads 1 -i "
        + shell_escape(jpg) + " -vf " + shell_escape("scale=640:-2")
        + " -frames:v 1 -q:v 5 " + shell_escape(preview);
    std::atomic<bool> never{false};
    int rc = cancellable_system(cmd, never);
    std::error_code ec;
    return rc == 0 && fs::exists(preview, ec) && fs::file_size(preview, ec) > 0;
}

bool enqueue_contact_sheet(const std::string& id, const std::string& path, bool force) {
    if (id.empty() || path.empty()) return false;
    std::error_code ec;
    if (!fs::exists(path, ec) || fs::path(path).extension() != ".mp4") return false;

    {
        std::lock_guard<std::mutex> lk(g_rec_history_mu);
        bool found = false;
        for (auto& r : g_rec_history) {
            if (r.value("id", "") != id) continue;
            found = true;
            if (r.value("thumb_generating", false)) return false;
            std::string existing = r.value("thumb_path", "");
            if (!force && !existing.empty() && fs::exists(existing, ec)) return false;
            r["thumb_generating"] = true;
            save_recordings_locked();
            break;
        }
        if (!found) return false;
    }

    {
        std::lock_guard<std::mutex> lk(g_contact_mu);
        g_contact_queue.push_back({id, path});
    }
    g_contact_cv.notify_one();
    return true;
}

void contact_sheet_worker_loop() {
    while (true) {
        ContactTask task;
        {
            std::unique_lock<std::mutex> lk(g_contact_mu);
            g_contact_cv.wait(lk, [] { return !g_contact_queue.empty(); });
            task = std::move(g_contact_queue.front());
            g_contact_queue.pop_front();
            g_contact_active.fetch_add(1);
        }

        std::string thumb = fs::path(task.path).replace_extension(".jpg").string();
        std::cout << "[LeQu] contact sheet start "
                  << fs::path(task.path).filename().string() << std::endl;
        bool ok = make_lequ_contact_sheet(task.path, thumb);
        if (ok) make_contact_preview(thumb, contact_preview_path(thumb));
        MediaProbe media = probe_media(task.path);

        {
            std::lock_guard<std::mutex> lk(g_rec_history_mu);
            for (auto& r : g_rec_history) {
                if (r.value("id", "") != task.id) continue;
                r["thumb_generating"] = false;
                if (ok) r["thumb_path"] = thumb;
                if (media.duration > 0) r["duration"] = media.duration;
                if (media.width > 0) r["width"] = media.width;
                if (media.height > 0) r["height"] = media.height;
                if (media.fps > 0) r["fps"] = media.fps;
                break;
            }
            save_recordings_locked();
        }
        g_contact_active.fetch_sub(1);
        if (ok) {
            std::cout << "[LeQu] contact sheet saved "
                      << fs::path(thumb).filename().string() << std::endl;
        } else {
            std::cerr << "[LeQu] contact sheet failed "
                      << fs::path(task.path).filename().string() << std::endl;
        }
    }
}

void start_contact_sheet_workers() {
    if (g_contact_workers_started.exchange(true)) return;
    for (int i = 0; i < 3; ++i)
        spawn_guarded("contact-sheet", [] { contact_sheet_worker_loop(); });
}

int queue_missing_contact_sheets(int limit) {
    struct Candidate { std::string id, path; };
    std::vector<Candidate> candidates;
    {
        std::lock_guard<std::mutex> lk(g_rec_history_mu);
        std::error_code ec;
        for (const auto& r : g_rec_history) {
            if (!r.is_object()) continue;
            if (r.value("recoverable", false) || r.value("thumb_generating", false)) continue;
            std::string id = r.value("id", "");
            std::string path = r.value("path", "");
            if (id.empty() || path.empty() || fs::path(path).extension() != ".mp4") continue;
            if (!fs::exists(path, ec)) continue;
            std::string thumb = r.value("thumb_path", "");
            if (!thumb.empty() && fs::exists(thumb, ec)) continue;
            candidates.push_back({id, path});
            if (limit > 0 && static_cast<int>(candidates.size()) >= limit) break;
        }
    }
    int queued = 0;
    for (const auto& c : candidates)
        if (enqueue_contact_sheet(c.id, c.path, false)) ++queued;
    return queued;
}

void add_recording_history(const std::shared_ptr<RecJob>& job,
                           const std::string& path, const std::string& thumb) {
    MediaProbe media = probe_media(path);
    std::lock_guard<std::mutex> lk(g_rec_history_mu);
    for (auto it = g_rec_history.begin(); it != g_rec_history.end(); ++it) {
        if (it->value("path", "") == path) { g_rec_history.erase(it); break; }
    }
    // NOTE: build the object FIRST. insert(pos, {…}) treats the braces as an
    // initializer_list of elements, turning each {"k",v} into an array ["k",v].
    json entry = {
        {"id", rand_hex(16)}, {"name", job->name}, {"nickname", job->nickname},
        {"vid", job->vid}, {"state", state_str(job->state.load())},
        {"heartbeat_group", job->heartbeat_group},
        {"last_heartbeat_at", job->last_heartbeat_at.load()},
        {"heartbeat_ok", job->heartbeat_ok.load()},
        {"heartbeat_failed", job->heartbeat_failed.load()},
        {"heartbeat_skipped", job->heartbeat_skipped.load()},
        {"started_at", static_cast<long long>(job->started_at)},
        {"ended_at", static_cast<long long>(std::time(nullptr))},
        {"path", path}, {"thumb_path", thumb}, {"size", file_bytes(path)},
        {"duration", media.duration}, {"width", media.width}, {"height", media.height},
        {"fps", media.fps}, {"thumb_generating", false}
    };
    g_rec_history.insert(g_rec_history.begin(), std::move(entry));
    save_recordings_locked();
}

bool recording_name_active_locked(const std::string& name) {
    for (auto& kv : g_recs)
        if (kv.second->name == name && kv.second->state.load() < 3) return true;
    return false;
}

bool any_recording_active() {
    std::lock_guard<std::mutex> lk(g_rec_mu);
    for (const auto& kv : g_recs)
        if (kv.second && kv.second->state.load() < 3) return true;
    return false;
}

int contact_auto_interval_secs() {
    try { return std::clamp(std::stoi(env_or("LEQU_CONTACT_AUTO_SECONDS", "180")), 60, 3600); }
    catch (...) { return 180; }
}

bool contact_auto_enabled() {
    std::string v = env_or("LEQU_CONTACT_AUTO", "true");
    std::transform(v.begin(), v.end(), v.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return v != "0" && v != "false" && v != "off" && v != "no";
}

bool cpu_idle_for_contact_sheets() {
    if (any_recording_active()) return false;
    {
        std::lock_guard<std::mutex> lk(g_contact_mu);
        if (!g_contact_queue.empty()) return false;
    }
    if (g_contact_active.load() > 0) return false;

    double loads[3] = {0, 0, 0};
    unsigned int cores = std::max(1u, std::thread::hardware_concurrency());
    if (::getloadavg(loads, 3) != -1) {
        double threshold = std::max(1.0, static_cast<double>(cores) * 0.35);
        return loads[0] < threshold;
    }
    return true;
}

void contact_sheet_auto_loop() {
    while (true) {
        for (int i = 0; i < contact_auto_interval_secs(); ++i)
            std::this_thread::sleep_for(std::chrono::seconds(1));
        if (!contact_auto_enabled()) continue;
        if (!cpu_idle_for_contact_sheets()) continue;
        int queued = queue_missing_contact_sheets(3);
        if (queued > 0)
            std::cout << "[LeQu] idle contact sheet queued " << queued << " job(s)\n";
    }
}

std::string safe_name(std::string s) {
    for (char& c : s)
        if (c == '/' || c == '\\' || c == ':' || c == '<' || c == '>' || c == '"' ||
            c == '|' || c == '?' || c == '*' || (unsigned char)c < 0x20) c = '_';
    while (!s.empty() && (s.back() == ' ' || s.back() == '.')) s.pop_back();
    return s.empty() ? "lequ" : s;
}
std::string now_stamp() {
    std::time_t t = std::time(nullptr);
    std::tm tm{}; localtime_r(&t, &tm);
    char b[32]; std::strftime(b, sizeof b, "%Y%m%d-%H%M%S", &tm);
    return b;
}

// Resolve a fresh playUrl via /app/video/watch (never cached).
bool watch(const std::string& vid, std::string& play_url, bool& living, Anchor& owner) {
    Resp r = lequ_post("/app/video/watch", current_auth(), "vid=" + vid);
    json j;
    if (!parse_ok(r, j, "watch") || !j.is_object()) return false;
    play_url = jstr(j, "playUrl");
    living   = j.value("living", false);
    if (j.contains("owner") && j["owner"].is_object()) {
        owner.name = jstr(j["owner"], "name");
        owner.nickname = j["owner"].contains("nickname") ? jstr(j["owner"], "nickname")
                                                         : jstr(j["owner"], "nickName");
    }
    return true;
}

void set_record_retry_cooldown(const std::string& name) {
    if (name.empty()) return;
    std::lock_guard<std::mutex> lk(g_rec_mu);
    g_rec_retry_after[name] = std::time(nullptr) + record_retry_cooldown_secs();
}

void clear_record_retry_cooldown(const std::string& name) {
    if (name.empty()) return;
    std::lock_guard<std::mutex> lk(g_rec_mu);
    g_rec_retry_after.erase(name);
}

bool wait_record_cancelable(const std::shared_ptr<RecJob>& job, int seconds) {
    for (int i = 0; i < seconds * 10; ++i) {
        if (job->cancel.load()) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return true;
}

void record_heartbeat_loop(const std::shared_ptr<RecJob>& job,
                           const std::shared_ptr<std::string>& vid_ref,
                           const std::shared_ptr<std::mutex>& vid_mu,
                           const std::shared_ptr<std::atomic<bool>>& stop) {
    if (!record_heartbeat_enabled()) return;
    std::string priority = watch_priority();
    if (priority == "off") return;
    if (priority == "ab" && !ab_record_heartbeat_on(job->name)) {
        job->heartbeat_group = "off";
        std::cout << "[LeQu] record heartbeat disabled by A/B " << job->nickname
                  << " (" << job->name << ")" << std::endl;
        return;
    }
    job->heartbeat_group = (priority == "ab") ? "on" : priority;
    std::unique_lock<std::mutex> record_hb_lease;
    if (priority != "record" && priority != "ab") {
        record_hb_lease = std::unique_lock<std::mutex>(g_record_heartbeat_mu, std::try_to_lock);
        if (!record_hb_lease.owns_lock()) return;
    }

    std::string last_vid;
    RoomStats room;
    long long gid = 0, aid = 0, lt = 0, cid = 0;
    std::time_t last_join = 0;
    std::time_t last_skip_log = 0;
    int interval = record_heartbeat_secs();
    while (!stop->load() && !job->cancel.load()) {
        std::string vid;
        {
            std::lock_guard<std::mutex> lk(*vid_mu);
            vid = *vid_ref;
        }
        if (vid.empty()) {
            wait_record_cancelable(job, 3);
            continue;
        }
        priority = watch_priority();
        if (priority == "off") break;
        if ((priority == "record" || priority == "ab") && record_hb_lease.owns_lock())
            record_hb_lease.unlock();
        if (priority == "farm" && g_farm_heartbeat_users.load() > 0) {
            job->heartbeat_skipped.fetch_add(1);
            std::time_t now = std::time(nullptr);
            if (now - last_skip_log >= 60) {
                std::cout << "[LeQu] record heartbeat skipped " << job->nickname
                          << " (" << job->name << ") vid=" << vid
                          << " reason=farm-priority" << std::endl;
                last_skip_log = now;
            }
            wait_record_cancelable(job, interval);
            continue;
        }
        if (priority == "farm" && !record_hb_lease.owns_lock()) {
            record_hb_lease = std::unique_lock<std::mutex>(g_record_heartbeat_mu, std::try_to_lock);
            if (!record_hb_lease.owns_lock()) {
                wait_record_cancelable(job, interval);
                continue;
            }
        }
        std::unique_lock<std::mutex> account_watch;
        if (priority == "record" || priority == "ab") {
            account_watch = std::unique_lock<std::mutex>(g_account_watch_mu);
        } else {
            account_watch = std::unique_lock<std::mutex>(g_account_watch_mu, std::try_to_lock);
            if (!account_watch.owns_lock()) {
                job->heartbeat_skipped.fetch_add(1);
                wait_record_cancelable(job, interval);
                continue;
            }
        }

        const std::time_t now = std::time(nullptr);
        if (vid != last_vid || now - last_join > 60 || room.hcs_ip.empty()) {
            RoomStats fresh;
            if (fetch_room_stats(vid, fresh)) {
                room = fresh;
                last_join = now;
                if (vid != last_vid) {
                    gid = aid = lt = cid = 0;
                    last_vid = vid;
                }
            }
        }
        if (!room.hcs_ip.empty() && room.hcs_port > 0) {
            json st;
            if (fetch_status(room.hcs_ip, room.hcs_port, vid, cid, gid, aid, lt, st)
                && st.contains("ri") && st["ri"].is_object()) {
                const json& ri = st["ri"];
                gid = ri.value("gid", gid);
                aid = ri.value("aid", aid);
                lt  = ri.value("ut", lt);
                cid = ri.value("cid", cid);
                interval = std::clamp(ri.value("tivl", interval), 2, 30);
                job->last_heartbeat_at.store(static_cast<long long>(std::time(nullptr)));
                job->heartbeat_ok.fetch_add(1);
            } else {
                job->heartbeat_failed.fetch_add(1);
            }
        }
        wait_record_cancelable(job, interval);
    }
}

// Refresh the current broadcast for an anchor. The platform can rotate vid and
// signed playUrl after a brief disconnect, so checking only the original vid
// unnecessarily splits one live session into many files.
bool refresh_record_source(const std::shared_ptr<RecJob>& job, std::string& vid,
                           std::string& play_url, Anchor& owner) {
    bool living = false;
    if (watch(vid, play_url, living, owner) && living && !play_url.empty()) return true;

    for (const auto& kind : {"following", "hot", "recommended"}) {
        std::vector<Anchor> anchors;
        if (!fetch_list(kind, anchors)) continue;
        for (const auto& a : anchors) {
            if (a.name != job->name || !a.living || a.vid.empty()) continue;
            Anchor fresh_owner;
            std::string fresh_url;
            bool fresh_living = false;
            if (!watch(a.vid, fresh_url, fresh_living, fresh_owner)
                || !fresh_living || fresh_url.empty()) continue;
            vid = a.vid;
            play_url = std::move(fresh_url);
            owner = std::move(fresh_owner);
            return true;
        }
    }
    return false;
}

std::string ffconcat_escape(const std::string& path) {
    std::string out;
    out.reserve(path.size() + 8);
    for (char c : path) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    return out;
}

bool remux_recovered_recording(const std::string& id) {
    std::vector<std::string> sources;
    std::string output;
    {
        std::lock_guard<std::mutex> lk(g_rec_history_mu);
        for (const auto& r : g_rec_history) {
            if (r.value("id", "") != id) continue;
            sources = recording_sources(r);
            output = recovery_stem(sources.empty() ? "" : sources.front()) + ".mp4";
            break;
        }
    }
    if (sources.empty() || output == ".mp4") return false;
    std::error_code ec;
    for (const auto& source : sources)
        if (!fs::exists(source, ec) || file_bytes(source) <= 0 || !source_is_stale(source, std::time(nullptr)))
            return false;

    std::string concat = recovery_stem(sources.front()) + ".recovery.ffconcat";
    std::string command;
    if (sources.size() == 1) {
        command = "ffmpeg -nostdin -hide_banner -loglevel error -y -i "
                + shell_escape(sources.front()) + " -map 0 -c copy -movflags +faststart "
                + shell_escape(output);
    } else {
        std::ofstream list(concat);
        list << "ffconcat version 1.0\n";
        for (const auto& source : sources)
            list << "file '" << ffconcat_escape(source) << "'\n";
        list.close();
        command = "ffmpeg -nostdin -hide_banner -loglevel error -y -f concat -safe 0 -i "
                + shell_escape(concat) + " -map 0 -c copy -movflags +faststart "
                + shell_escape(output);
    }

    std::atomic<bool> never{false};
    int rc = cancellable_system(command, never);
    fs::remove(concat, ec);
    bool ok = rc == 0 && fs::exists(output, ec) && file_bytes(output) > 0;
    if (ok) {
        for (const auto& source : sources) fs::remove(source, ec);
    } else {
        fs::remove(output, ec);
    }

    std::lock_guard<std::mutex> lk(g_rec_history_mu);
    for (auto& r : g_rec_history) {
        if (r.value("id", "") != id) continue;
        r["remuxing"] = false;
        if (ok) {
            MediaProbe media = probe_media(output);
            r["path"] = output;
            r.erase("source_paths");
            r["state"] = "done";
            r["recoverable"] = false;
            r["size"] = file_bytes(output);
            r["duration"] = media.duration;
            r["width"] = media.width;
            r["height"] = media.height;
            r["fps"] = media.fps;
            r["ended_at"] = static_cast<long long>(std::time(nullptr));
            r.erase("remux_error");
        } else {
            r["state"] = "interrupted";
            r["recoverable"] = true;
            r["remux_error"] = "ffmpeg 转封装失败，源文件已保留";
        }
        break;
    }
    save_recordings_locked();
    std::cout << "[LeQu] recovery remux " << (ok ? "completed" : "failed")
              << " id=" << id << " sources=" << sources.size() << "\n";
    return ok;
}

void do_record(std::shared_ptr<RecJob> job) {
    std::string current_vid = job->vid;
    std::string play_url;
    Anchor owner;
    if (!refresh_record_source(job, current_vid, play_url, owner)) {
        job->state = 4;
        std::cerr << "[LeQu] record " << job->name << " vid=" << job->vid
                  << ": not live / no playUrl\n";
        set_record_retry_cooldown(job->name);
        std::lock_guard<std::mutex> lk(g_rec_mu);
        for (auto it = g_recs.begin(); it != g_recs.end(); ++it) {
            if (it->second == job) { g_recs.erase(it); break; }
        }
        return;
    }
    if (!owner.name.empty())     job->name = owner.name;
    if (!owner.nickname.empty()) job->nickname = owner.nickname;
    clear_record_retry_cooldown(job->name);
    {
        std::string priority = watch_priority();
        if (priority == "off") job->heartbeat_group = "off";
        else if (priority == "ab") job->heartbeat_group = ab_record_heartbeat_on(job->name) ? "on" : "off";
        else job->heartbeat_group = priority;
    }

    std::string dir = media_dir() + "/" + safe_name(job->name);
    std::error_code ec; fs::create_directories(dir, ec);
    std::string stem = dir + "/" + safe_name(job->nickname) + "_" + job->name + "_" + now_stamp();
    std::string flv = stem + ".flv";
    std::string mp4 = stem + ".mp4";
    std::string concat_file = stem + ".ffconcat";
    std::vector<std::string> parts;
    job->file = mp4;
    job->started_at = std::time(nullptr);
    job->state = 1;
    std::cout << "[LeQu] ↻ recording " << job->nickname << " (" << job->name
              << ") vid=" << current_vid << " hb=" << job->heartbeat_group
              << " → " << fs::path(mp4).filename().string() << std::endl;

    auto hb_stop = std::make_shared<std::atomic<bool>>(false);
    auto hb_vid = std::make_shared<std::string>(current_vid);
    auto hb_mu = std::make_shared<std::mutex>();
    std::thread heartbeat([job, hb_vid, hb_mu, hb_stop] {
        record_heartbeat_loop(job, hb_vid, hb_mu, hb_stop);
    });

    int part_no = 0;
    while (!job->cancel.load()) {
        char suffix[32];
        std::snprintf(suffix, sizeof suffix, ".part%04d.flv", part_no++);
        std::string part = stem + suffix;
        const long long timeout_us = static_cast<long long>(record_read_timeout_secs()) * 1000000LL;
        std::string cmd = "ffmpeg -nostdin -hide_banner -loglevel warning -rw_timeout "
                          + std::to_string(timeout_us) + " -i " + shell_escape(play_url)
                          + " -map 0 -c copy -f flv " + shell_escape(part);
        int capture_rc = cancellable_system(cmd, job->cancel);
        if (fs::exists(part, ec) && fs::file_size(part, ec) > 0) parts.push_back(part);
        else fs::remove(part, ec);
        if (job->cancel.load()) break;

        std::cout << "[LeQu] stream interrupted " << job->nickname << " (" << job->name
                  << ") rc=" << capture_rc << ", waiting up to "
                  << record_reconnect_grace_secs() << "s for reconnect" << std::endl;
        bool resumed = false;
        const auto deadline = std::chrono::steady_clock::now()
                            + std::chrono::seconds(record_reconnect_grace_secs());
        while (!job->cancel.load() && std::chrono::steady_clock::now() < deadline) {
            if (!wait_record_cancelable(job, 5)) break;
            Anchor fresh_owner;
            std::string fresh_url;
            std::string fresh_vid = current_vid;
            if (!refresh_record_source(job, fresh_vid, fresh_url, fresh_owner)) continue;
            current_vid = std::move(fresh_vid);
            play_url = std::move(fresh_url);
            {
                std::lock_guard<std::mutex> lk(*hb_mu);
                *hb_vid = current_vid;
            }
            if (!fresh_owner.nickname.empty()) job->nickname = fresh_owner.nickname;
            resumed = true;
            std::cout << "[LeQu] stream resumed " << job->nickname << " (" << job->name
                      << ") vid=" << current_vid << ", continuing same recording" << std::endl;
            break;
        }
        if (!resumed) break;
    }

    hb_stop->store(true);
    if (heartbeat.joinable()) heartbeat.join();

    bool stopped = job->cancel.load();
    bool have = !parts.empty();
    if (have) {
        job->state = 2;   // remux
        std::string remux;
        if (parts.size() == 1) {
            fs::rename(parts.front(), flv, ec);
            if (ec) {
                ec.clear();
                fs::copy_file(parts.front(), flv, fs::copy_options::overwrite_existing, ec);
            }
            remux = "ffmpeg -nostdin -hide_banner -loglevel error -y -i "
                    + shell_escape(flv) + " -map 0 -c copy -movflags +faststart "
                    + shell_escape(mp4);
        } else {
            std::ofstream list(concat_file);
            list << "ffconcat version 1.0\n";
            for (const auto& part : parts)
                list << "file '" << ffconcat_escape(part) << "'\n";
            list.close();
            remux = "ffmpeg -nostdin -hide_banner -loglevel error -y -f concat -safe 0 -i "
                    + shell_escape(concat_file) + " -map 0 -c copy -movflags +faststart "
                    + shell_escape(mp4);
        }
        std::atomic<bool> never{false};
        int rc = cancellable_system(remux, never);
        if (rc == 0 && fs::exists(mp4, ec) && fs::file_size(mp4, ec) > 0) {
            fs::remove(flv, ec);
            for (const auto& part : parts) fs::remove(part, ec);
            fs::remove(concat_file, ec);
            job->state = stopped ? 5 : 3;
            // Contact sheets are intentionally manual. Decoding 120 frames for
            // every completed recording can saturate CPU while several streams
            // are still recording.
            add_recording_history(job, mp4, "");
            std::cout << "[LeQu] ✓ saved " << fs::path(mp4).filename().string()
                      << " (" << parts.size() << " segment"
                      << (parts.size() == 1 ? "" : "s") << ")" << std::endl;
        } else {
            std::string kept = fs::exists(flv, ec) ? flv : parts.front();
            job->file = kept;     // keep at least the first playable segment if remux failed
            job->state = stopped ? 5 : 3;
            add_recording_history(job, kept, "");
            std::cerr << "[LeQu] remux failed; kept " << parts.size()
                      << " source segment(s), primary=" << fs::path(kept).filename().string() << "\n";
        }
    } else {
        job->state = 4;
        set_record_retry_cooldown(job->name);
        std::cerr << "[LeQu] ✗ no data captured for " << job->name << " vid=" << job->vid << "\n";
    }
    std::lock_guard<std::mutex> lk(g_rec_mu);
    for (auto it = g_recs.begin(); it != g_recs.end(); ++it) {
        if (it->second == job) { g_recs.erase(it); break; }
    }
}

// Begin recording an anchor's current live (by vid). No-op if already active.
bool start_recording(const std::string& name, const std::string& nickname,
                     const std::string& vid, bool respect_cooldown = true) {
    {
        std::lock_guard<std::mutex> lk(g_rec_mu);
        if (g_recs.count(vid)) return false;
        if (recording_name_active_locked(name)) return false;
        if (respect_cooldown) {
            auto retry = g_rec_retry_after.find(name);
            if (retry != g_rec_retry_after.end()) {
                if (retry->second > std::time(nullptr)) return false;
                g_rec_retry_after.erase(retry);
            }
        }
        auto job = std::make_shared<RecJob>();
        job->name = name; job->nickname = nickname; job->vid = vid;
        g_recs[vid] = job;
        spawn_guarded("record", [job] { do_record(job); });
    }
    return true;
}

void do_play(std::shared_ptr<PlayJob> job) {
    std::string play_url; bool living = false; Anchor owner;
    if (!watch(job->vid, play_url, living, owner) || !living || play_url.empty()) {
        job->failed = true;
    } else {
        if (!owner.name.empty()) job->name = owner.name;
        if (!owner.nickname.empty()) job->nickname = owner.nickname;
        std::error_code ec;
        fs::create_directories(job->dir, ec);
        const std::string playlist = job->dir + "/index.m3u8";
        const std::string segment = job->dir + "/seg_%06d.ts";
        std::string cmd =
            "ffmpeg -nostdin -hide_banner -loglevel warning"
            " -rw_timeout 20000000 -fflags nobuffer -flags low_delay -i " + shell_escape(play_url) +
            " -map 0:v:0 -map 0:a:0? -c copy"
            " -f hls -hls_time 1 -hls_list_size 6"
            " -hls_flags delete_segments+append_list+omit_endlist+independent_segments"
            " -hls_segment_filename " + shell_escape(segment) + " " + shell_escape(playlist);
        std::thread ready_watch([job, playlist] {
            for (int i = 0; i < 100 && !job->cancel.load(); ++i) {
                std::error_code ec;
                if (fs::exists(playlist, ec) && fs::file_size(playlist, ec) > 0) {
                    job->ready = true;
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        });
        int rc = cancellable_system(cmd, job->cancel);
        ready_watch.join();
        if (!job->cancel.load() && rc != 0) job->failed = true;
    }
    std::error_code ec;
    fs::remove_all(job->dir, ec);
    std::lock_guard<std::mutex> lk(g_play_mu);
    g_players.erase(job->id);
    std::cout << "[LeQu] player stopped vid=" << job->vid << " id=" << job->id << "\n";
}

std::shared_ptr<PlayJob> start_player(const std::string& vid,
                                      const std::string& name,
                                      const std::string& nickname) {
    std::lock_guard<std::mutex> lk(g_play_mu);
    for (const auto& kv : g_players) {
        if (kv.second->vid == vid && !kv.second->failed.load()) return kv.second;
    }
    auto job = std::make_shared<PlayJob>();
    job->id = rand_hex(16);
    job->vid = vid;
    job->name = name;
    job->nickname = nickname;
    job->dir = player_root() + "/" + job->id;
    job->started_at = std::time(nullptr);
    job->touched_at = job->started_at;
    g_players[job->id] = job;
    spawn_guarded("play", [job] { do_play(job); });
    std::thread([job] {
        while (!job->cancel.load() && !job->failed.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            if (std::time(nullptr) - job->touched_at.load() > 45) {
                std::cout << "[LeQu] player idle timeout vid=" << job->vid
                          << " id=" << job->id << "\n";
                job->cancel = true;
            }
        }
    }).detach();
    std::cout << "[LeQu] player starting vid=" << vid << " id=" << job->id << "\n";
    return job;
}

// ── low-latency HTTP-FLV ─────────────────────────────────────────────────────
// Source is rtmp, so remux straight to a continuous FLV stream (-c copy) instead
// of segmenting to HLS. Latency drops to ~1-2s and there are no per-segment
// stalls (the "转圈圈"). Returns the child pid + read-end fd; caller pumps the fd
// to the client and kills the process group on disconnect.
struct FlvPipe { pid_t pid = -1; int fd = -1; };
FlvPipe spawn_flv(const std::string& play_url) {
    int fds[2];
    if (pipe(fds) != 0) return {};
    pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return {}; }
    if (pid == 0) {
        dup2(fds[1], STDOUT_FILENO);
        close(fds[0]); close(fds[1]);
        setpgid(0, 0);
        execlp("ffmpeg", "ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "warning",
	               "-fflags", "nobuffer", "-flags", "low_delay",
	               "-analyzeduration", "1000000", "-probesize", "262144",
	               "-rw_timeout", "15000000", "-rtmp_live", "live",
               "-rtmp_buffer", "500", "-tcp_nodelay", "1",
               "-i", play_url.c_str(), "-c", "copy", "-f", "flv",
               "-flvflags", "no_duration_filesize", "-flush_packets", "1",
               "pipe:1", static_cast<char*>(nullptr));
        _exit(127);
    }
    close(fds[1]);
    return { pid, fds[0] };
}

// ── live lists ───────────────────────────────────────────────────────────────
const std::map<std::string, std::pair<std::string, std::string>>& list_endpoints() {
    static const std::map<std::string, std::pair<std::string, std::string>> m = {
        {"hot",         {"/app/video/hotlivelist",       "count=200&start=0&type=1"}},
        {"recommended", {"/app/video/recommendlivelist", "count=50&start=0&type=1"}},
        {"following",   {"/app/follower/livings",        "count=100&start=0&type=1"}},
    };
    return m;
}

bool fetch_list(const std::string& kind, std::vector<Anchor>& out) {
    auto it = list_endpoints().find(kind);
    if (it == list_endpoints().end()) return false;
    Resp r = lequ_post(it->second.first, current_auth(), it->second.second);
    json j;
    if (!parse_ok(r, j, "list:" + kind)) return false;
    collect_anchors(j, out);
    if (kind == "following") {
        for (auto& a : out) {
            a.followed = true;
            a.follow_known = true;
        }
    }
    return true;
}

void annotate_following(std::vector<Anchor>& anchors) {
    std::vector<Anchor> following;
    if (!fetch_list("following", following)) return;
    std::map<std::string, bool> names;
    for (const auto& a : following)
        if (!a.name.empty()) names[a.name] = true;
    for (auto& a : anchors) {
        if (a.name.empty()) continue;
        a.followed = names.count(a.name) != 0;
        a.follow_known = true;
    }
}

bool valid_lequ_id(const std::string& value) {
    if (value.empty() || value.size() > 80) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '_' || c == '-';
    });
}

bool fetch_room_stats(const std::string& vid, RoomStats& out) {
    const std::time_t now = std::time(nullptr);
    {
        std::lock_guard<std::mutex> lk(g_room_stats_mu);
        auto it = g_room_stats.find(vid);
        if (it != g_room_stats.end() && now - it->second.fetched_at < ROOM_STATS_TTL) {
            out = it->second;
            return true;
        }
    }

    std::string user;
    {
        std::lock_guard<std::mutex> lk(g_auth_mu);
        user = g_user;
    }
    if (!valid_lequ_id(user)) {
        std::cerr << "[LeQu] room stats: current user ID missing\n";
        return false;
    }
    Resp r = lequ_post("/app/video/join", current_auth(),
                       "vid=" + vid + "&name=" + user);
    json j;
    if (!parse_ok(r, j, "video/join")) return false;
    if (!j.contains("watchingCount") && !j.contains("watchCount")) {
        std::cerr << "[LeQu] video/join: room counters missing for vid=" << vid << "\n";
        return false;
    }
    RoomStats fresh;
    fresh.watching = j.value("watchingCount", 0LL);
    fresh.total = j.value("watchCount", 0LL);
    fresh.followed = j.value("follow", j.value("followed", false));
    fresh.hcs_ip = jstr(j, "hcsIp");                      // comment server (public IP)
    try { fresh.hcs_port = std::stoi(jstr(j, "hcsPort")); } catch (...) { fresh.hcs_port = 0; }
    fresh.fetched_at = now;
    {
        std::lock_guard<std::mutex> lk(g_room_stats_mu);
        g_room_stats[vid] = fresh;
    }
    out = fresh;
    return true;
}

// ── live comments / danmaku ──────────────────────────────────────────────────
// The room comment ("弹幕") feed is a separate plain-HTTP server whose address
// (hcsIp:hcsPort) is handed out by /app/video/join. Poll
//   GET http://<hcsIp>:<hcsPort>/getcomments?vid=&sid=&cid=<cursor>
// where cid is the largest item id seen so far (0 cold-start). The response is
//   {"rv":"ok","ri":{"cl":[ {tp,nm,nk,lg,ct,id,lvl,tn,rnk,...}, ... ]}}
// tp: 0=chat, 1=system, 2=follow notice. ct = text. NOT Zeus-signed.
struct Comment {
    long long id = 0;
    int type = 0;
    std::string name, nickname, logo, content, title, reply_nick;
    int level = 0;
};

// Per-vid watch heartbeat state so the player's comment-poll doubles as a real
// watch heartbeat (advancing gid/aid/lt), which is what credits watch-time toward
// the 5-min 么么哒. Keyed by vid; reset if the poll lapses (user left & returned).
struct WatchHB { long long gid = 0, aid = 0, lt = 0, cid = 0; std::time_t started = 0, last = 0; };
std::map<std::string, WatchHB> g_watch_hb;
std::mutex g_watch_hb_mu;

// url-encode a query value (the sid is opaque base64-ish; vid is alnum).
std::string url_encode(const std::string& s) {
    static const char* H = "0123456789ABCDEF";
    std::string o; o.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o.push_back(c);
        else { o.push_back('%'); o.push_back(H[c >> 4]); o.push_back(H[c & 0xF]); }
    }
    return o;
}

// GET getstatus heartbeat — returns the room's current comment head cid (and
// live counters). Plain HTTP, gzip (curl auto-decodes). Response is parsed JSON.
// gid/aid/lt thread the watch-session state back so the server credits watch
// time (needed to earn the 5-min 么么哒); pass 0s for a one-shot read.
bool fetch_status(const std::string& ip, int port, const std::string& vid,
                  long long cid, long long gid, long long aid, long long lt, json& out) {
    if (ip.empty() || port <= 0) return false;
    std::string sid = current_auth();
    if (sid.empty()) return false;
    CURL* c = curl_easy_init();
    if (!c) return false;
    std::string body;
    std::string url = "http://" + ip + ":" + std::to_string(port) + "/getstatus?vid="
                      + url_encode(vid) + "&hid=0&gid=" + std::to_string(gid)
                      + "&cnt=0&lt=" + std::to_string(lt) + "&aid=" + std::to_string(aid)
                      + "&sid=" + url_encode(sid) + "&cid=" + std::to_string(cid);
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_USERAGENT, user_agent().c_str());
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");   // accept+auto-decode any
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, wcb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 8L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    CURLcode rc = curl_easy_perform(c);
    curl_easy_cleanup(c);
    if (rc != CURLE_OK) return false;
    try { out = json::parse(body); } catch (...) { return false; }
    return true;
}

// Raw GET getcomments?vid=&sid=&cid= → parsed json (curl auto-decodes gzip).
bool fetch_comments_raw(const std::string& ip, int port, const std::string& vid,
                        long long cid, json& out) {
    if (ip.empty() || port <= 0) return false;
    std::string sid = current_auth();
    if (sid.empty()) return false;
    CURL* c = curl_easy_init();
    if (!c) return false;
    std::string body;
    std::string url = "http://" + ip + ":" + std::to_string(port) + "/getcomments?vid="
                      + url_encode(vid) + "&sid=" + url_encode(sid)
                      + "&cid=" + std::to_string(cid);
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_USERAGENT, user_agent().c_str());
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, wcb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 8L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    CURLcode rc = curl_easy_perform(c);
    curl_easy_cleanup(c);
    if (rc != CURLE_OK) return false;
    try { out = json::parse(body); } catch (...) { return false; }
    return true;
}

// Parse a getstatus `ri.cl` array into Comment structs (sorted by id).
void parse_comment_list(const json& cl, std::vector<Comment>& out, long long& max_id) {
    if (!cl.is_array()) return;
    for (const auto& e : cl) {
        if (!e.is_object()) continue;
        Comment cm;
        cm.id        = e.value("id", 0LL);
        cm.type      = e.value("tp", 0);
        cm.name      = jstr(e, "nm");
        cm.nickname  = jstr(e, "nk");
        cm.logo      = jstr(e, "lg");
        cm.content   = jstr(e, "ct");
        cm.title     = jstr(e, "tn");
        cm.reply_nick= jstr(e, "rnk");
        cm.level     = e.value("lvl", 0);
        if (cm.id > max_id) max_id = cm.id;
        out.push_back(std::move(cm));
    }
    std::sort(out.begin(), out.end(), [](const Comment& a, const Comment& b){ return a.id < b.id; });
}

// ── login challenges (SMS) ───────────────────────────────────────────────────
struct Challenge { std::string cookie_file, api_phone, local_phone, smsId; std::time_t created = 0; };
std::mutex g_ch_mu;
std::map<std::string, Challenge> g_challenges;

std::string rand_hex(int n) {
    std::vector<unsigned char> b(n);
    RAND_bytes(b.data(), n);
    static const char* H = "0123456789abcdef";
    std::string s; s.reserve(n * 2);
    for (unsigned char x : b) { s.push_back(H[x >> 4]); s.push_back(H[x & 0xF]); }
    return s;
}
void gc_challenges_locked() {
    std::time_t now = std::time(nullptr);
    for (auto it = g_challenges.begin(); it != g_challenges.end();) {
        if (now - it->second.created > 300) {
            std::error_code ec; fs::remove(it->second.cookie_file, ec);
            it = g_challenges.erase(it);
        } else ++it;
    }
}

// ── auth (JWT, same scheme as the rest of the panel) ─────────────────────────
bool rauth(const Request& req) {
    std::string tok;
    auto h = req.get_header_value("Authorization");
    if (h.rfind("Bearer ", 0) == 0) tok = h.substr(7);
    else if (req.has_param("token")) tok = req.get_param_value("token");
    return !tok.empty() && jwt_verify_hs256(tok);
}
void unauth(Response& res) {
    res.status = 401;
    res.set_content("{\"detail\":\"Unauthorized\"}", "application/json");
}
void send_json(Response& res, const json& j, int status = 200) {
    res.status = status;
    res.set_content(j.dump(), "application/json");
}
void send_lequ_upstream_error(Response& res, const std::string& message) {
    if (g_lequ_auth_expired.load()) {
        send_json(res, {{"error", "LeQu 登录已失效，请重新登录"},
                        {"lequ_auth_expired", true}}, 401);
        return;
    }
    send_json(res, {{"error", message}}, 502);
}
json body_json(const Request& req) {
    try { return json::parse(req.body); } catch (...) { return json::object(); }
}

// ── scheduler ────────────────────────────────────────────────────────────────
std::atomic<bool> g_sched_running{false};

void scheduler_loop() {
    while (g_sched_running.load()) {
        for (int i = 0; i < poll_secs() && g_sched_running.load(); ++i)
            std::this_thread::sleep_for(std::chrono::seconds(1));
        if (!g_sched_running.load()) break;
        if (current_auth().empty()) continue;

        json tracked;
        { std::lock_guard<std::mutex> lk(g_track_mu); tracked = g_tracked; }
        if (tracked.empty()) continue;

        // gather living anchors from all lists, keyed by stable name
        std::map<std::string, Anchor> living;
        for (const auto& kind : {"hot", "following", "recommended"}) {
            std::vector<Anchor> as;
            if (fetch_list(kind, as))
                for (auto& a : as)
                    if (a.living && !a.name.empty() && !a.vid.empty()) living[a.name] = a;
        }
        for (const auto& t : tracked) {
            std::string name = t.value("name", "");
            if (name.empty()) continue;
            auto it = living.find(name);
            if (it == living.end()) continue;
            std::string nick = it->second.nickname.empty() ? t.value("nickname", "") : it->second.nickname;
            if (start_recording(name, nick, it->second.vid))
                std::cout << "[LeQu] auto-start " << nick << " (" << name << ")\n";
        }
    }
}

const char* state_str(int s) {
    switch (s) { case 0: return "resolving"; case 1: return "recording"; case 2: return "remuxing";
                 case 3: return "done"; case 4: return "failed"; case 5: return "stopped"; }
    return "unknown";
}

json tracked_state_json() {
    json tracked;
    { std::lock_guard<std::mutex> lk(g_track_mu); tracked = g_tracked; }
    json arr = json::array();
    for (auto& t : tracked) {
        if (!t.is_object()) continue;
        json e = t;
        std::string name = t.value("name", "");
        e["recording"] = false;
        e["state"] = "idle";
        std::lock_guard<std::mutex> lk(g_rec_mu);
        for (auto& kv : g_recs) {
            if (kv.second->name != name) continue;
            e["recording"] = kv.second->state.load() < 3;
            e["state"] = state_str(kv.second->state.load());
            e["vid"] = kv.second->vid;
            break;
        }
        arr.push_back(std::move(e));
    }
    return arr;
}

json recordings_state_json() {
    std::vector<std::shared_ptr<RecJob>> jobs;
    {
        std::lock_guard<std::mutex> lk(g_rec_mu);
        jobs.reserve(g_recs.size());
        for (const auto& kv : g_recs) jobs.push_back(kv.second);
    }
    json arr = json::array();
    for (const auto& j : jobs) {
        long long bytes = 0;
        fs::path output(j->file);
        std::string stem = output.replace_extension("").string();
        std::error_code ec;
        for (int part = 0; part < 10000; ++part) {
            char suffix[32];
            std::snprintf(suffix, sizeof suffix, ".part%04d.flv", part);
            std::string path = stem + suffix;
            if (!fs::exists(path, ec)) break;
            bytes += file_bytes(path);
        }
        std::string flv = stem + ".flv";
        if (fs::exists(flv, ec)) bytes += file_bytes(flv);
        if (bytes == 0 && fs::exists(j->file, ec)) bytes = file_bytes(j->file);
        long long duration = j->started_at > 0
            ? std::max<long long>(0, std::time(nullptr) - j->started_at) : 0;
        arr.push_back({{"name", j->name}, {"nickname", j->nickname}, {"vid", j->vid},
                       {"state", state_str(j->state.load())},
                       {"started_at", (long long)j->started_at},
                       {"duration", duration}, {"size", bytes},
                       {"heartbeat_group", j->heartbeat_group},
                       {"last_heartbeat_at", j->last_heartbeat_at.load()},
                       {"heartbeat_ok", j->heartbeat_ok.load()},
                       {"heartbeat_failed", j->heartbeat_failed.load()},
                       {"heartbeat_skipped", j->heartbeat_skipped.load()},
                       {"file", fs::path(j->file).filename().string()}});
    }
    return arr;
}

json recordings_history_json() {
    json arr = json::array();
    std::lock_guard<std::mutex> lk(g_rec_history_mu);
    for (const auto& r : g_rec_history) {
        if (!r.is_object()) continue;
        std::string path = r.value("path", "");
        std::error_code ec;
        if (path.empty() || !fs::exists(path, ec)) continue;
        json item = r;
        std::string id = item.value("id", "");
        std::string thumb = item.value("thumb_path", "");
        std::string preview = thumb.empty() ? "" : contact_preview_path(thumb);
        item["source_count"] = static_cast<int>(recording_sources(r).size());
        item.erase("path");
        item.erase("thumb_path");
        item.erase("source_paths");
        item["file"] = fs::path(path).filename().string();
        item["size"] = file_bytes(path);
        if (!item.contains("duration") || !item["duration"].is_number()
            || item.value("duration", 0.0) <= 0) {
            long long started = item.value("started_at", 0LL);
            long long ended = item.value("ended_at", 0LL);
            item["duration"] = std::max<long long>(0, ended - started);
        }
        item["download_url"] = "/api/lequ/recordings/" + id + "/download";
        item["play_url"] = fs::path(path).extension() == ".mp4"
            ? "/api/lequ/recordings/" + id + "/play" : "";
        item["thumb_url"] = (!thumb.empty() && fs::exists(thumb, ec))
            ? "/api/lequ/recordings/" + id + "/thumb" : "";
        item["preview_url"] = (!preview.empty() && fs::exists(preview, ec))
            ? "/api/lequ/recordings/" + id + "/preview" : "";
        arr.push_back(std::move(item));
    }
    return arr;
}

json lequ_state_json() {
    return json{{"tracked", tracked_state_json()}, {"active", recordings_state_json()},
                {"history", recordings_history_json()}};
}

// ── heart farm (24/7 auto-watch → send 么么哒) ───────────────────────────────
// Earning is automatic: 5 min of getstatus heartbeats (advancing gid/aid/lt)
// credits one 么么哒 server-side (the app's "收下" button is cosmetic, no request).
// We then send it with /biz/v2/buy/gift giftId=348 (params confirmed from capture).
constexpr long long MOMODA_GIFT_ID = 348;
constexpr int       MOMODA_WATCH_SECS = 300;

// last raw upstream buy/gift response (verbatim from gw.lequ.com), for proof.
std::mutex g_last_gift_mu;
std::string g_last_gift_raw;
long long   g_last_gift_http = 0;
std::time_t g_last_gift_at = 0;

// Send a gift; returns true only when upstream code==0. Fills code/msg.
bool send_gift(const std::string& vid, const std::string& name, long long giftId,
               int giftNum, long long pkId, int buyType, int& code, std::string& msg) {
    std::string body = "vid=" + vid + "&giftId=" + std::to_string(giftId)
                     + "&pkId=" + std::to_string(pkId) + "&giftNum=" + std::to_string(giftNum)
                     + "&name=" + name + "&buyType=" + std::to_string(buyType)
                     + "&giftPositionType=0";
    Resp r = lequ_post("/biz/v2/buy/gift", current_auth(), body);
    {   // record the verbatim gateway response (proof it's a real upstream reply)
        std::lock_guard<std::mutex> lk(g_last_gift_mu);
        g_last_gift_raw = r.body; g_last_gift_http = r.status; g_last_gift_at = std::time(nullptr);
    }
    json j;
    if (!parse_ok(r, j, "buy/gift")) { code = -999; msg = "上游不可达"; return false; }
    if (!j.is_object()) { code = -998; msg = "异常响应"; return false; }   // never .value() an array
    code = j.value("code", 0);
    msg = jstr(j, "message");
    return code == 0;
}

struct FarmWorker {
    std::string id = rand_hex(12);
    std::string target;                 // assigned anchor name; "" = random slot
    std::string vid, name, nickname;
    std::atomic<int> watched{0};        // secs toward the next 么么哒
    std::atomic<int> sent{0};           // hearts sent by this worker
    std::atomic<int> state{0};          // 0 picking 1 watching 2 sending 3 idle/offline
    std::atomic<bool> stop{false};
    std::atomic<bool> paused{false};
    std::atomic<bool> repick{false};    // leave current room & re-pick (target changed)
    std::string last_error;
    std::time_t started_at = 0;
};

struct FarmState {
    std::atomic<bool> enabled{false};
    std::atomic<int>  concurrency{1};
    std::string mode = "random";        // random | specific
    std::vector<std::string> targets;   // specific anchor names
    std::atomic<long long> total_sent{0};
    std::mutex mu;
    std::vector<std::shared_ptr<FarmWorker>> workers;
};
FarmState g_farm;
std::set<std::string> g_farm_busy_vids;   // vids currently being farmed (random dedup)
std::mutex g_farm_busy_mu;

// On a FAILED send we leave the room; remember it briefly so the same worker
// doesn't immediately re-enter a room that just wouldn't credit. A successful
// send keeps the worker in place (the room grants another 么么哒 every 5 min).
std::map<std::string, std::time_t> g_farm_done;
std::mutex g_farm_done_mu;
constexpr int FARM_DONE_COOLDOWN = 1800;   // 30 min cooldown for a failed room
bool farm_vid_done(const std::string& vid) {
    std::lock_guard<std::mutex> lk(g_farm_done_mu);
    auto it = g_farm_done.find(vid);
    return it != g_farm_done.end() && std::time(nullptr) - it->second < FARM_DONE_COOLDOWN;
}
void farm_mark_done(const std::string& vid) {
    std::lock_guard<std::mutex> lk(g_farm_done_mu);
    g_farm_done[vid] = std::time(nullptr);
    // light gc
    if (g_farm_done.size() > 500) {
        std::time_t now = std::time(nullptr);
        for (auto it = g_farm_done.begin(); it != g_farm_done.end(); )
            it = (now - it->second > FARM_DONE_COOLDOWN) ? g_farm_done.erase(it) : std::next(it);
    }
}

// in-memory event log (shown on /momoda/ + flushed to stdout for journalctl)
struct FarmEvent { std::time_t at; std::string text; };
std::deque<FarmEvent> g_farm_log;
std::mutex g_farm_log_mu;
void farm_log(const std::string& text) {
    std::lock_guard<std::mutex> lk(g_farm_log_mu);
    g_farm_log.push_back({std::time(nullptr), text});
    while (g_farm_log.size() > 120) g_farm_log.pop_front();
    std::cout << "[LeQu][farm] " << text << std::endl;   // endl flushes → journalctl
}

std::string farm_file() { return cfg_dir() + "/lequ_farm.json"; }
void load_farm() {
    std::ifstream f(farm_file());
    if (!f) return;
    try {
        json j; f >> j;
        g_farm.concurrency = std::max(0, std::min(8, j.value("concurrency", 1)));
        g_farm.mode = j.value("mode", "random");
        g_farm.targets = j.value("targets", std::vector<std::string>{});
        g_farm.total_sent = j.value("total_sent", 0LL);
        // enabled is restored too so a 24/7 farm survives restarts
        g_farm.enabled = j.value("enabled", false);
    } catch (...) {}
}
void save_farm_locked() {
    json j{{"concurrency", g_farm.concurrency.load()}, {"mode", g_farm.mode},
           {"targets", g_farm.targets}, {"total_sent", g_farm.total_sent.load()},
           {"enabled", g_farm.enabled.load()}};
    std::ofstream f(farm_file());
    if (f) f << j.dump(2);
}

// Pick a living room for a worker. specific → the worker's assigned target's
// current vid; random → any living hot room not already farmed by a sibling.
bool farm_pick_room(const std::shared_ptr<FarmWorker>& w) {
    std::vector<Anchor> hot;
    fetch_list("hot", hot);
    if (!w->target.empty()) {
        std::lock_guard<std::mutex> lk(g_farm_busy_mu);
        for (auto& a : hot)
            if (a.name == w->target && a.living && !a.vid.empty() && !farm_vid_done(a.vid)) {
                if (g_farm_busy_vids.count(a.vid)) return false;
                w->vid = a.vid; w->name = a.name; w->nickname = a.nickname;
                g_farm_busy_vids.insert(a.vid);
                return true;
            }
        return false;   // target offline or this broadcast already farmed
    }
    std::lock_guard<std::mutex> lk(g_farm_busy_mu);
    for (auto& a : hot) {
        if (!a.living || a.vid.empty()) continue;
        if (g_farm_busy_vids.count(a.vid)) continue;   // another worker is on it
        if (farm_vid_done(a.vid)) continue;            // already farmed this broadcast
        w->vid = a.vid; w->name = a.name; w->nickname = a.nickname;
        g_farm_busy_vids.insert(a.vid);
        return true;
    }
    return false;
}
void farm_release_vid(const std::string& vid) {
    if (vid.empty()) return;
    std::lock_guard<std::mutex> lk(g_farm_busy_mu);
    g_farm_busy_vids.erase(vid);
}

// One worker: pick a room → heartbeat-watch 5 min → send 么么哒 → repeat.
void farm_worker_loop(std::shared_ptr<FarmWorker> w) {
    w->started_at = std::time(nullptr);
    while (!w->stop.load() && g_farm.enabled.load()) {
        if (w->paused.load()) {
            w->state = 4;
            farm_release_vid(w->vid); w->vid.clear(); w->watched = 0;
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }
        if (current_auth().empty() || g_lequ_auth_expired.load()) {
            w->state = 3; w->last_error = "LeQu 未登录";
            std::this_thread::sleep_for(std::chrono::seconds(10));
            continue;
        }
        w->state = 0;
        farm_release_vid(w->vid); w->vid.clear();
        if (!farm_pick_room(w)) {                      // nothing to watch right now
            w->state = 3;
            std::this_thread::sleep_for(std::chrono::seconds(w->target.empty() ? 8 : 20));
            continue;
        }
        std::string priority = watch_priority();
        if ((priority == "record" || priority == "ab") && any_recording_active()) {
            w->state = 3;
            w->last_error = priority == "ab" ? "AB测试：等待录制空闲" : "录制优先：等待录制空闲";
            farm_release_vid(w->vid); w->vid.clear();
            std::this_thread::sleep_for(std::chrono::seconds(10));
            continue;
        }
        w->state = 3;
        w->last_error = "等待账号观看通道";
        std::unique_lock<std::mutex> account_watch(g_account_watch_mu);
        // resolve comment/heat server (also "joins" the room)
        RoomStats rs;
        if (!fetch_room_stats(w->vid, rs) || rs.hcs_ip.empty()) {
            w->last_error = "无法进入房间"; farm_release_vid(w->vid); w->vid.clear();
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }
        // heartbeat-watch loop (advance gid/aid/lt/cid like the app)
        long long gid = 0, aid = 0, lt = 0, cid = 0;
        w->watched = 0;
        w->state = 1;
        farm_log("▶ 进入房间 " + w->nickname + " (" + w->name + ")，开始挂 5 分钟");
        bool room_dead = false;
        struct FarmHeartbeatUse {
            std::atomic<int>& users;
            explicit FarmHeartbeatUse(std::atomic<int>& v) : users(v) { users.fetch_add(1); }
            ~FarmHeartbeatUse() { users.fetch_sub(1); }
        } farm_hb_use(g_farm_heartbeat_users);
        while (!w->stop.load() && g_farm.enabled.load() && !room_dead) {
            if (w->paused.load()) break;
            if (w->repick.exchange(false)) break;   // target changed → leave & re-pick
            priority = watch_priority();
            if ((priority == "record" || priority == "ab") && any_recording_active()) {
                w->last_error = priority == "ab" ? "AB测试：释放观看通道" : "录制优先：释放观看通道";
                break;
            }
            json st;
            if (fetch_status(rs.hcs_ip, rs.hcs_port, w->vid, cid, gid, aid, lt, st)
                && st.contains("ri") && st["ri"].is_object()) {
                const json& ri = st["ri"];
                if (jstr(ri, "st") == "0") { room_dead = true; break; }   // st 0 = ended
                gid = ri.value("gid", gid);
                aid = ri.value("aid", aid);
                lt  = ri.value("ut", lt);
                cid = ri.value("cid", cid);
            }
            int tivl = 3;
            for (int i = 0; i < tivl && !w->stop.load() && !w->paused.load()
                            && g_farm.enabled.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                w->watched = w->watched.load() + 1;
            }
            if (w->watched.load() >= MOMODA_WATCH_SECS) {
                w->state = 2;
                int code = 0; std::string msg;
                if (send_gift(w->vid, w->name, MOMODA_GIFT_ID, 1, 0, 0, code, msg)) {
                    w->sent = w->sent.load() + 1;
                    g_farm.total_sent = g_farm.total_sent.load() + 1;
                    { std::lock_guard<std::mutex> lk(g_farm.mu); save_farm_locked(); }
                    w->last_error.clear();
                    farm_log("🫶 送出么么哒 → " + w->nickname + " (" + w->name +
                             ")，累计 " + std::to_string(g_farm.total_sent.load()));
                    w->watched = 0;        // same room grants another in 5 min → stay & farm on
                    w->state = 1;
                } else {
                    // didn't credit — leave this room (brief cooldown) and re-pick
                    w->last_error = "送出失败(" + std::to_string(code) + ") " + msg + "，换房间";
                    farm_log("⚠ 送出失败 " + std::to_string(code) + " " + msg + " → " +
                             w->nickname + " (" + w->name + ")，换房间");
                    farm_mark_done(w->vid);
                    break;
                }
            }
        }
        farm_release_vid(w->vid); w->vid.clear();
    }
    w->state = 3;
    farm_release_vid(w->vid);
}

void farm_start_claimer();   // fwd

// Random concurrency and specific targets are independent worker groups.
// Reconcile by worker identity so changing the editor mode or adding a target
// never repurposes an in-progress random worker and loses its watch progress.
void farm_reconcile() {
    std::lock_guard<std::mutex> lk(g_farm.mu);
    if (!g_farm.enabled.load()) {
        for (auto& w : g_farm.workers) w->stop = true;
        g_farm.workers.clear();
        return;
    }
    const int random_count = std::max(0, std::min(8, g_farm.concurrency.load()));
    const std::set<std::string> desired_targets(g_farm.targets.begin(), g_farm.targets.end());
    std::set<std::string> kept_targets;
    std::vector<std::shared_ptr<FarmWorker>> kept;
    int kept_random = 0;

    for (auto& w : g_farm.workers) {
        bool keep = false;
        if (w->target.empty()) {
            keep = kept_random < random_count;
            if (keep) ++kept_random;
        } else if (desired_targets.count(w->target) && !kept_targets.count(w->target)) {
            keep = true;
            kept_targets.insert(w->target);
        }
        if (keep) kept.push_back(w);
        else w->stop = true;
    }
    g_farm.workers.swap(kept);

    while (kept_random < random_count) {
        auto w = std::make_shared<FarmWorker>();
        g_farm.workers.push_back(w);
        ++kept_random;
        spawn_guarded("farm-worker", [w] { farm_worker_loop(w); });
    }
    for (const auto& target : g_farm.targets) {
        if (kept_targets.count(target)) continue;
        auto w = std::make_shared<FarmWorker>();
        w->target = target;
        g_farm.workers.push_back(w);
        kept_targets.insert(target);
        spawn_guarded("farm-worker", [w] { farm_worker_loop(w); });
    }
    farm_start_claimer();
}

// While farming, periodically claim completed daily tasks (status==2). The farm
// naturally completes "观看直播10分钟"(id33) and "打赏礼物"(id23) by watching+gifting,
// so the diamonds just sit there claimable. Harmless: only claims status==2.
std::atomic<bool> g_farm_claimer_running{false};
void farm_claimer_loop() {
    while (g_farm.enabled.load()) {
        for (int i = 0; i < 60 && g_farm.enabled.load(); ++i)
            std::this_thread::sleep_for(std::chrono::seconds(1));
        if (!g_farm.enabled.load()) break;
        if (current_auth().empty() || g_lequ_auth_expired.load()) continue;
        Resp r = lequ_post("/app/general/personal/task/info", current_auth(), "");
        json j;
        if (!parse_ok(r, j, "task/info") || !j.contains("taskList") || !j["taskList"].is_array()) continue;
        for (auto& t : j["taskList"]) {
            if (!t.is_object()) continue;
            if (t.value("status", 0) != 2) continue;     // 2 = completed & claimable
            long long tid = t.value("taskId", 0LL);
            if (tid <= 0) continue;
            Resp cr = lequ_post("/app/general/personal/task/fulfill", current_auth(),
                                "taskId=" + std::to_string(tid));
            json cj;
            if (parse_ok(cr, cj, "task/fulfill"))
                farm_log("💎 领取任务 " + jstr(t, "title") + " +" +
                         std::to_string(t.value("score", 0)) + jstr(t, "unit"));
        }
    }
    g_farm_claimer_running = false;
}
void farm_start_claimer() {
    if (g_farm_claimer_running.exchange(true)) return;
    spawn_guarded("farm-claimer", [] { farm_claimer_loop(); });
}

json farm_status_json() {
    json arr = json::array();
    std::lock_guard<std::mutex> lk(g_farm.mu);
    for (auto& w : g_farm.workers) {
        const char* s = w->state.load()==0?"picking":w->state.load()==1?"watching":
                        w->state.load()==2?"sending":w->state.load()==4?"paused":"idle";
        arr.push_back({{"id", w->id}, {"target", w->target},
                       {"name", w->name}, {"nickname", w->nickname},
                       {"vid", w->vid}, {"watched", w->watched.load()},
                       {"need", MOMODA_WATCH_SECS}, {"sent", w->sent.load()},
                       {"state", s}, {"paused", w->paused.load()}, {"error", w->last_error}});
    }
    json log = json::array();
    {
        std::lock_guard<std::mutex> lk(g_farm_log_mu);
        for (auto it = g_farm_log.rbegin(); it != g_farm_log.rend(); ++it)   // newest first
            log.push_back({{"at", (long long)it->at}, {"text", it->text}});
    }
    json last_gift;
    {
        std::lock_guard<std::mutex> lk(g_last_gift_mu);
        last_gift = {{"at", (long long)g_last_gift_at}, {"http", g_last_gift_http},
                     {"raw", g_last_gift_raw}};
    }
    return json{{"enabled", g_farm.enabled.load()}, {"concurrency", g_farm.concurrency.load()},
                {"mode", g_farm.mode}, {"targets", g_farm.targets},
                {"total_sent", g_farm.total_sent.load()}, {"workers", arr}, {"log", log},
                {"last_gift_response", last_gift}};
}

void recovery_scanner_loop() {
    while (g_recovery_scanner_running.load()) {
        for (int i = 0; i < 300 && g_recovery_scanner_running.load(); ++i)
            std::this_thread::sleep_for(std::chrono::seconds(1));
        if (!g_recovery_scanner_running.load()) break;
        scan_recoverable_recordings();
    }
}

}  // namespace

// ── routes ───────────────────────────────────────────────────────────────────
void register_lequ_routes(httplib::Server& svr) {
    // Never let a handler exception abort the process — return 500 instead.
    svr.set_exception_handler([](const Request&, Response& res, std::exception_ptr ep) {
        std::string what = "internal error";
        try { if (ep) std::rethrow_exception(ep); }
        catch (const std::exception& e) { what = e.what(); }
        catch (...) {}
        std::cerr << "[web] handler exception: " << what << "\n";
        res.status = 500;
        res.set_content(json{{"error", "服务器内部错误"}}.dump(), "application/json");
    });
    load_auth();
    load_tracked();
    load_lequ_settings();
    load_be_cache();
    load_farm();
    load_recordings();

    // --- auth status / logout ---
    svr.Get("/api/lequ/auth/status", [](const Request& req, Response& res) {
        if (!rauth(req)) return unauth(res);
        std::lock_guard<std::mutex> lk(g_auth_mu);
        send_json(res, {{"logged_in", !g_session.empty() && !g_lequ_auth_expired.load()},
                        {"expired", g_lequ_auth_expired.load()},
                        {"user_id", g_user}, {"phone", g_phone}});
    });
    svr.Post("/api/lequ/auth/logout", [](const Request& req, Response& res) {
        if (!rauth(req)) return unauth(res);
        { std::lock_guard<std::mutex> lk(g_auth_mu); g_session.clear(); g_user.clear(); g_phone.clear(); }
        g_lequ_auth_expired.store(false);
        save_auth();
        send_json(res, {{"ok", true}});
    });

    // --- SMS login: send → (user enters code) → complete ---
    svr.Post("/api/lequ/auth/sms/send", [](const Request& req, Response& res) {
        if (!rauth(req)) return unauth(res);
        json b = body_json(req);
        std::string phone = b.value("phone", "");
        std::string country = b.value("country", "86");
        std::string local; for (char c : phone) if (isdigit((unsigned char)c)) local += c;
        // strip leading country code if the user typed it inline
        if (local.rfind(country, 0) == 0 && local.size() > country.size() + 6) local = local.substr(country.size());
        if (local.empty()) return send_json(res, {{"error", "手机号无效"}}, 400);
        std::string api_phone = country + "_" + local;

        std::string id = rand_hex(12);
        std::string ck = cfg_dir() + "/.lequ_ck_" + id;
        Resp r = lequ_post("/app/sms/send", "", "phone=" + api_phone + "&sendType=8", ck);
        json j;
        if (!parse_ok(r, j, "sms/send")) { std::error_code ec; fs::remove(ck, ec);
            return send_json(res, {{"error", "短信发送失败"}}, 502); }
        std::string smsId = jstr(j, "smsId");
        if (smsId.empty()) { std::error_code ec; fs::remove(ck, ec);
            return send_json(res, {{"error", "网关未返回 smsId"}}, 502); }
        { std::lock_guard<std::mutex> lk(g_ch_mu); gc_challenges_locked();
          g_challenges[id] = {ck, api_phone, local, smsId, std::time(nullptr)}; }
        send_json(res, {{"challenge_id", id}, {"expires_in", 300}});
    });

    svr.Post("/api/lequ/auth/sms/complete", [](const Request& req, Response& res) {
        if (!rauth(req)) return unauth(res);
        json b = body_json(req);
        std::string id = b.value("challenge_id", "");
        std::string code = b.value("code", "");
        Challenge ch;
        { std::lock_guard<std::mutex> lk(g_ch_mu); gc_challenges_locked();
          auto it = g_challenges.find(id);
          if (it == g_challenges.end()) return send_json(res, {{"error", "验证会话已过期，请重新发送"}}, 400);
          ch = it->second; }
        if (code.empty()) return send_json(res, {{"error", "请输入验证码"}}, 400);

        Resp v = lequ_post("/app/sms/verify", "", "smsId=" + ch.smsId + "&smsCode=" + code, ch.cookie_file);
        if (!v.transport_ok || v.status >= 400) {
            json vj; parse_ok(v, vj, "sms/verify");   // logs gateway code if present
            return send_json(res, {{"error", "验证码校验失败"}}, 400);
        }
        std::string login_body =
            "auth.accessToken=" + ch.local_phone + "&auth.token=" + ch.local_phone +
            "&auth.password=&auth.phoneAuthType=CODE&auth.phone=" + ch.api_phone + "&auth.authType=PHONE";
        Resp lr = lequ_post("/app/user/login/reg", "", login_body, ch.cookie_file);
        json lj;
        if (!parse_ok(lr, lj, "login/reg")) return send_json(res, {{"error", "登录失败"}}, 502);
        std::string sid = jstr(lj, "sessionId");
        if (sid.empty()) return send_json(res, {{"error", "网关未返回 sessionId"}}, 502);

        { std::lock_guard<std::mutex> lk(g_auth_mu); g_session = sid; g_user = jstr(lj, "name"); g_phone = ch.api_phone; }
        g_lequ_auth_expired.store(false);
        save_auth();
        { std::lock_guard<std::mutex> lk(g_ch_mu); std::error_code ec; fs::remove(ch.cookie_file, ec); g_challenges.erase(id); }
        std::lock_guard<std::mutex> lk(g_auth_mu);
        std::cout << "[LeQu] login ok, user=" << g_user << "\n";
        send_json(res, {{"logged_in", true}, {"user_id", g_user}});
    });

    // --- live lists + search ---
    svr.Get("/api/lequ/live", [](const Request& req, Response& res) {
        if (!rauth(req)) return unauth(res);
        if (current_auth().empty()) return send_json(res, {{"error", "未登录 LeQu"}}, 401);
        std::string kind = req.has_param("type") ? req.get_param_value("type") : "hot";
        std::vector<Anchor> as;
        if (!fetch_list(kind, as)) return send_lequ_upstream_error(res, "拉取列表失败");
        if (kind != "following") annotate_following(as);
        json arr = json::array();
        for (auto& a : as) arr.push_back(anchor_to_json(a));
        send_json(res, {{"type", kind}, {"list", arr}});
    });

    svr.Get("/api/lequ/search", [](const Request& req, Response& res) {
        if (!rauth(req)) return unauth(res);
        if (current_auth().empty()) return send_json(res, {{"error", "未登录 LeQu"}}, 401);
        std::string q = req.has_param("q") ? req.get_param_value("q") : "";
        std::map<std::string, Anchor> uniq;   // key name|vid
        for (const char* kind : {"hot", "following", "recommended"}) {
            std::vector<Anchor> as;
            if (fetch_list(kind, as)) for (auto& a : as) {
                const std::string key = a.name + "|" + a.vid;
                auto it = uniq.find(key);
                if (it == uniq.end()) {
                    uniq[key] = a;
                } else {
                    it->second.followed = it->second.followed || a.followed;
                    it->second.follow_known = it->second.follow_known || a.follow_known;
                }
            }
        }
        if (g_lequ_auth_expired.load())
            return send_lequ_upstream_error(res, "搜索直播列表失败");
        std::string ql = q; for (char& c : ql) c = (char)tolower((unsigned char)c);
        json arr = json::array();
        for (auto& kv : uniq) {
            const Anchor& a = kv.second;
            std::string nl = a.nickname; for (char& c : nl) c = (char)tolower((unsigned char)c);
            bool hit = q.empty() || a.name == q || a.vid == q ||
                       (!ql.empty() && nl.find(ql) != std::string::npos);
            if (hit) arr.push_back(anchor_to_json(a));
        }
        send_json(res, {{"q", q}, {"list", arr}});
    });

    svr.Get("/api/lequ/user/info", [](const Request& req, Response& res) {
        if (!rauth(req)) return unauth(res);
        if (current_auth().empty() || g_lequ_auth_expired.load())
            return send_json(res, {{"error", "LeQu 登录已失效，请重新登录"},
                                   {"lequ_auth_expired", true}}, 401);
        std::string target = req.has_param("name") ? req.get_param_value("name") : "";
        bool self = target.empty();
        if (self) {
            std::lock_guard<std::mutex> lk(g_auth_mu);
            target = g_user;
        }
        if (!valid_lequ_id(target)) return send_json(res, {{"error", "用户 ID 无效"}}, 400);
        std::string body = "field=all&name=" + target + "&extendField=percent%2Cpersonal";
        if (!self) body += "&anchorName=" + target;
        Resp r = lequ_post("/app/user/userInfo", current_auth(), body);
        json j;
        if (!parse_ok(r, j, "userInfo") || !j.is_object()) return send_lequ_upstream_error(res, "获取用户信息失败");
        json profile{
            {"self", self}, {"name", jstr(j, "name")}, {"nickname", jstr(j, "nickname")},
            {"logo", jstr(j, "logoUrl")}, {"gender", jstr(j, "gender")},
            {"signature", jstr(j, "signature")}, {"location", jstr(j, "location")},
            {"country", jstr(j, "country")}, {"birthday", jstr(j, "birthday")},
            {"level", j.value("level", 0)}, {"vip_level", j.value("vipLevel", 0)},
            {"anchor_level", j.value("anchorLevel", 0)},
            {"follow_count", j.value("followCount", 0LL)},
            {"fans_count", j.value("fansCount", 0LL)},
            {"followed", j.value("followed", false)}, {"living", j.value("living", false)},
            {"online", j.value("onlineStatus", false)},
            {"certification", j.value("certification", 0)},
            {"clips_count", j.value("clipsCount", 0LL)},
            {"trends_count", j.value("trendsCount", 0LL)},
            {"fans_group_name", jstr(j, "fansGroupName")},
            {"fans_group_level", j.value("fansGroupLevel", 0)}
        };
        send_json(res, {{"profile", profile}});
    });

    svr.Get("/api/lequ/live/stats", [](const Request& req, Response& res) {
        if (!rauth(req)) return unauth(res);
        if (current_auth().empty()) return send_json(res, {{"error", "未登录 LeQu"}}, 401);
        std::string vid = req.has_param("vid") ? req.get_param_value("vid") : "";
        if (!valid_lequ_id(vid)) return send_json(res, {{"error", "直播 vid 无效"}}, 400);
        RoomStats stats;
        if (!fetch_room_stats(vid, stats))
            return send_lequ_upstream_error(res, "获取房间人数失败");
        send_json(res, {{"vid", vid},
                        {"watching_count", stats.watching},
                        {"watch_count", stats.total},
                        {"followed", stats.followed},
                        {"cached_for", ROOM_STATS_TTL}});
    });

    // --- live comments / danmaku (incremental poll by cid cursor) ---
    svr.Get("/api/lequ/comments", [](const Request& req, Response& res) {
        if (!rauth(req)) return unauth(res);
        if (current_auth().empty()) return send_json(res, {{"error", "未登录 LeQu"}}, 401);
        std::string vid = req.has_param("vid") ? req.get_param_value("vid") : "";
        if (!valid_lequ_id(vid)) return send_json(res, {{"error", "直播 vid 无效"}}, 400);
        long long cid = 0;
        if (req.has_param("cid")) { try { cid = std::stoll(req.get_param_value("cid")); } catch (...) {} }
        RoomStats stats;
        if (!fetch_room_stats(vid, stats))   // also yields hcsIp/hcsPort (10s cached)
            return send_lequ_upstream_error(res, "获取评论服务器地址失败");
        if (stats.hcs_ip.empty() || stats.hcs_port <= 0)
            return send_json(res, {{"comments", json::array()}, {"cid", cid},
                                   {"note", "该直播间未提供评论服务器"}});
        // Advancing watch heartbeat (per-vid): this makes the player's poll credit
        // real watch-time toward the 5-min 么么哒. getstatus reports the current head
        // cursor (ri.cid); getcomments?cid=<head> returns the ~10-item chat window.
        WatchHB hb;
        std::time_t now = std::time(nullptr);
        {
            std::lock_guard<std::mutex> lk(g_watch_hb_mu);
            auto& s = g_watch_hb[vid];
            if (s.started == 0 || now - s.last > 20) { s = WatchHB{}; s.started = now; }  // fresh session
            s.last = now;
            hb = s;
        }
        json st;
        if (!fetch_status(stats.hcs_ip, stats.hcs_port, vid, hb.cid, hb.gid, hb.aid, hb.lt, st)
            || !st.contains("ri") || !st["ri"].is_object())
            return send_json(res, {{"error", "拉取评论失败"}}, 502);
        const json& ri = st["ri"];
        long long head = ri.value("cid", 0LL);
        {   // advance heartbeat state from the response
            std::lock_guard<std::mutex> lk(g_watch_hb_mu);
            auto& s = g_watch_hb[vid];
            s.gid = ri.value("gid", s.gid); s.aid = ri.value("aid", s.aid);
            s.lt = ri.value("ut", s.lt);   s.cid = head;
        }
        std::vector<Comment> cs; long long max_id = cid;
        if (head > 0) {
            json gc;
            if (fetch_comments_raw(stats.hcs_ip, stats.hcs_port, vid, head, gc)
                && gc.contains("ri") && gc["ri"].contains("cl"))
                parse_comment_list(gc["ri"]["cl"], cs, max_id);
        }
        json arr = json::array();
        for (const auto& c : cs) {
            if (c.id <= cid) continue;   // already delivered
            arr.push_back({{"id", c.id}, {"type", c.type}, {"name", c.name},
                           {"nickname", c.nickname}, {"logo", c.logo}, {"content", c.content},
                           {"title", c.title}, {"reply_nick", c.reply_nick}, {"level", c.level}});
        }
        long long watched = now - hb.started;
        send_json(res, {{"comments", arr}, {"cid", std::max(max_id, cid)},
                        {"watching_count", stats.watching},
                        {"watched_secs", watched < 0 ? 0 : watched}, {"need_secs", MOMODA_WATCH_SECS}});
    });

    // --- low-latency HTTP-FLV (preferred over HLS) ---
    svr.Get("/api/lequ/playurl", [](const Request& req, Response& res) {
        if (!rauth(req)) return unauth(res);
        if (current_auth().empty()) return send_json(res, {{"error", "未登录 LeQu"}}, 401);
        std::string vid = req.has_param("vid") ? req.get_param_value("vid") : "";
        if (!valid_lequ_id(vid)) return send_json(res, {{"error", "直播 vid 无效"}}, 400);
        auto t0 = std::chrono::steady_clock::now();
        std::string play_url; bool living = false; Anchor owner;
        if (!watch(vid, play_url, living, owner)) {
            std::cerr << "[LeQu] playurl upstream watch failed vid=" << vid << "\n";
            return send_lequ_upstream_error(res, "获取直播源流失败");
        }
        if (!living || play_url.empty()) {
            std::cerr << "[LeQu] playurl unavailable vid=" << vid << " living=" << living << "\n";
            return send_json(res, {{"error", "直播已结束或源流不可用"}, {"living", living}}, 409);
        }
        auto watch_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        std::cout << "[LeQu] playurl issued vid=" << vid << " watch_ms=" << watch_ms << "\n";
        send_json(res, {{"vid", vid}, {"living", living}, {"playUrl", play_url},
                        {"owner", {{"name", owner.name}, {"nickname", owner.nickname}, {"logo", owner.logo}}},
                        {"watch_ms", watch_ms}});
    });

    svr.Get("/api/lequ/flv", [](const Request& req, Response& res) {
        if (!rauth(req)) return unauth(res);
        if (current_auth().empty()) return send_json(res, {{"error", "未登录 LeQu"}}, 401);
        std::string vid = req.has_param("vid") ? req.get_param_value("vid") : "";
        if (!valid_lequ_id(vid)) return send_json(res, {{"error", "直播 vid 无效"}}, 400);
        auto t0 = std::chrono::steady_clock::now();
        std::string play_url; bool living = false; Anchor owner;
        if (!watch(vid, play_url, living, owner)) {
            std::cerr << "[LeQu] flv upstream watch failed vid=" << vid << "\n";
            return send_lequ_upstream_error(res, "获取直播流失败");
        }
        if (!living || play_url.empty()) {
            std::cerr << "[LeQu] flv unavailable vid=" << vid << " living=" << living << "\n";
            return send_json(res, {{"error", "直播已结束或源流不可用"}, {"living", living}}, 409);
        }
        auto watch_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        std::cout << "[LeQu] flv stream start vid=" << vid << " watch_ms=" << watch_ms << "\n";
        res.set_header("Cache-Control", "no-store");
        res.set_header("X-Accel-Buffering", "no");
        res.set_chunked_content_provider(
            "video/x-flv",
            [play_url, vid, t0](size_t, httplib::DataSink& sink) -> bool {
                FlvPipe p = spawn_flv(play_url);
                if (p.fd < 0) {
                    std::cerr << "[LeQu] flv spawn failed vid=" << vid << "\n";
                    return false;
                }
                std::vector<char> buf(65536);
                bool first = true;
                size_t total = 0;
                for (;;) {
                    ssize_t n = read(p.fd, buf.data(), buf.size());
                    if (n <= 0) break;
                    if (first) {
                        first = false;
                        auto first_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0).count();
                        std::cout << "[LeQu] flv first bytes vid=" << vid
                                  << " first_ms=" << first_ms << "\n";
                    }
                    total += static_cast<size_t>(n);
                    if (!sink.is_writable() || !sink.write(buf.data(), (size_t)n)) break;
                }
                close(p.fd);
                kill(-p.pid, SIGKILL);          // stop ffmpeg when the viewer leaves
                int st; waitpid(p.pid, &st, 0);
                std::cout << "[LeQu] flv stream end vid=" << vid
                          << " bytes=" << total << " status=" << st << "\n";
                sink.done();
                return true;
            });
    });

    svr.Post("/api/lequ/player/start", [](const Request& req, Response& res) {
        if (!rauth(req)) return unauth(res);
        if (current_auth().empty()) return send_json(res, {{"error", "未登录 LeQu"}}, 401);
        json b = body_json(req);
        std::string vid = b.value("vid", "");
        std::string name = b.value("name", "");
        std::string nickname = b.value("nickname", "");
        if (!valid_lequ_id(vid)) return send_json(res, {{"error", "直播 vid 无效"}}, 400);
        auto job = start_player(vid, name, nickname);
        // Wait only long enough to catch an immediate failure (watch() ~2s); the
        // frontend then polls the playlist itself, so we don't hog a worker thread
        // for the full segment-generation time (~6s).
        for (int i = 0; i < 30 && !job->ready.load() && !job->failed.load(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (job->failed.load())
            return send_lequ_upstream_error(res, "直播流解析或 HLS 转封装失败");
        send_json(res, {{"ok", true}, {"session_id", job->id},
                        {"ready", job->ready.load()},
                        {"playlist", "/api/lequ/player/" + job->id + "/index.m3u8"},
                        {"vid", vid}, {"name", job->name}, {"nickname", job->nickname}});
    });

    svr.Post("/api/lequ/player/stop", [](const Request& req, Response& res) {
        if (!rauth(req)) return unauth(res);
        std::string id = body_json(req).value("session_id", "");
        std::shared_ptr<PlayJob> job;
        {
            std::lock_guard<std::mutex> lk(g_play_mu);
            auto it = g_players.find(id);
            if (it != g_players.end()) job = it->second;
        }
        if (job) job->cancel = true;
        send_json(res, {{"ok", true}});
    });

    svr.Get(R"(/api/lequ/player/([0-9a-f]+)/([^/]+))",
            [](const Request& req, Response& res) {
        const std::string id = req.matches[1];
        const std::string file = req.matches[2];
        if (file != "index.m3u8" &&
            !(file.size() > 3 && file.rfind("seg_", 0) == 0 &&
              file.substr(file.size() - 3) == ".ts")) {
            return send_json(res, {{"error", "文件名无效"}}, 400);
        }
        std::shared_ptr<PlayJob> job;
        {
            std::lock_guard<std::mutex> lk(g_play_mu);
            auto it = g_players.find(id);
            if (it != g_players.end()) job = it->second;
        }
        if (!job) return send_json(res, {{"error", "播放会话不存在或已结束"}}, 404);
        job->touched_at = std::time(nullptr);
        const std::string path = job->dir + "/" + file;
        std::error_code ec;
        if (!fs::exists(path, ec)) return send_json(res, {{"error", "分片尚未生成"}}, 404);
        res.set_header("Cache-Control", "no-store");
        res.set_header("X-Accel-Buffering", "no");
        res.set_file_content(path, file == "index.m3u8"
                                      ? "application/vnd.apple.mpegurl"
                                      : "video/mp2t");
    });

    // --- real platform follow state ---
    svr.Post("/api/lequ/follow", [](const Request& req, Response& res) {
        if (!rauth(req)) return unauth(res);
        if (current_auth().empty()) return send_json(res, {{"error", "未登录 LeQu"}}, 401);
        json b = body_json(req);
        std::string vid = b.value("vid", "");
        std::string name = b.value("name", "");
        bool follow = b.value("follow", true);
        // follow is keyed on the anchor (name); vid is optional (only present when
        // following from inside a live room). Allow following from the profile page.
        if (!valid_lequ_id(name)) return send_json(res, {{"error", "主播 name 无效"}}, 400);
        if (!vid.empty() && !valid_lequ_id(vid)) return send_json(res, {{"error", "vid 无效"}}, 400);

        const std::string path = follow ? "/social/follow" : "/social/unfollow";
        Resp r = lequ_post(path, current_auth(), "vid=" + vid + "&name=" + name);
        json upstream;
        if (!parse_ok(r, upstream, follow ? "follow" : "unfollow"))
            return send_lequ_upstream_error(res, follow ? "平台关注失败" : "取消关注失败");
        if (upstream.contains("data") && upstream["data"].is_boolean()
            && !upstream["data"].get<bool>())
            return send_json(res, {{"error", follow ? "平台未确认关注" : "平台未确认取消关注"}}, 502);

        send_json(res, {{"ok", true}, {"followed", follow}, {"name", name}, {"vid", vid}});
    });

    // --- room countdown gift (the 5-min free "小心心") ---
    // Captured: POST /app/live/room/countdown/gift  body vid=&sessionid=&sid=.
    // Response describes the countdown + the free gift to claim/send.
    svr.Get("/api/lequ/countdown_gift", [](const Request& req, Response& res) {
        if (!rauth(req)) return unauth(res);
        if (current_auth().empty()) return send_json(res, {{"error", "未登录 LeQu"}}, 401);
        std::string vid = req.has_param("vid") ? req.get_param_value("vid") : "";
        if (!valid_lequ_id(vid)) return send_json(res, {{"error", "直播 vid 无效"}}, 400);
        std::string sid = current_auth();
        std::string body = "vid=" + vid + "&sessionid=" + sid + "&sid=" + sid;
        Resp r = lequ_post("/app/live/room/countdown/gift", sid, body);
        json j;
        if (!parse_ok(r, j, "countdown/gift")) return send_lequ_upstream_error(res, "获取倒计时心心失败");
        send_json(res, j);
    });

    // --- send a gift (captured endpoint /biz/v2/buy/gift, Zeus-signed) ---
    // buyType 0 = pay from balance; pkId != 0 = send from the free/backpack bag
    // (e.g. the watch-time "小心心"). giftId of the free heart is not in captures,
    // so the caller supplies giftId/pkId explicitly. This spends currency when
    // buyType=0 — the UI must confirm first.
    svr.Post("/api/lequ/gift/send", [](const Request& req, Response& res) {
        if (!rauth(req)) return unauth(res);
        if (current_auth().empty()) return send_json(res, {{"error", "未登录 LeQu"}}, 401);
        json b = body_json(req);
        std::string vid  = b.value("vid", "");
        std::string name = b.value("name", "");        // anchor name
        long long giftId = b.value("giftId", 0LL);
        long long pkId   = b.value("pkId", 0LL);
        int giftNum      = b.value("giftNum", 1);
        int buyType      = b.value("buyType", 0);
        if (!valid_lequ_id(vid) || !valid_lequ_id(name))
            return send_json(res, {{"error", "vid 或主播 name 无效"}}, 400);
        if (giftId <= 0 || giftNum <= 0 || giftNum > 9999)
            return send_json(res, {{"error", "giftId / giftNum 无效"}}, 400);
        int code = 0; std::string msg;
        bool ok = send_gift(vid, name, giftId, giftNum, pkId, buyType, code, msg);
        if (!ok) {
            if (g_lequ_auth_expired.load()) return send_lequ_upstream_error(res, "送礼失败");
            return send_json(res, {{"error", msg.empty() ? "赠送失败" : msg}, {"code", code}}, 502);
        }
        send_json(res, {{"ok", true}, {"giftId", giftId}, {"giftNum", giftNum}});
    });

    // --- tracked anchors ---
    svr.Get("/api/lequ/tracked", [](const Request& req, Response& res) {
        if (!rauth(req)) return unauth(res);
        send_json(res, {{"tracked", tracked_state_json()}});
    });
    svr.Post("/api/lequ/track", [](const Request& req, Response& res) {
        if (!rauth(req)) return unauth(res);
        json b = body_json(req);
        std::string name = b.value("name", "");
        if (name.empty()) return send_json(res, {{"error", "缺少主播 name"}}, 400);
        std::lock_guard<std::mutex> lk(g_track_mu);
        for (auto& t : g_tracked) if (t.value("name", "") == name) return send_json(res, {{"ok", true}, {"dup", true}});
        g_tracked.push_back({{"name", name}, {"nickname", b.value("nickname", "")}});
        save_tracked_locked();
        send_json(res, {{"ok", true}});
    });
    svr.Post("/api/lequ/untrack", [](const Request& req, Response& res) {
        if (!rauth(req)) return unauth(res);
        json b = body_json(req);
        std::string name = b.value("name", "");
        std::lock_guard<std::mutex> lk(g_track_mu);
        json keep = json::array();
        for (auto& t : g_tracked) if (t.value("name", "") != name) keep.push_back(t);
        g_tracked = keep; save_tracked_locked();
        send_json(res, {{"ok", true}});
    });

    // --- manual record / stop ---
    svr.Post("/api/lequ/record", [](const Request& req, Response& res) {
        if (!rauth(req)) return unauth(res);
        if (current_auth().empty()) return send_json(res, {{"error", "未登录 LeQu"}}, 401);
        json b = body_json(req);
        std::string vid = b.value("vid", "");
        std::string name = b.value("name", "");
        std::string nick = b.value("nickname", "");
        if (vid.empty() && name.empty()) return send_json(res, {{"error", "需要 vid 或 name"}}, 400);
        if (vid.empty()) {   // resolve current vid from lists by name
            for (const char* kind : {"hot", "following", "recommended"}) {
                std::vector<Anchor> as;
                if (fetch_list(kind, as)) for (auto& a : as)
                    if (a.name == name && a.living) { vid = a.vid; if (nick.empty()) nick = a.nickname; break; }
                if (!vid.empty()) break;
            }
            if (vid.empty()) return send_json(res, {{"error", "主播当前不在直播列表中（可能未开播）"}}, 404);
        }
        bool started = start_recording(name, nick, vid, false);
        send_json(res, {{"ok", started}, {"vid", vid}, {"already", !started}});
    });
    svr.Post("/api/lequ/recording/stop", [](const Request& req, Response& res) {
        if (!rauth(req)) return unauth(res);
        json b = body_json(req);
        std::string vid = b.value("vid", "");
        std::lock_guard<std::mutex> lk(g_rec_mu);
        auto it = g_recs.find(vid);
        if (it == g_recs.end()) return send_json(res, {{"error", "没有该录制任务"}}, 404);
        it->second->cancel = true;
        send_json(res, {{"ok", true}});
    });

    // --- active recordings ---
    svr.Get("/api/lequ/recordings", [](const Request& req, Response& res) {
        if (!rauth(req)) return unauth(res);
        send_json(res, {{"active", recordings_state_json()},
                        {"history", recordings_history_json()}});
    });
    svr.Post("/api/lequ/recordings/scan", [](const Request& req, Response& res) {
        if (!rauth(req)) return unauth(res);
        int added = scan_recoverable_recordings();
        send_json(res, {{"ok", true}, {"added", added}, {"history", recordings_history_json()}});
    });
    svr.Post(R"(/api/lequ/recordings/([0-9a-f]+)/remux)",
             [](const Request& req, Response& res) {
        if (!rauth(req)) return unauth(res);
        std::string id = req.matches[1];
        bool found = false;
        {
            std::lock_guard<std::mutex> lk(g_rec_history_mu);
            for (auto& r : g_rec_history) {
                if (r.value("id", "") != id) continue;
                found = true;
                if (!r.value("recoverable", false))
                    return send_json(res, {{"error", "该记录不需要转封装"}}, 400);
                if (r.value("remuxing", false))
                    return send_json(res, {{"ok", true}, {"remuxing", true}});
                auto sources = recording_sources(r);
                if (sources.empty())
                    return send_json(res, {{"error", "源文件不存在"}}, 404);
                for (const auto& source : sources) {
                    std::error_code ec;
                    if (!fs::exists(source, ec))
                        return send_json(res, {{"error", "部分源文件已经不存在"}}, 404);
                    if (!source_is_stale(source, std::time(nullptr)))
                        return send_json(res, {{"error", "文件仍在写入，请稍后再试"}}, 409);
                }
                r["remuxing"] = true;
                r["state"] = "remuxing";
                r.erase("remux_error");
                save_recordings_locked();
                break;
            }
        }
        if (!found) return send_json(res, {{"error", "录制记录不存在"}}, 404);
        spawn_guarded("recovery-remux", [id] { remux_recovered_recording(id); });
        send_json(res, {{"ok", true}, {"remuxing", true}});
    });
    svr.Get(R"(/api/lequ/recordings/([0-9a-f]+)/download)",
            [](const Request& req, Response& res) {
        if (!rauth(req)) return unauth(res);
        std::string id = req.matches[1], path;
        {
            std::lock_guard<std::mutex> lk(g_rec_history_mu);
            for (const auto& r : g_rec_history)
                if (r.value("id", "") == id) { path = r.value("path", ""); break; }
        }
        std::error_code ec;
        if (path.empty() || !fs::exists(path, ec))
            return send_json(res, {{"error", "录制文件不存在"}}, 404);
        std::string filename = fs::path(path).filename().string();
        res.set_header("Content-Disposition",
            "attachment; filename=\"lequ-recording" + fs::path(path).extension().string() +
            "\"; filename*=UTF-8''" + url_encode(filename));
        res.set_header("Cache-Control", "private, no-store");
        res.set_file_content(path, fs::path(path).extension() == ".mp4"
                                   ? "video/mp4" : "video/x-flv");
    });
    svr.Get(R"(/api/lequ/recordings/([0-9a-f]+)/play)",
            [](const Request& req, Response& res) {
        if (!rauth(req)) return unauth(res);
        std::string id = req.matches[1], path;
        {
            std::lock_guard<std::mutex> lk(g_rec_history_mu);
            for (const auto& r : g_rec_history)
                if (r.value("id", "") == id) { path = r.value("path", ""); break; }
        }
        std::error_code ec;
        if (path.empty() || !fs::exists(path, ec) || fs::path(path).extension() != ".mp4")
            return send_json(res, {{"error", "MP4 文件不存在"}}, 404);
        res.set_header("Cache-Control", "private, no-store");
        res.set_header("Accept-Ranges", "bytes");
        res.set_file_content(path, "video/mp4");
    });
    svr.Get(R"(/api/lequ/recordings/([0-9a-f]+)/thumb)",
            [](const Request& req, Response& res) {
        if (!rauth(req)) return unauth(res);
        std::string id = req.matches[1], path;
        {
            std::lock_guard<std::mutex> lk(g_rec_history_mu);
            for (const auto& r : g_rec_history)
                if (r.value("id", "") == id) { path = r.value("thumb_path", ""); break; }
        }
        std::error_code ec;
        if (path.empty() || !fs::exists(path, ec))
            return send_json(res, {{"error", "缩略图不存在"}}, 404);
        res.set_header("Cache-Control", "private, max-age=3600");
        res.set_file_content(path, "image/jpeg");
    });
    svr.Get(R"(/api/lequ/recordings/([0-9a-f]+)/preview)",
            [](const Request& req, Response& res) {
        if (!rauth(req)) return unauth(res);
        std::string id = req.matches[1], thumb;
        {
            std::lock_guard<std::mutex> lk(g_rec_history_mu);
            for (const auto& r : g_rec_history)
                if (r.value("id", "") == id) { thumb = r.value("thumb_path", ""); break; }
        }
        std::string path = thumb.empty() ? "" : contact_preview_path(thumb);
        std::error_code ec;
        if (path.empty() || !fs::exists(path, ec))
            return send_json(res, {{"error", "预览图不存在"}}, 404);
        res.set_header("Cache-Control", "private, max-age=86400");
        res.set_file_content(path, "image/jpeg");
    });
    svr.Post("/api/lequ/recordings/contactsheet/batch",
             [](const Request& req, Response& res) {
        if (!rauth(req)) return unauth(res);
        int queued = queue_missing_contact_sheets(0);
        send_json(res, {{"ok", true}, {"queued", queued}, {"history", recordings_history_json()}});
    });
    svr.Post(R"(/api/lequ/recordings/([0-9a-f]+)/contactsheet)",
             [](const Request& req, Response& res) {
        if (!rauth(req)) return unauth(res);
        std::string id = req.matches[1], path;
        {
            std::lock_guard<std::mutex> lk(g_rec_history_mu);
            for (const auto& r : g_rec_history)
                if (r.value("id", "") == id) { path = r.value("path", ""); break; }
        }
        std::error_code ec;
        if (path.empty() || !fs::exists(path, ec))
            return send_json(res, {{"error", "录制文件不存在"}}, 404);
        bool queued = enqueue_contact_sheet(id, path, true);
        send_json(res, {{"ok", true}, {"generating", true}, {"queued", queued}});
    });

    // --- daily tasks (watch-time → diamonds), captured endpoints ---
    svr.Get("/api/lequ/tasks", [](const Request& req, Response& res) {
        if (!rauth(req)) return unauth(res);
        if (current_auth().empty()) return send_json(res, {{"error", "未登录 LeQu"}}, 401);
        Resp r = lequ_post("/app/general/personal/task/info", current_auth(), "");
        json j;
        if (!parse_ok(r, j, "task/info")) return send_lequ_upstream_error(res, "获取任务失败");
        send_json(res, j);
    });
    svr.Post("/api/lequ/tasks/claim", [](const Request& req, Response& res) {
        if (!rauth(req)) return unauth(res);
        if (current_auth().empty()) return send_json(res, {{"error", "未登录 LeQu"}}, 401);
        long long taskId = body_json(req).value("taskId", 0LL);
        if (taskId <= 0) return send_json(res, {{"error", "taskId 无效"}}, 400);
        Resp r = lequ_post("/app/general/personal/task/fulfill", current_auth(),
                           "taskId=" + std::to_string(taskId));
        json j;
        if (!parse_ok(r, j, "task/fulfill")) return send_lequ_upstream_error(res, "领取失败");
        send_json(res, {{"ok", true}, {"taskId", taskId}, {"result", j}});
    });

    // --- 么么哒 farm (24/7 auto-watch + send) ---
    svr.Get("/api/lequ/settings", [](const Request& req, Response& res) {
        if (!rauth(req)) return unauth(res);
        send_json(res, settings_json());
    });
    svr.Post("/api/lequ/settings", [](const Request& req, Response& res) {
        if (!rauth(req)) return unauth(res);
        json b = body_json(req);
        {
            std::lock_guard<std::mutex> lk(g_settings_mu);
            if (b.contains("watch_priority"))
                g_settings.watch_priority = normalize_watch_priority(b.value("watch_priority", "farm"));
            save_settings_locked();
        }
        send_json(res, settings_json());
    });

    svr.Get("/api/lequ/farm/status", [](const Request& req, Response& res) {
        if (!rauth(req)) return unauth(res);
        send_json(res, farm_status_json());
    });
    svr.Post("/api/lequ/farm/config", [](const Request& req, Response& res) {
        if (!rauth(req)) return unauth(res);
        json b = body_json(req);
        {
            std::lock_guard<std::mutex> lk(g_farm.mu);
            if (b.contains("concurrency"))
                g_farm.concurrency = std::max(0, std::min(8, b.value("concurrency", 1)));
            if (b.contains("mode")) {
                std::string m = b.value("mode", "random");
                g_farm.mode = (m == "specific") ? "specific" : "random";
            }
            if (b.contains("targets") && b["targets"].is_array()) {
                std::vector<std::string> t;
                std::set<std::string> seen;
                for (auto& x : b["targets"]) {
                    std::string s = x.is_string() ? x.get<std::string>() : "";
                    if (valid_lequ_id(s) && seen.insert(s).second && t.size() < 8)
                        t.push_back(s);
                }
                g_farm.targets = t;
            }
            if (b.contains("enabled")) g_farm.enabled = b.value("enabled", false);
            save_farm_locked();
        }
        farm_reconcile();
        send_json(res, farm_status_json());
    });
    svr.Post(R"(/api/lequ/farm/worker/([0-9a-f]+))",
             [](const Request& req, Response& res) {
        if (!rauth(req)) return unauth(res);
        std::string id = req.matches[1];
        std::string action = body_json(req).value("action", "");
        bool found = false;
        {
            std::lock_guard<std::mutex> lk(g_farm.mu);
            auto it = std::find_if(g_farm.workers.begin(), g_farm.workers.end(),
                [&](const auto& w){ return w->id == id; });
            if (it != g_farm.workers.end()) {
                found = true;
                auto w = *it;
                if (action == "pause") {
                    w->paused = true;
                } else if (action == "resume") {
                    w->paused = false;
                } else if (action == "cancel") {
                    w->stop = true;
                    if (w->target.empty()) {
                        g_farm.concurrency = std::max(0, g_farm.concurrency.load() - 1);
                    } else {
                        g_farm.targets.erase(std::remove(g_farm.targets.begin(),
                            g_farm.targets.end(), w->target), g_farm.targets.end());
                    }
                    g_farm.workers.erase(it);
                    save_farm_locked();
                } else {
                    return send_json(res, {{"error", "action 必须是 pause、resume 或 cancel"}}, 400);
                }
            }
        }
        if (!found) return send_json(res, {{"error", "worker 不存在"}}, 404);
        if (action == "cancel") farm_reconcile();
        send_json(res, farm_status_json());
    });

    svr.Get("/api/lequ/debug/logs", [](const Request& req, Response& res) {
        if (!rauth(req)) return unauth(res);
        unsigned long long after = 0;
        if (req.has_param("after")) {
            try { after = std::stoull(req.get_param_value("after")); } catch (...) {}
        }
        json arr = json::array();
        for (auto& item : debug_logs_since(after)) arr.push_back(std::move(item));
        send_json(res, {{"logs", arr}, {"latest_seq", g_debug_seq.load()}});
    });

    svr.Get("/api/lequ/state", [](const Request& req, Response& res) {
        if (!rauth(req)) return unauth(res);
        json state{{"tracked", tracked_state_json()}, {"active", recordings_state_json()}};
        if (req.has_param("history") && req.get_param_value("history") == "1")
            state["history"] = recordings_history_json();
        send_json(res, state);
    });

    svr.Get("/api/lequ/state/stream", [](const Request& req, Response& res) {
        if (!rauth(req)) return unauth(res);
        res.set_header("Cache-Control", "no-cache");
        res.set_header("X-Accel-Buffering", "no");
        res.set_chunked_content_provider(
            "text/event-stream",
            [](size_t, httplib::DataSink& sink) -> bool {
                std::string previous;
                auto last_ping = std::chrono::steady_clock::now() - std::chrono::seconds(15);
                while (sink.is_writable()) {
                    std::string current;
                    try { current = lequ_state_json().dump(); }
                    catch (const std::exception& e) {   // never terminate the process from an SSE provider
                        std::cerr << "[web] state/stream error: " << e.what() << "\n"; return false;
                    }
                    if (current != previous) {
                        std::string event = "event: state\ndata: " + current + "\n\n";
                        if (!sink.write(event.data(), event.size())) return false;
                        previous = std::move(current);
                    }
                    auto now = std::chrono::steady_clock::now();
                    if (now - last_ping >= std::chrono::seconds(15)) {
                        static const char ping[] = ": ping\n\n";
                        if (!sink.write(ping, sizeof(ping) - 1)) return false;
                        last_ping = now;
                    }
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                }
                sink.done();
                return true;
            });
    });

    svr.Get("/api/lequ/debug/stream", [](const Request& req, Response& res) {
        if (!rauth(req)) return unauth(res);
        unsigned long long after = 0;
        if (req.has_param("after")) {
            try { after = std::stoull(req.get_param_value("after")); } catch (...) {}
        }
        res.set_header("Cache-Control", "no-cache");
        res.set_header("X-Accel-Buffering", "no");
        res.set_chunked_content_provider(
            "text/event-stream",
            [after](size_t, httplib::DataSink& sink) mutable -> bool {
                auto last_ping = std::chrono::steady_clock::now() - std::chrono::seconds(15);
                while (sink.is_writable()) {
                    for (const auto& item : debug_logs_since(after)) {
                        after = std::max(after, item.value("seq", 0ULL));
                        std::string event = "event: debug\ndata: " + item.dump() + "\n\n";
                        if (!sink.write(event.data(), event.size())) return false;
                    }
                    auto now = std::chrono::steady_clock::now();
                    if (now - last_ping >= std::chrono::seconds(15)) {
                        static const char ping[] = ": ping\n\n";
                        if (!sink.write(ping, sizeof(ping) - 1)) return false;
                        last_ping = now;
                    }
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                }
                sink.done();
                return true;
            });
    });
}

void lequ_start_scheduler() {
    if (g_sched_running.exchange(true)) return;
    start_contact_sheet_workers();
    if (!g_contact_auto_running.exchange(true))
        spawn_guarded("contact-auto", [] { contact_sheet_auto_loop(); });
    spawn_guarded("scheduler", [] { scheduler_loop(); });
    if (!g_recovery_scanner_running.exchange(true))
        spawn_guarded("recovery-scanner", [] { recovery_scanner_loop(); });
    std::cout << "[LeQu] auto-track scheduler started (poll " << poll_secs() << "s)\n";
    if (g_farm.enabled.load()) {   // resume a 24/7 farm across restarts
        farm_reconcile();
        std::cout << "[LeQu] 么么哒 farm resumed (concurrency "
                  << g_farm.concurrency.load() << ", mode " << g_farm.mode << ")\n";
    }
}
