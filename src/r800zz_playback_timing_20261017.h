#pragma once

#include <chrono>
#include <cstdint>
#include <thread>

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/rational.h>
}

namespace r800zz {

// Shared realtime pacing for CUDA, DirectML, and CPU pipelines.
inline bool paceOrDropFrame(
        bool offlineConvert,
        int64_t pts,
        int64_t& firstVideoPts,
        bool playbackClockStarted,
        AVRational videoTimeBase,
        std::chrono::steady_clock::time_point wallStart,
        uint64_t& dropped) {
    constexpr double kDropLagSeconds = 0.120;
    if (offlineConvert || pts == AV_NOPTS_VALUE) return false;
    if (firstVideoPts == AV_NOPTS_VALUE) {
        firstVideoPts = pts;
        return false;
    }
    // The initial RVM warm-up is excluded from playback elapsed time.
    if (!playbackClockStarted) return false;
    const double src = static_cast<double>(pts - firstVideoPts) * av_q2d(videoTimeBase);
    const auto now = std::chrono::steady_clock::now();
    const double wall = std::chrono::duration<double>(now - wallStart).count();
    const double delta = src - wall;
    if (delta > 0.002) {
        std::this_thread::sleep_for(std::chrono::duration<double>(delta));
        return false;
    }
    if (delta < -kDropLagSeconds) {
        ++dropped;
        return true;
    }
    return false;
}

} // namespace r800zz
