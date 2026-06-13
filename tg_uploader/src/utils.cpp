#include "utils.h"
#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace fs = std::filesystem;

std::string format_file_size(int64_t bytes) {
    char buf[64];
    if (bytes >= 1024LL * 1024 * 1024) {
        std::snprintf(buf, sizeof(buf), "%.2f GB",
                      static_cast<double>(bytes) / (1024.0 * 1024 * 1024));
    } else if (bytes >= 1024LL * 1024) {
        std::snprintf(buf, sizeof(buf), "%.2f MB",
                      static_cast<double>(bytes) / (1024.0 * 1024));
    } else {
        std::snprintf(buf, sizeof(buf), "%.2f KB",
                      static_cast<double>(bytes) / 1024.0);
    }
    return buf;
}

std::string format_duration(double seconds) {
    if (seconds <= 0) return "unknown";
    int total = static_cast<int>(seconds);
    int h = total / 3600;
    int m = (total % 3600) / 60;
    int s = total % 60;
    char buf[16];
    if (h > 0) {
        std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", h, m, s);
    } else {
        std::snprintf(buf, sizeof(buf), "%02d:%02d", m, s);
    }
    return buf;
}

// Parse JPEG SOF markers to get image dimensions
ImageSize get_jpeg_size(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return {};

    uint8_t buf[2];
    // Check JPEG SOI marker
    if (std::fread(buf, 1, 2, f) != 2 || buf[0] != 0xFF || buf[1] != 0xD8) {
        std::fclose(f);
        return {};
    }

    while (true) {
        if (std::fread(buf, 1, 2, f) != 2 || buf[0] != 0xFF) break;
        uint8_t marker = buf[1];
        if (marker == 0xD9 || marker == 0xDA) break; // EOI / SOS

        // SOF markers (Start of Frame): C0..CF except C4, C8, CC
        if (marker >= 0xC0 && marker <= 0xCF &&
            marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
            uint8_t seg[7];
            if (std::fread(seg, 1, 7, f) == 7) {
                int h = (seg[3] << 8) | seg[4];
                int w = (seg[5] << 8) | seg[6];
                std::fclose(f);
                return {w, h};
            }
            break;
        }
        // Skip segment
        if (std::fread(buf, 1, 2, f) != 2) break;
        int len = (buf[0] << 8) | buf[1];
        std::fseek(f, len - 2, SEEK_CUR);
    }
    std::fclose(f);
    return {};
}

std::string shell_escape(const std::string& s) {
    // Wrap in single quotes; escape any embedded single quotes as '\''
    std::string result = "'";
    for (char c : s) {
        if (c == '\'') {
            result += "'\\''";
        } else {
            result += c;
        }
    }
    result += "'";
    return result;
}

bool is_video_file(const std::string& path) {
    auto dot = path.rfind('.');
    if (dot == std::string::npos) return false;
    std::string ext = path.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext == ".mp4" || ext == ".avi" || ext == ".mov" || ext == ".mkv" ||
           ext == ".webm" || ext == ".m4v" || ext == ".flv" || ext == ".wmv";
}

std::string fmt_elapsed(double secs) {
    int s = static_cast<int>(secs);
    if (s < 60)   return std::to_string(s) + "s";
    if (s < 3600) return std::to_string(s / 60) + "m " + std::to_string(s % 60) + "s";
    return std::to_string(s / 3600) + "h " + std::to_string((s % 3600) / 60) + "m " + std::to_string(s % 60) + "s";
}

std::string create_cover_thumb(const std::string& orig_jpg, const std::string& log_prefix) {
    if (orig_jpg.empty()) return "";
    std::string basename = fs::path(orig_jpg).filename().string();
    std::string tmp = "/tmp/tg_cover_"
                      + std::to_string(::getpid()) + "_"
                      + std::to_string(std::time(nullptr)) + "_"
                      + basename;
    std::string cmd = "ffmpeg -y -loglevel error"
                      " -i " + shell_escape(orig_jpg) +
                      " -vf \"scale=320:320:force_original_aspect_ratio=decrease\""
                      " -q:v 5"
                      " " + shell_escape(tmp) + " 2>/dev/null";
    if (std::system(cmd.c_str()) == 0 && fs::exists(tmp)) {
        auto sz = get_jpeg_size(tmp);
        std::cout << "[INFO] " << log_prefix << "Cover thumb: " << sz.width << "x" << sz.height
                  << " (" << format_file_size(static_cast<int64_t>(fs::file_size(tmp))) << ")"
                  << " → " << tmp << "\n";
        return tmp;
    }
    std::cerr << "\033[33m[WARNING]\033[0m Failed to resize thumbnail, skipping cover\n";
    return "";
}

// Cancellable wrapper around system(): fork+exec via /bin/sh, poll a flag,
// SIGTERM then SIGKILL the whole process group on cancel.
int cancellable_system(const std::string& cmd, std::atomic<bool>& cancel_flag) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        // child — new process group so we can signal ffmpeg + any helpers
        setpgid(0, 0);
        execl("/bin/sh", "sh", "-c", cmd.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    // parent — poll
    bool killed = false;
    bool term_sent = false;
    auto term_time = std::chrono::steady_clock::time_point::min();
    while (true) {
        int status = 0;
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            if (killed) return -1;
            if (WIFEXITED(status))    return WEXITSTATUS(status);
            if (WIFSIGNALED(status))  return -1;
            return -1;
        }
        if (r < 0) return -1;

        if (cancel_flag.load() && !term_sent) {
            // SIGTERM whole process group to also catch ffmpeg's children
            ::kill(-pid, SIGTERM);
            term_sent = true;
            killed = true;
            term_time = std::chrono::steady_clock::now();
        } else if (term_sent &&
                   std::chrono::steady_clock::now() - term_time > std::chrono::seconds(2)) {
            // escalate
            ::kill(-pid, SIGKILL);
            term_sent = false;   // don't re-send forever
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}
