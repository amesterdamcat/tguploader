#pragma once
#include <string>

struct VideoInfo {
    int width = 0;
    int height = 0;
    double duration = 0.0;
};

VideoInfo get_video_info(const std::string& path);
