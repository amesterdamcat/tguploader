#include "caption.h"
#include "utils.h"
#include <algorithm>
#include <regex>
#include <filesystem>
#include <sys/stat.h>

namespace fs = std::filesystem;

static std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> parts;
    std::string token;
    for (char c : s) {
        if (c == delim) {
            parts.push_back(token);
            token.clear();
        } else {
            token += c;
        }
    }
    parts.push_back(token);
    return parts;
}

std::string extract_hashtags(const std::string& file_path) {
    try {
        fs::path p(file_path);
        std::string stem = p.stem().string();
        auto parts = split(stem, '_');

        if (parts.size() < 2) return "";

        static const std::vector<std::string> platform_keywords = {
            "Chaturbate", "StripChat", "OnlyFans", "ManyVids",
            "Cam4", "Streamate", "LiveJasmin"
        };

        int platform_index = -1;
        for (int i = 0; i < static_cast<int>(parts.size()); i++) {
            std::string lower_part = parts[i];
            std::transform(lower_part.begin(), lower_part.end(), lower_part.begin(), ::tolower);
            for (const auto& kw : platform_keywords) {
                std::string lower_kw = kw;
                std::transform(lower_kw.begin(), lower_kw.end(), lower_kw.begin(), ::tolower);
                if (lower_part == lower_kw) {
                    platform_index = i;
                    break;
                }
            }
            if (platform_index >= 0) break;
        }

        std::string model_name;
        std::vector<std::string> search_parts;

        if (platform_index > 0) {
            // Join parts before the platform keyword
            for (int i = 0; i < platform_index; i++) {
                if (i > 0) model_name += '_';
                model_name += parts[i];
            }
            for (int i = platform_index + 1; i < static_cast<int>(parts.size()); i++) {
                search_parts.push_back(parts[i]);
            }
        } else {
            model_name = parts[0];
            for (int i = 1; i < static_cast<int>(parts.size()); i++) {
                search_parts.push_back(parts[i]);
            }
        }

        // Search for date pattern YYYYMMDD or YYYY-MM-DD
        std::regex date_re(R"((\d{4})[-]?(\d{2})[-]?(\d{2}))");
        for (const auto& part : search_parts) {
            std::smatch m;
            if (std::regex_search(part, m, date_re)) {
                return "#" + model_name + " #date" + m[1].str() + m[2].str() + m[3].str();
            }
        }

        return model_name.empty() ? "" : "#" + model_name;

    } catch (...) {
        // Fallback: use parent directory name
        try {
            fs::path p(file_path);
            if (p.has_parent_path()) {
                std::string parent = p.parent_path().filename().string();
                if (!parent.empty() && parent[0] != '.') {
                    return "#" + parent;
                }
            }
        } catch (...) {}
        return "";
    }
}

std::string make_video_caption(const std::string& file_path, const VideoInfo& info) {
    std::string file_name = fs::path(file_path).filename().string();
    std::string hashtag = extract_hashtags(file_path);

    int64_t file_size = 0;
    struct stat st;
    if (::stat(file_path.c_str(), &st) == 0) {
        file_size = st.st_size;
    }

    std::string size_str = format_file_size(file_size);
    std::string dur_str = format_duration(info.duration);

    int w = info.width > 0 ? info.width : 1920;
    int h = info.height > 0 ? info.height : 1080;

    return "```\n" + file_name + "\n"
         + "Resolution: " + std::to_string(w) + "x" + std::to_string(h) + "\n"
         + "Duration: " + dur_str + "\n"
         + "Size: " + size_str + "\n"
         + "```\n"
         + hashtag;
}

std::string make_image_caption(const std::string& file_path, int width, int height) {
    std::string file_name = fs::path(file_path).filename().string();
    std::string hashtag = extract_hashtags(file_path);

    int64_t file_size = 0;
    struct stat st;
    if (::stat(file_path.c_str(), &st) == 0) {
        file_size = st.st_size;
    }

    std::string size_str = format_file_size(file_size);

    std::string result = "```\n" + file_name + "\n";
    if (width > 0 && height > 0) {
        result += "Resolution: " + std::to_string(width) + "x" + std::to_string(height) + "\n";
    }
    result += "Size: " + size_str + "\n"
           + "```\n"
           + hashtag;
    return result;
}
