#pragma once
#include <string>
#include "video_info.h"

// Extract "#model_name #dateYYYYMMDD" from filename
std::string extract_hashtags(const std::string& file_path);

// Generate video caption (markdown with code block)
std::string make_video_caption(const std::string& file_path, const VideoInfo& info);

// Generate image caption (markdown with code block), width/height=0 means unknown
std::string make_image_caption(const std::string& file_path, int width = 0, int height = 0);
