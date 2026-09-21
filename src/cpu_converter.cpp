#include "cpu_converter.h"

#include "rvm_ort.h"

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <sstream>
#include <thread>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace {

std::string fferr(int code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, buffer, sizeof(buffer));
    return buffer;
}

void logCpu(const std::string& message) {
    std::cerr << "CPP_CPU: " << message << "\n";
    std::cerr.flush();
}

std::string pathUtf8(const std::filesystem::path& path) {
    return path.u8string();
}

float clamp01(float value) {
    return std::min(1.0f, std::max(0.0f, value));
}

class CpuConverter {
public:
    explicit CpuConverter(const CpuConversionOptions& options) : options_(options) {}
    ~CpuConverter() { cleanup(); }

    bool run(std::string& error) {
        if (!initInput(error) || !initOutput(error) || !initRvm(error)) {
            return false;
        }

        logCpu("pipeline active: FFmpeg software decode -> ONNX Runtime CPU EP "
               "RVM FP32 -> CPU compose/pack -> FFmpeg library encode");
        std::unique_ptr<AVPacket, PacketDeleter> packet(av_packet_alloc());
        if (!packet) {
            error = "av_packet_alloc failed";
            return false;
        }

        for (;;) {
            const int rc = av_read_frame(inputFormat_, packet.get());
            if (rc == AVERROR_EOF) break;
            if (rc < 0) {
                error = "av_read_frame: " + fferr(rc);
                return false;
            }
            if (packet->stream_index == videoIndex_) {
                if (!sendVideoPacket(packet.get(), error)) return false;
            } else if (packet->stream_index == audioIndex_ && outputAudio_) {
                if (!sendAudioPacket(packet.get(), error)) return false;
            }
            av_packet_unref(packet.get());
        }

        if (!flushVideo(error) || !flushAudio(error)) return false;
        const int trailer = av_write_trailer(outputFormat_);
        if (trailer < 0) {
            error = "av_write_trailer: " + fferr(trailer);
            return false;
        }
        logCpu("completed frames=" + std::to_string(processedFrames_));
        logProgress(true);
        return true;
    }

private:
    struct PacketDeleter {
        void operator()(AVPacket* packet) const {
            if (packet) av_packet_free(&packet);
        }
    };
    struct FrameDeleter {
        void operator()(AVFrame* frame) const {
            if (frame) av_frame_free(&frame);
        }
    };

    CpuConversionOptions options_;
    AVFormatContext* inputFormat_ = nullptr;
    AVFormatContext* outputFormat_ = nullptr;
    AVCodecContext* videoDecoder_ = nullptr;
    AVCodecContext* videoEncoder_ = nullptr;
    AVCodecContext* audioDecoder_ = nullptr;
    AVCodecContext* audioEncoder_ = nullptr;
    AVStream* inputVideo_ = nullptr;
    AVStream* inputAudio_ = nullptr;
    AVStream* outputVideo_ = nullptr;
    AVStream* outputAudio_ = nullptr;
    SwsContext* toRgb_ = nullptr;
    SwsContext* fromRgb_ = nullptr;
    SwrContext* audioResampler_ = nullptr;
    AVAudioFifo* audioFifo_ = nullptr;
    AVFrame* encodedVideoFrame_ = nullptr;
    int videoIndex_ = -1;
    int audioIndex_ = -1;
    int width_ = 0;
    int height_ = 0;
    AVRational frameRate_{30, 1};
    bool audioCopy_ = false;
    int64_t audioNextPts_ = AV_NOPTS_VALUE;
    int64_t lastVideoPts_ = AV_NOPTS_VALUE;
    uint64_t processedFrames_ = 0;
    std::chrono::steady_clock::time_point lastProgressLog_{};
    RvmOrtCpu rvm_;
    std::vector<uint8_t> sourceRgb_;
    std::vector<uint8_t> processedRgb_;
    std::vector<float> rvmInput_;

    void logProgress(bool force = false) {
        const auto now = std::chrono::steady_clock::now();
        if (!force && lastProgressLog_.time_since_epoch().count() != 0 &&
            now - lastProgressLog_ < std::chrono::milliseconds(200)) {
            return;
        }
        logCpu("PROGRESS frames=" + std::to_string(processedFrames_));
        lastProgressLog_ = now;
    }

    void cleanup() {
        if (outputFormat_) {
            if (outputFormat_->pb) avio_closep(&outputFormat_->pb);
            avformat_free_context(outputFormat_);
            outputFormat_ = nullptr;
        }
        if (encodedVideoFrame_) av_frame_free(&encodedVideoFrame_);
        if (audioFifo_) av_audio_fifo_free(audioFifo_);
        audioFifo_ = nullptr;
        if (audioResampler_) swr_free(&audioResampler_);
        if (fromRgb_) sws_freeContext(fromRgb_);
        fromRgb_ = nullptr;
        if (toRgb_) sws_freeContext(toRgb_);
        toRgb_ = nullptr;
        if (audioEncoder_) avcodec_free_context(&audioEncoder_);
        if (audioDecoder_) avcodec_free_context(&audioDecoder_);
        if (videoEncoder_) avcodec_free_context(&videoEncoder_);
        if (videoDecoder_) avcodec_free_context(&videoDecoder_);
        if (inputFormat_) avformat_close_input(&inputFormat_);
    }

