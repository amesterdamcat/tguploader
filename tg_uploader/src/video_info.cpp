#include "video_info.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

VideoInfo get_video_info(const std::string& path) {
    VideoInfo info;
    AVFormatContext* fmt_ctx = nullptr;

    if (avformat_open_input(&fmt_ctx, path.c_str(), nullptr, nullptr) < 0)
        return info;

    if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
        avformat_close_input(&fmt_ctx);
        return info;
    }

    for (unsigned i = 0; i < fmt_ctx->nb_streams; i++) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            auto* par = fmt_ctx->streams[i]->codecpar;
            info.width = par->width;
            info.height = par->height;

            // Handle SAR (sample aspect ratio)
            if (par->sample_aspect_ratio.num > 0 && par->sample_aspect_ratio.den > 0) {
                double sar = static_cast<double>(par->sample_aspect_ratio.num) /
                             par->sample_aspect_ratio.den;
                if (sar != 1.0) {
                    info.width = static_cast<int>(info.width * sar);
                }
            }

            // Duration from stream
            if (fmt_ctx->streams[i]->duration > 0) {
                auto tb = fmt_ctx->streams[i]->time_base;
                info.duration = static_cast<double>(fmt_ctx->streams[i]->duration) *
                                tb.num / tb.den;
            }
            break;
        }
    }

    // Fallback: container-level duration
    if (info.duration <= 0 && fmt_ctx->duration > 0) {
        info.duration = static_cast<double>(fmt_ctx->duration) / AV_TIME_BASE;
    }

    avformat_close_input(&fmt_ctx);
    return info;
}
