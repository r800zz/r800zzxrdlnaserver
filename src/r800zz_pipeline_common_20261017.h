#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}

namespace r800zz {

inline std::string ffmpegError(int code) {
    char message[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, message, sizeof(message));
    return message;
}

// Shared input opening, stream selection, metadata, and initial seek.
// The caller owns the input format context and closes it during cleanup.
template<class Log>
bool openVideoInput(
        const std::filesystem::path& input, int64_t startMs,
        AVFormatContext*& format,
        AVStream*& video, AVStream*& audio,
        int& videoIndex, int& audioIndex,
        int& width, int& height, AVRational& frameRate,
        int64_t& requestedStartTimestampUs,
        std::string& error, Log&& log) {
    const std::string path = input.u8string();
    int rc = avformat_open_input(&format, path.c_str(), nullptr, nullptr);
    if (rc < 0) { error = "avformat_open_input: " + ffmpegError(rc); return false; }
    rc = avformat_find_stream_info(format, nullptr);
    if (rc < 0) { error = "avformat_find_stream_info: " + ffmpegError(rc); return false; }

    videoIndex = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (videoIndex < 0) { error = "No video stream"; return false; }
    video = format->streams[videoIndex];
    width = video->codecpar->width;
    height = video->codecpar->height;
    frameRate = av_guess_frame_rate(format, video, nullptr);
    if (frameRate.num <= 0 || frameRate.den <= 0) frameRate = AVRational{30, 1};

    audioIndex = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, videoIndex, nullptr, 0);
    if (audioIndex >= 0) audio = format->streams[audioIndex];

    int64_t durationMs = 0;
    if (format->duration != AV_NOPTS_VALUE && format->duration > 0)
        durationMs = av_rescale_q(format->duration, AV_TIME_BASE_Q, AVRational{1, 1000});
    log("META durationMs=" + std::to_string(durationMs));

    if (startMs > 0) {
        const int64_t baseUs = format->start_time != AV_NOPTS_VALUE ? format->start_time : 0;
        requestedStartTimestampUs = baseUs + startMs * 1000LL;
        rc = avformat_seek_file(format, -1, INT64_MIN, requestedStartTimestampUs,
                                INT64_MAX, AVSEEK_FLAG_BACKWARD);
        if (rc < 0) {
            error = "avformat_seek_file startMs=" + std::to_string(startMs) + ": " + ffmpegError(rc);
            return false;
        }
        log("input seek requested startMs=" + std::to_string(startMs));
    }
    return true;
}

// One demux/dispatch loop for every inference backend and output format.
// Decode, audio handling and GPU processing remain caller operations.
template<class Log, class Decode, class Audio>
bool readVideoPackets(
        AVFormatContext* format, int videoIndex, int audioIndex,
        bool previewOnly, bool hasOutputAudio, std::atomic<bool>* cancelFlag,
        uint64_t& videoPacketsRead, std::string& error,
        Log&& log, Decode&& decode, Audio&& audio) {
    AVPacket* packet = av_packet_alloc();
    if (!packet) { error = "av_packet_alloc failed"; return false; }
    bool success = true;
    for (;;) {
        if (cancelFlag && cancelFlag->load()) {
            log("stream cancellation requested");
            break;
        }
        const int rc = av_read_frame(format, packet);
        if (rc == AVERROR_EOF) break;
        if (rc < 0) {
            error = "av_read_frame: " + ffmpegError(rc);
            success = false;
            break;
        }
        if (packet->stream_index == videoIndex) {
            ++videoPacketsRead;
            if (videoPacketsRead == 1) {
                log("first-frame stage=demux_video_packet bytes=" + std::to_string(packet->size) +
                    " pts=" + std::to_string(packet->pts) + " dts=" + std::to_string(packet->dts));
            }
            success = decode(packet, error);
        } else if (!previewOnly && audioIndex >= 0 &&
                   packet->stream_index == audioIndex && hasOutputAudio) {
            success = audio(packet, error);
        }
        av_packet_unref(packet);
        if (!success) break;
    }
    av_packet_free(&packet);
    return success;
}

} // namespace r800zz
