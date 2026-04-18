#pragma once
/*
 * ytcui-dl — ytfast_types.h
 * Core data structures.
 */

#include <string>
#include <vector>
#include <cstdint>

namespace ytfast {

struct StreamFormat {
    int         itag             = 0;
    std::string url;         // for downloading: has &range=0-N appended for DASH streams
    std::string stream_url;  // for streaming (mpv): raw URL without range, use for playback
    std::string mime_type;
    std::string quality;
    int         width            = 0;
    int         height           = 0;
    int64_t     bitrate          = 0;
    int64_t     content_length   = 0;
    bool        has_video        = false;
    bool        has_audio        = false;
    int         audio_sample_rate= 0;
    int         audio_channels   = 0;
    std::string audio_codec;      // e.g. "mp4a.40.2", "opus"
    std::string video_codec;      // e.g. "avc1.4d401f", "vp9"
};

struct VideoInfo {
    std::string id;
    std::string title;
    std::string channel;
    std::string channel_id;
    std::string description;
    std::string thumbnail_url;
    std::string upload_date;
    std::string duration_str;
    int         duration_secs    = 0;
    int64_t     view_count       = 0;
    std::string view_count_str;
    bool        is_live          = false;
    std::string url;
    std::vector<StreamFormat> formats;
};

using SearchResult = VideoInfo;

} // namespace ytfast