    bool initInput(std::string& error) {
        int rc = avformat_open_input(
            &inputFormat_, pathUtf8(options_.input).c_str(), nullptr, nullptr);
        if (rc < 0) {
            error = "avformat_open_input: " + fferr(rc);
            return false;
        }
        rc = avformat_find_stream_info(inputFormat_, nullptr);
        if (rc < 0) {
            error = "avformat_find_stream_info: " + fferr(rc);
            return false;
        }
        videoIndex_ = av_find_best_stream(
            inputFormat_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (videoIndex_ < 0) {
            error = "No video stream";
            return false;
        }
        inputVideo_ = inputFormat_->streams[videoIndex_];
        width_ = inputVideo_->codecpar->width;
        height_ = inputVideo_->codecpar->height;
        if (width_ <= 0 || height_ <= 0 || (width_ & 1) || (height_ & 1)) {
            error = "CPU conversion requires positive even video dimensions";
            return false;
        }
        frameRate_ = av_guess_frame_rate(inputFormat_, inputVideo_, nullptr);
        if (frameRate_.num <= 0 || frameRate_.den <= 0) {
            frameRate_ = AVRational{30, 1};
        }

        const AVCodec* decoder = avcodec_find_decoder(
            inputVideo_->codecpar->codec_id);
        if (!decoder) {
            error = "Software video decoder not found";
            return false;
        }
        videoDecoder_ = avcodec_alloc_context3(decoder);
        if (!videoDecoder_) {
            error = "avcodec_alloc_context3 video decoder failed";
            return false;
        }
        rc = avcodec_parameters_to_context(
            videoDecoder_, inputVideo_->codecpar);
        if (rc < 0) {
            error = "avcodec_parameters_to_context video: " + fferr(rc);
            return false;
        }
        videoDecoder_->pkt_timebase = inputVideo_->time_base;
        videoDecoder_->thread_count = 0;
        rc = avcodec_open2(videoDecoder_, decoder, nullptr);
        if (rc < 0) {
            error = "avcodec_open2 software video decoder: " + fferr(rc);
            return false;
        }

        audioIndex_ = av_find_best_stream(
            inputFormat_, AVMEDIA_TYPE_AUDIO, -1, videoIndex_, nullptr, 0);
        if (audioIndex_ >= 0) inputAudio_ = inputFormat_->streams[audioIndex_];

        const size_t pixels = static_cast<size_t>(width_) *
                              static_cast<size_t>(height_);
        sourceRgb_.resize(pixels * 3);
        processedRgb_.resize(pixels * 3);
        rvmInput_.resize(pixels * 3);
        return true;
    }

    const AVCodec* selectRegularVideoEncoder() {
        const char* names[] = {"libx264", "libopenh264"};
        for (const char* name : names) {
            if (const AVCodec* codec = avcodec_find_encoder_by_name(name)) {
                return codec;
            }
        }
        return avcodec_find_encoder(AV_CODEC_ID_MPEG4);
    }

    bool initOutput(std::string& error) {
        const bool alphaWebm =
            options_.mode == CpuConversionMode::WebmVp9Alpha;
        const char* muxer = alphaWebm ? "webm" : "mp4";
        const std::string outputPath = pathUtf8(options_.output);
        int rc = avformat_alloc_output_context2(
            &outputFormat_, nullptr, muxer, outputPath.c_str());
        if (rc < 0 || !outputFormat_) {
            error = std::string("avformat_alloc_output_context2 ") + muxer;
            return false;
        }

        const AVCodec* encoder = alphaWebm
            ? avcodec_find_encoder_by_name("libvpx-vp9")
            : selectRegularVideoEncoder();
        if (!encoder) {
            error = alphaWebm
                ? "libvpx-vp9 encoder not found in linked FFmpeg"
                : "No C++ FFmpeg software MP4 video encoder was found";
            return false;
        }
        videoEncoder_ = avcodec_alloc_context3(encoder);
        if (!videoEncoder_) {
            error = "avcodec_alloc_context3 video encoder failed";
            return false;
        }
        videoEncoder_->width = width_;
        videoEncoder_->height = height_;
        videoEncoder_->pix_fmt = alphaWebm
            ? AV_PIX_FMT_YUVA420P : AV_PIX_FMT_YUV420P;
        videoEncoder_->time_base = av_inv_q(frameRate_);
        videoEncoder_->framerate = frameRate_;
        videoEncoder_->gop_size = std::max(
            1, static_cast<int>(std::lround(av_q2d(frameRate_) * 2.0)));
        videoEncoder_->max_b_frames = 0;
        if (outputFormat_->oformat->flags & AVFMT_GLOBALHEADER) {
            videoEncoder_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        }

        AVDictionary* encoderOptions = nullptr;
        if (alphaWebm) {
            videoEncoder_->thread_count = static_cast<int>(
                std::max(2u, std::thread::hardware_concurrency()));
            videoEncoder_->bit_rate = 0;
            av_dict_set(&encoderOptions, "crf",
                        std::to_string(std::clamp(options_.quality, 0, 63)).c_str(), 0);
            av_dict_set(&encoderOptions, "deadline", "good", 0);
            av_dict_set(&encoderOptions, "cpu-used", "4", 0);
            av_dict_set(&encoderOptions, "row-mt", "1", 0);
        } else if (encoder->id == AV_CODEC_ID_H264) {
            videoEncoder_->bit_rate = std::max<int64_t>(
                8000000, static_cast<int64_t>(width_) * height_ * 2);
            if (std::strcmp(encoder->name, "libx264") == 0) {
                av_dict_set(&encoderOptions, "preset", "medium", 0);
                av_dict_set(&encoderOptions, "crf",
                            std::to_string(std::clamp(options_.quality, 0, 51)).c_str(), 0);
            }
        } else {
            videoEncoder_->flags |= AV_CODEC_FLAG_QSCALE;
            videoEncoder_->global_quality = 3 * FF_QP2LAMBDA;
            videoEncoder_->bit_rate = std::max<int64_t>(
                8000000, static_cast<int64_t>(width_) * height_ * 2);
        }

        rc = avcodec_open2(videoEncoder_, encoder, &encoderOptions);
        av_dict_free(&encoderOptions);
        if (rc < 0) {
            error = std::string("avcodec_open2 ") + encoder->name + ": " +
                    fferr(rc);
            return false;
        }
        logCpu(std::string("video encoder=") + encoder->name);

        outputVideo_ = avformat_new_stream(outputFormat_, nullptr);
        if (!outputVideo_) {
            error = "avformat_new_stream video failed";
            return false;
        }
        rc = avcodec_parameters_from_context(
            outputVideo_->codecpar, videoEncoder_);
        if (rc < 0) {
            error = "avcodec_parameters_from_context video: " + fferr(rc);
            return false;
        }
        outputVideo_->time_base = videoEncoder_->time_base;
        outputVideo_->codecpar->codec_tag = 0;
        if (alphaWebm) {
            av_dict_set(&outputVideo_->metadata, "alpha_mode", "1", 0);
        } else if (encoder->id == AV_CODEC_ID_H264) {
            outputVideo_->codecpar->codec_tag = MKTAG('a', 'v', 'c', '1');
        }

        encodedVideoFrame_ = av_frame_alloc();
        if (!encodedVideoFrame_) {
            error = "av_frame_alloc output video failed";
            return false;
        }
        encodedVideoFrame_->format = videoEncoder_->pix_fmt;
        encodedVideoFrame_->width = width_;
        encodedVideoFrame_->height = height_;
        rc = av_frame_get_buffer(encodedVideoFrame_, 64);
        if (rc < 0) {
            error = "av_frame_get_buffer output video: " + fferr(rc);
            return false;
        }

        if (inputAudio_ && !initAudio(error)) return false;

        rc = avio_open2(&outputFormat_->pb, outputPath.c_str(),
                        AVIO_FLAG_WRITE, nullptr, nullptr);
        if (rc < 0) {
            error = "avio_open2 output: " + fferr(rc);
            return false;
        }
        AVDictionary* muxOptions = nullptr;
        if (!alphaWebm) av_dict_set(&muxOptions, "movflags", "+faststart", 0);
        rc = avformat_write_header(outputFormat_, &muxOptions);
        av_dict_free(&muxOptions);
        if (rc < 0) {
            error = "avformat_write_header: " + fferr(rc);
            return false;
        }
        return true;
    }

    bool initRvm(std::string& error) {
        const float ratio = options_.downsample > 0.0f
            ? options_.downsample : 0.25f;
        logCpu("RVM CPU EP init model=" + pathUtf8(options_.model));
        return rvm_.initialize(
            options_.model.wstring(), width_, height_, ratio, error);
    }

    bool initAudio(std::string& error) {
        const bool alphaWebm =
            options_.mode == CpuConversionMode::WebmVp9Alpha;
        const AVCodecID inputId = inputAudio_->codecpar->codec_id;
        const bool compatibleCopy = alphaWebm
            ? (inputId == AV_CODEC_ID_OPUS || inputId == AV_CODEC_ID_VORBIS)
            : inputId == AV_CODEC_ID_AAC;
        if (compatibleCopy) {
            outputAudio_ = avformat_new_stream(outputFormat_, nullptr);
            if (!outputAudio_) {
                error = "avformat_new_stream copied audio failed";
                return false;
            }
            const int rc = avcodec_parameters_copy(
                outputAudio_->codecpar, inputAudio_->codecpar);
            if (rc < 0) {
                error = "avcodec_parameters_copy audio: " + fferr(rc);
                return false;
            }
            outputAudio_->codecpar->codec_tag = 0;
            outputAudio_->time_base = inputAudio_->time_base;
            audioCopy_ = true;
            logCpu(std::string("audio copy=") + avcodec_get_name(inputId));
            return true;
        }

        const AVCodec* decoder = avcodec_find_decoder(inputId);
        if (!decoder) {
            error = std::string("audio decoder not found for ") +
                    avcodec_get_name(inputId);
            return false;
        }
        audioDecoder_ = avcodec_alloc_context3(decoder);
        if (!audioDecoder_) {
            error = "avcodec_alloc_context3 audio decoder failed";
            return false;
        }
        int rc = avcodec_parameters_to_context(
            audioDecoder_, inputAudio_->codecpar);
        if (rc < 0) {
            error = "avcodec_parameters_to_context audio: " + fferr(rc);
            return false;
        }
        audioDecoder_->pkt_timebase = inputAudio_->time_base;
        rc = avcodec_open2(audioDecoder_, decoder, nullptr);
        if (rc < 0) {
            error = "avcodec_open2 audio decoder: " + fferr(rc);
            return false;
        }
        if (audioDecoder_->sample_rate <= 0) audioDecoder_->sample_rate = 48000;
        if (audioDecoder_->ch_layout.nb_channels <= 0) {
            av_channel_layout_default(&audioDecoder_->ch_layout, 2);
        }

        const AVCodecID outputId = alphaWebm
            ? AV_CODEC_ID_OPUS : AV_CODEC_ID_AAC;
        const AVCodec* encoder = alphaWebm
            ? avcodec_find_encoder_by_name("libopus")
            : avcodec_find_encoder(outputId);
        if (!encoder) {
            error = std::string("audio encoder not found for ") +
                    avcodec_get_name(outputId);
            return false;
        }
        audioEncoder_ = avcodec_alloc_context3(encoder);
        if (!audioEncoder_) {
            error = "avcodec_alloc_context3 audio encoder failed";
            return false;
        }
        const int channels = audioDecoder_->ch_layout.nb_channels == 1 ? 1 : 2;
        audioEncoder_->sample_rate = 48000;
        audioEncoder_->sample_fmt = alphaWebm
            ? AV_SAMPLE_FMT_FLT : AV_SAMPLE_FMT_FLTP;
        audioEncoder_->bit_rate = channels == 1 ? 128000 : 192000;
        audioEncoder_->time_base = AVRational{1, audioEncoder_->sample_rate};
        av_channel_layout_default(&audioEncoder_->ch_layout, channels);
        if (outputFormat_->oformat->flags & AVFMT_GLOBALHEADER) {
            audioEncoder_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        }
        rc = avcodec_open2(audioEncoder_, encoder, nullptr);
        if (rc < 0) {
            error = "avcodec_open2 audio encoder: " + fferr(rc);
            return false;
        }
        outputAudio_ = avformat_new_stream(outputFormat_, nullptr);
        if (!outputAudio_) {
            error = "avformat_new_stream transcoded audio failed";
            return false;
        }
        rc = avcodec_parameters_from_context(
            outputAudio_->codecpar, audioEncoder_);
        if (rc < 0) {
            error = "avcodec_parameters_from_context audio: " + fferr(rc);
            return false;
        }
        outputAudio_->codecpar->codec_tag = 0;
        outputAudio_->time_base = audioEncoder_->time_base;

        audioFifo_ = av_audio_fifo_alloc(
            audioEncoder_->sample_fmt,
            audioEncoder_->ch_layout.nb_channels,
            std::max(4096, audioEncoder_->frame_size * 4));
        if (!audioFifo_) {
            error = "av_audio_fifo_alloc failed";
            return false;
        }
        logCpu(std::string("audio transcode ") +
               avcodec_get_name(inputId) + "->" +
               avcodec_get_name(outputId));
        return true;
    }

    bool sendVideoPacket(AVPacket* packet, std::string& error) {
        int rc = avcodec_send_packet(videoDecoder_, packet);
        if (rc < 0 && rc != AVERROR(EAGAIN)) {
            error = "avcodec_send_packet video: " + fferr(rc);
            return false;
        }
        return drainVideoDecoder(error);
    }

    bool drainVideoDecoder(std::string& error) {
        std::unique_ptr<AVFrame, FrameDeleter> frame(av_frame_alloc());
        if (!frame) {
            error = "av_frame_alloc decoded video failed";
            return false;
        }
        for (;;) {
            const int rc = avcodec_receive_frame(videoDecoder_, frame.get());
            if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return true;
            if (rc < 0) {
                error = "avcodec_receive_frame video: " + fferr(rc);
                return false;
            }
            if (!processVideoFrame(frame.get(), error)) return false;
            av_frame_unref(frame.get());
        }
    }

    bool processVideoFrame(AVFrame* frame, std::string& error) {
        toRgb_ = sws_getCachedContext(
            toRgb_, frame->width, frame->height,
            static_cast<AVPixelFormat>(frame->format),
            width_, height_, AV_PIX_FMT_RGB24,
            SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!toRgb_) {
            error = "sws_getCachedContext decode->RGB failed";
            return false;
        }
        uint8_t* rgbData[4]{sourceRgb_.data(), nullptr, nullptr, nullptr};
        int rgbLinesize[4]{width_ * 3, 0, 0, 0};
        const int scaled = sws_scale(
            toRgb_, frame->data, frame->linesize, 0, frame->height,
            rgbData, rgbLinesize);
        if (scaled != height_) {
            error = "sws_scale decode->RGB returned " + std::to_string(scaled);
            return false;
        }

        const size_t pixels = static_cast<size_t>(width_) *
                              static_cast<size_t>(height_);
        for (size_t i = 0; i < pixels; ++i) {
            rvmInput_[i] = sourceRgb_[i * 3] / 255.0f;
            rvmInput_[pixels + i] = sourceRgb_[i * 3 + 1] / 255.0f;
            rvmInput_[pixels * 2 + i] = sourceRgb_[i * 3 + 2] / 255.0f;
        }
        RvmCpuOutput rvmOutput{};
        if (!rvm_.run(rvmInput_.data(), rvmOutput, error)) return false;
        if (!rvmOutput.alphaFp32) {
            error = "RVM CPU returned no alpha plane";
            return false;
        }

        processedRgb_ = sourceRgb_;
        if (options_.mode == CpuConversionMode::ChromaKey) {
            for (size_t i = 0; i < pixels; ++i) {
                const float alpha = clamp01(rvmOutput.alphaFp32[i]);
                processedRgb_[i * 3] = static_cast<uint8_t>(
                    sourceRgb_[i * 3] * alpha + 0.5f);
                processedRgb_[i * 3 + 1] = static_cast<uint8_t>(
                    sourceRgb_[i * 3 + 1] * alpha +
                    255.0f * (1.0f - alpha) + 0.5f);
                processedRgb_[i * 3 + 2] = static_cast<uint8_t>(
                    sourceRgb_[i * 3 + 2] * alpha + 0.5f);
            }
        } else if (options_.mode == CpuConversionMode::AlphaPacked) {
            packAlpha(rvmOutput.alphaFp32);
        }

        int rc = av_frame_make_writable(encodedVideoFrame_);
        if (rc < 0) {
            error = "av_frame_make_writable video: " + fferr(rc);
            return false;
        }
        fromRgb_ = sws_getCachedContext(
            fromRgb_, width_, height_, AV_PIX_FMT_RGB24,
            width_, height_, videoEncoder_->pix_fmt,
            SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!fromRgb_) {
            error = "sws_getCachedContext RGB->encoder failed";
            return false;
        }
        const uint8_t* sourceData[4]{
            processedRgb_.data(), nullptr, nullptr, nullptr};
        int sourceLinesize[4]{width_ * 3, 0, 0, 0};
        const int converted = sws_scale(
            fromRgb_, sourceData, sourceLinesize, 0, height_,
            encodedVideoFrame_->data, encodedVideoFrame_->linesize);
        if (converted != height_) {
            error = "sws_scale RGB->encoder returned " +
                    std::to_string(converted);
            return false;
        }
        if (options_.mode == CpuConversionMode::WebmVp9Alpha) {
            for (int y = 0; y < height_; ++y) {
                uint8_t* alphaRow = encodedVideoFrame_->data[3] +
                    static_cast<ptrdiff_t>(y) * encodedVideoFrame_->linesize[3];
                for (int x = 0; x < width_; ++x) {
                    alphaRow[x] = static_cast<uint8_t>(
                        clamp01(rvmOutput.alphaFp32[
                            static_cast<size_t>(y) * width_ + x]) *
                            255.0f + 0.5f);
                }
            }
        }

        int64_t sourcePts = frame->best_effort_timestamp != AV_NOPTS_VALUE
            ? frame->best_effort_timestamp : frame->pts;
        int64_t encoderPts = sourcePts == AV_NOPTS_VALUE
            ? static_cast<int64_t>(processedFrames_)
            : av_rescale_q(sourcePts, inputVideo_->time_base,
                           videoEncoder_->time_base);
        if (lastVideoPts_ != AV_NOPTS_VALUE && encoderPts <= lastVideoPts_) {
            encoderPts = lastVideoPts_ + 1;
        }
        lastVideoPts_ = encoderPts;
        encodedVideoFrame_->pts = encoderPts;
        rc = avcodec_send_frame(videoEncoder_, encodedVideoFrame_);
        if (rc < 0) {
            error = "avcodec_send_frame video: " + fferr(rc);
            return false;
        }
        if (!drainVideoEncoder(error)) return false;
        ++processedFrames_;
        logProgress();
        return true;
    }

    void packAlpha(const float* alpha) {
        int alphaWidth = static_cast<int>(
            std::floor(width_ * 0.4f + 0.5f));
        int alphaHeight = static_cast<int>(
            std::floor(height_ * 0.4f + 0.5f));
        alphaWidth = std::max(4, alphaWidth & ~3);
        alphaHeight = std::max(2, alphaHeight & ~1);
        const int halfWidth = alphaWidth / 2;
        const int halfHeight = alphaHeight / 2;
        const int quarterWidth = alphaWidth / 4;
        const int rightSecondX = alphaWidth - quarterWidth;
        const int xCenter = width_ / 2 - halfWidth / 2;
        const int yBottom = height_ - halfHeight;
        const int xRight = width_ - quarterWidth;

        auto writeBlock = [&](int destinationX, int destinationY,
                              int blockWidth, int blockHeight,
                              int alphaX, int alphaY) {
            for (int y = 0; y < blockHeight; ++y) {
                for (int x = 0; x < blockWidth; ++x) {
                    const int packedX = alphaX + x;
                    const int packedY = alphaY + y;
                    const int sourceX = std::clamp(
                        (packedX * width_) / std::max(1, alphaWidth),
                        0, width_ - 1);
                    const int sourceY = std::clamp(
                        (packedY * height_) / std::max(1, alphaHeight),
                        0, height_ - 1);
                    const size_t sourceIndex =
                        static_cast<size_t>(sourceY) * width_ + sourceX;
                    const size_t destinationIndex =
                        (static_cast<size_t>(destinationY + y) * width_ +
                         destinationX + x) * 3;
                    processedRgb_[destinationIndex] = static_cast<uint8_t>(
                        clamp01(alpha[sourceIndex]) * 255.0f + 0.5f);
                    processedRgb_[destinationIndex + 1] = 0;
                    processedRgb_[destinationIndex + 2] = 0;
                }
            }
        };

        writeBlock(xCenter, yBottom, halfWidth, halfHeight, 0, 0);
        writeBlock(xCenter, 0, halfWidth, halfHeight, 0, halfHeight);
        writeBlock(xRight, yBottom, quarterWidth, halfHeight,
                   halfWidth, 0);
        writeBlock(0, yBottom, quarterWidth, halfHeight,
                   rightSecondX, 0);
        writeBlock(xRight, 0, quarterWidth, halfHeight,
                   halfWidth, halfHeight);
        writeBlock(0, 0, quarterWidth, halfHeight,
                   rightSecondX, halfHeight);
    }

    bool drainVideoEncoder(std::string& error) {
        std::unique_ptr<AVPacket, PacketDeleter> packet(av_packet_alloc());
        if (!packet) {
            error = "av_packet_alloc encoded video failed";
            return false;
        }
        for (;;) {
            const int rc = avcodec_receive_packet(videoEncoder_, packet.get());
            if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return true;
            if (rc < 0) {
                error = "avcodec_receive_packet video: " + fferr(rc);
                return false;
            }
            av_packet_rescale_ts(packet.get(), videoEncoder_->time_base,
                                 outputVideo_->time_base);
            packet->stream_index = outputVideo_->index;
            packet->pos = -1;
            const int write = av_interleaved_write_frame(
                outputFormat_, packet.get());
            av_packet_unref(packet.get());
            if (write < 0) {
                error = "av_interleaved_write_frame video: " + fferr(write);
                return false;
            }
        }
    }

    bool sendAudioPacket(AVPacket* packet, std::string& error) {
        if (audioCopy_) {
            av_packet_rescale_ts(packet, inputAudio_->time_base,
                                 outputAudio_->time_base);
            packet->stream_index = outputAudio_->index;
            packet->pos = -1;
            const int write = av_interleaved_write_frame(
                outputFormat_, packet);
            if (write < 0) {
                error = "av_interleaved_write_frame copied audio: " +
                        fferr(write);
                return false;
            }
            return true;
        }

        int rc = avcodec_send_packet(audioDecoder_, packet);
        if (rc < 0 && rc != AVERROR(EAGAIN)) {
            error = "avcodec_send_packet audio: " + fferr(rc);
            return false;
        }
        return drainAudioDecoder(error);
    }

    bool drainAudioDecoder(std::string& error) {
        std::unique_ptr<AVFrame, FrameDeleter> frame(av_frame_alloc());
        if (!frame) {
            error = "av_frame_alloc decoded audio failed";
            return false;
        }
        for (;;) {
            const int rc = avcodec_receive_frame(audioDecoder_, frame.get());
            if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return true;
            if (rc < 0) {
                error = "avcodec_receive_frame audio: " + fferr(rc);
                return false;
            }
            if (!convertAudioFrame(frame.get(), error)) return false;
            av_frame_unref(frame.get());
        }
    }

    bool convertAudioFrame(AVFrame* input, std::string& error) {
        const int64_t sourcePts = input->best_effort_timestamp != AV_NOPTS_VALUE
            ? input->best_effort_timestamp : input->pts;
        if (audioNextPts_ == AV_NOPTS_VALUE && sourcePts != AV_NOPTS_VALUE) {
            audioNextPts_ = av_rescale_q(
                sourcePts, inputAudio_->time_base,
                audioEncoder_->time_base);
        }
        const int inputRate = input->sample_rate > 0
            ? input->sample_rate : audioDecoder_->sample_rate;
        const AVSampleFormat inputFormat = input->format >= 0
            ? static_cast<AVSampleFormat>(input->format)
            : audioDecoder_->sample_fmt;
        const AVChannelLayout* inputLayout =
            input->ch_layout.nb_channels > 0
                ? &input->ch_layout : &audioDecoder_->ch_layout;
        if (!audioResampler_) {
            int initialize = swr_alloc_set_opts2(
                &audioResampler_, &audioEncoder_->ch_layout,
                audioEncoder_->sample_fmt, audioEncoder_->sample_rate,
                inputLayout, inputFormat, inputRate, 0, nullptr);
            if (initialize < 0 || !audioResampler_) {
                error = "swr_alloc_set_opts2: " + fferr(initialize);
                return false;
            }
            initialize = swr_init(audioResampler_);
            if (initialize < 0) {
                error = "swr_init: " + fferr(initialize);
                return false;
            }
        }
        const int destinationSamples = static_cast<int>(av_rescale_rnd(
            swr_get_delay(audioResampler_, inputRate) +
                input->nb_samples,
            audioEncoder_->sample_rate, inputRate,
            AV_ROUND_UP));
        std::unique_ptr<AVFrame, FrameDeleter> converted(av_frame_alloc());
        if (!converted) {
            error = "av_frame_alloc converted audio failed";
            return false;
        }
        converted->format = audioEncoder_->sample_fmt;
        converted->sample_rate = audioEncoder_->sample_rate;
        av_channel_layout_copy(
            &converted->ch_layout, &audioEncoder_->ch_layout);
        converted->nb_samples = destinationSamples;
        int rc = av_frame_get_buffer(converted.get(), 0);
        if (rc < 0) {
            error = "av_frame_get_buffer converted audio: " + fferr(rc);
            return false;
        }
        const int inputPlaneCount = av_sample_fmt_is_planar(inputFormat)
            ? std::max(1, inputLayout->nb_channels) : 1;
        std::vector<const uint8_t*> inputPlanes(
            static_cast<size_t>(inputPlaneCount));
        for (size_t i = 0; i < inputPlanes.size(); ++i) {
            inputPlanes[i] = input->extended_data[i];
        }
        rc = swr_convert(audioResampler_, converted->data,
                         destinationSamples, inputPlanes.data(),
                         input->nb_samples);
        if (rc < 0) {
            error = "swr_convert: " + fferr(rc);
            return false;
        }
        if (av_audio_fifo_realloc(
                audioFifo_, av_audio_fifo_size(audioFifo_) + rc) < 0) {
            error = "av_audio_fifo_realloc failed";
            return false;
        }
        const int written = av_audio_fifo_write(
            audioFifo_, reinterpret_cast<void**>(converted->data), rc);
        if (written != rc) {
            error = "av_audio_fifo_write failed";
            return false;
        }
        return encodeAudioFifo(false, error);
    }

    bool encodeAudioFifo(bool flushRemainder, std::string& error) {
        const int frameSamples = audioEncoder_->frame_size > 0
            ? audioEncoder_->frame_size : 1024;
        while (av_audio_fifo_size(audioFifo_) >= frameSamples ||
               (flushRemainder && av_audio_fifo_size(audioFifo_) > 0)) {
            const int take = std::min(
                frameSamples, av_audio_fifo_size(audioFifo_));
            std::unique_ptr<AVFrame, FrameDeleter> frame(av_frame_alloc());
            if (!frame) {
                error = "av_frame_alloc encoded audio failed";
                return false;
            }
            frame->format = audioEncoder_->sample_fmt;
            frame->sample_rate = audioEncoder_->sample_rate;
            av_channel_layout_copy(&frame->ch_layout,
                                   &audioEncoder_->ch_layout);
            frame->nb_samples = frameSamples;
            int rc = av_frame_get_buffer(frame.get(), 0);
            if (rc < 0) {
                error = "av_frame_get_buffer encoded audio: " + fferr(rc);
                return false;
            }
            const int read = av_audio_fifo_read(
                audioFifo_, reinterpret_cast<void**>(frame->data), take);
            if (read != take) {
                error = "av_audio_fifo_read failed";
                return false;
            }
            if (take < frameSamples) {
                av_samples_set_silence(
                    frame->data, take, frameSamples - take,
                    audioEncoder_->ch_layout.nb_channels,
                    audioEncoder_->sample_fmt);
            }
            if (audioNextPts_ == AV_NOPTS_VALUE) audioNextPts_ = 0;
            frame->pts = audioNextPts_;
            audioNextPts_ += frameSamples;
            rc = avcodec_send_frame(audioEncoder_, frame.get());
            if (rc < 0) {
                error = "avcodec_send_frame audio: " + fferr(rc);
                return false;
            }
            if (!drainAudioEncoder(error)) return false;
        }
        return true;
    }

    bool drainAudioEncoder(std::string& error) {
        std::unique_ptr<AVPacket, PacketDeleter> packet(av_packet_alloc());
        if (!packet) {
            error = "av_packet_alloc encoded audio failed";
            return false;
        }
        for (;;) {
            const int rc = avcodec_receive_packet(audioEncoder_, packet.get());
            if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return true;
            if (rc < 0) {
                error = "avcodec_receive_packet audio: " + fferr(rc);
                return false;
            }
            av_packet_rescale_ts(packet.get(), audioEncoder_->time_base,
                                 outputAudio_->time_base);
            packet->stream_index = outputAudio_->index;
            packet->pos = -1;
            const int write = av_interleaved_write_frame(
                outputFormat_, packet.get());
            av_packet_unref(packet.get());
            if (write < 0) {
                error = "av_interleaved_write_frame audio: " + fferr(write);
                return false;
            }
        }
    }

    bool flushVideo(std::string& error) {
        int rc = avcodec_send_packet(videoDecoder_, nullptr);
        if (rc >= 0 && !drainVideoDecoder(error)) return false;
        rc = avcodec_send_frame(videoEncoder_, nullptr);
        if (rc >= 0 && !drainVideoEncoder(error)) return false;
        return true;
    }

    bool flushAudio(std::string& error) {
        if (!outputAudio_ || audioCopy_) return true;
        int rc = avcodec_send_packet(audioDecoder_, nullptr);
        if (rc >= 0 && !drainAudioDecoder(error)) return false;

        if (!audioResampler_) {
            rc = avcodec_send_frame(audioEncoder_, nullptr);
            if (rc >= 0 && !drainAudioEncoder(error)) return false;
            return true;
        }

        for (;;) {
            const int delayed = static_cast<int>(av_rescale_rnd(
                swr_get_delay(audioResampler_, audioDecoder_->sample_rate),
                audioEncoder_->sample_rate, audioDecoder_->sample_rate,
                AV_ROUND_UP));
            if (delayed <= 0) break;
            std::unique_ptr<AVFrame, FrameDeleter> converted(av_frame_alloc());
            if (!converted) {
                error = "av_frame_alloc audio resampler flush failed";
                return false;
            }
            converted->format = audioEncoder_->sample_fmt;
            converted->sample_rate = audioEncoder_->sample_rate;
            av_channel_layout_copy(&converted->ch_layout,
                                   &audioEncoder_->ch_layout);
            converted->nb_samples = delayed;
            rc = av_frame_get_buffer(converted.get(), 0);
            if (rc < 0) {
                error = "av_frame_get_buffer audio flush: " + fferr(rc);
                return false;
            }
            rc = swr_convert(audioResampler_, converted->data, delayed,
                             nullptr, 0);
            if (rc <= 0) break;
            if (av_audio_fifo_realloc(
                    audioFifo_, av_audio_fifo_size(audioFifo_) + rc) < 0) {
                error = "av_audio_fifo_realloc flush failed";
                return false;
            }
            if (av_audio_fifo_write(
                    audioFifo_, reinterpret_cast<void**>(converted->data), rc) != rc) {
                error = "av_audio_fifo_write flush failed";
                return false;
            }
        }
        if (!encodeAudioFifo(true, error)) return false;
        rc = avcodec_send_frame(audioEncoder_, nullptr);
        if (rc >= 0 && !drainAudioEncoder(error)) return false;
        return true;
    }
};

} // namespace

bool RunCpuConversion(const CpuConversionOptions& options, std::string& error) {
    CpuConverter converter(options);
    return converter.run(error);
}
