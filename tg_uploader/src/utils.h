#pragma once
#include <cstdint>
#include <string>

std::string format_file_size(int64_t bytes);
std::string format_duration(double seconds);
bool is_video_file(const std::string& path);

struct ImageSize { int width = 0; int height = 0; };
ImageSize get_jpeg_size(const std::string& path);

// Escape a string for safe use in shell commands (wraps in single quotes)
std::string shell_escape(const std::string& s);

// Resize a JPEG thumbnail to fit within 320×320 using ffmpeg.
// Returns path to a temp file (caller must delete), or "" on failure.
// Telegram requires video thumbnails to be ≤320×320px.
std::string create_cover_thumb(const std::string& orig_jpg, const std::string& log_prefix = "");

// Format elapsed seconds as "5s", "1m 23s", "2h 5m 17s"
std::string fmt_elapsed(double secs);
