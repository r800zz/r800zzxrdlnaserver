#include "r800zz_dml_backend.h"
#include "rvm_ort_dml.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <fcntl.h>
#include <io.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/time.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <new>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr double kDropLagSeconds = 0.120;

std::string fferr(int code) {
    char buf[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, buf, sizeof(buf));
    return buf;
}

void logLine(const std::string& s) {
    // Keep the CUDA worker log prefix so the existing server parses the same
    // META / RESIDENT / progress messages for either backend.
    std::cerr << "CPP_GPU: " << s << std::endl;
}

std::filesystem::path partialOutputPath(
        const std::filesystem::path& finalOutput) {
    auto partial = finalOutput;
    partial += L".r800zz-part";
    return partial;
}

void removePartialOutput(const std::filesystem::path& partial) {
    if (partial.empty()) return;
    std::error_code ignored;
    std::filesystem::remove(partial, ignored);
}

bool commitPartialOutput(const std::filesystem::path& partial,
                         const std::filesystem::path& finalOutput,
                         std::string& error) {
    if (MoveFileExW(partial.wstring().c_str(), finalOutput.wstring().c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return true;
    }
    error = "finalize output file Win32=" + std::to_string(GetLastError());
    return false;
}

int writeHandleAvio(void* opaque, const uint8_t* data, int size) {
    HANDLE output = static_cast<HANDLE>(opaque);
    if (!output || output == INVALID_HANDLE_VALUE || size <= 0) return AVERROR(EIO);
    int total = 0;
    while (total < size) {
        DWORD written = 0;
        const DWORD request = static_cast<DWORD>(size - total);
        if (!WriteFile(output, data + total, request, &written, nullptr) || written == 0) {
            return AVERROR(EIO);
        }
        total += static_cast<int>(written);
    }
    return total;
}

struct AvFormatCloser { void operator()(AVFormatContext* p) const { if (p) avformat_close_input(&p); } };
struct AvCodecCloser { void operator()(AVCodecContext* p) const { if (p) avcodec_free_context(&p); } };
struct AvFrameCloser { void operator()(AVFrame* p) const { if (p) av_frame_free(&p); } };
struct AvPacketCloser { void operator()(AVPacket* p) const { if (p) av_packet_free(&p); } };

struct ComReleaser {
    template<class T> void operator()(T* p) const { if (p) p->Release(); }
};

struct AdapterInfo {
    int index = -1;
    UINT vendorId = 0;
    bool software = false;
    std::wstring description;
};

std::string wideToUtf8(const std::wstring& s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(
        CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
        nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
        out.data(), n, nullptr, nullptr);
    return out;
}

std::vector<AdapterInfo> enumerateAdapters() {
    IDXGIFactory1* rawFactory = nullptr;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&rawFactory));
    if (FAILED(hr)) throw std::runtime_error("CreateDXGIFactory1 failed");
    std::unique_ptr<IDXGIFactory1, ComReleaser> factory(rawFactory);

    std::vector<AdapterInfo> result;
    for (UINT index = 0;; ++index) {
        IDXGIAdapter1* rawAdapter = nullptr;
        hr = factory->EnumAdapters1(index, &rawAdapter);
        if (hr == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(hr)) continue;
        std::unique_ptr<IDXGIAdapter1, ComReleaser> adapter(rawAdapter);
        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(adapter->GetDesc1(&desc))) continue;
        result.push_back(AdapterInfo{
            static_cast<int>(index),
            desc.VendorId,
            (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0,
            desc.Description});
    }
    return result;
}

const char* vendorName(UINT vendorId) {
    switch (vendorId) {
    case 0x1002: return "AMD";
    case 0x8086: return "Intel";
    case 0x10DE: return "NVIDIA";
    case 0x1414: return "Microsoft";
    default: return "Unknown";
    }
}

int chooseDefaultHardwareAdapter(const std::vector<AdapterInfo>& adapters) {
    for (UINT vendor : {0x1002u, 0x10DEu, 0x8086u}) {
        for (const auto& a : adapters) {
            if (!a.software && a.vendorId == vendor) return a.index;
        }
    }
    for (const auto& a : adapters) {
        if (!a.software) return a.index;
    }
    return -1;
}

AVPixelFormat chooseD3d11Format(AVCodecContext*, const AVPixelFormat* formats) {
    if (!formats) return AV_PIX_FMT_NONE;
    for (const AVPixelFormat* p = formats; *p != AV_PIX_FMT_NONE; ++p) {
        if (*p == AV_PIX_FMT_D3D11) return *p;
    }
    return formats[0];
}

bool decodeHex(const std::string& text, std::string& output) {
    if ((text.size() & 1u) != 0u) return false;
    auto value = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    output.clear();
    output.reserve(text.size() / 2);
    for (size_t i = 0; i < text.size(); i += 2) {
        const int high = value(text[i]);
        const int low = value(text[i + 1]);
        if (high < 0 || low < 0) return false;
        output.push_back(static_cast<char>((high << 4) | low));
    }
    return true;
}

struct Args {
    enum class OutputMode {
        AlphaPacked,
        WebmVp9Alpha,
        ChromaKey,
    };

    std::filesystem::path input;
    std::filesystem::path model;
    int qp = 24;
    int device = -1;
    float downsample = 0.25f;
    int64_t startMs = 0;
    bool previewOnly = false;
    bool resident = false;
    bool offlineConvert = false;
    bool noPreview = true;
    bool listDevices = false;
    RvmBackend backend = RvmBackend::DirectML;
    std::filesystem::path outputFile;
    OutputMode outputMode = OutputMode::AlphaPacked;
};

void usage() {
    std::cerr
        << "Usage:\n"
        << "  r800zz_ai_worker.exe --list-devices\n"
        << "  r800zz_ai_worker.exe <video> [output] "
           "[--backend dml|cpu] [--model MODEL] [--device N] [--qp N] "
           "[--downsample R] [--start-ms MS] "
           "[--output-mode alpha-packed|webm-alpha|chroma-key] "
           "[--resident] [--no-preview]\n";
}

bool parseArgs(int argc, wchar_t** argv, Args& out) {
    if (argc == 2 && std::wstring(argv[1]) == L"--list-devices") {
        out.listDevices = true;
        return true;
    }
    if (argc < 2) return false;
    out.input = argv[1];

    int i = 2;
    if (i < argc && argv[i][0] != L'-') {
        out.outputFile = argv[i++];
        out.offlineConvert = true;
    }

    for (; i < argc; ++i) {
        const std::wstring a = argv[i];
        auto need = [&](const wchar_t*) -> const wchar_t* {
            if (i + 1 >= argc) return nullptr;
            return argv[++i];
        };

        if (a == L"--backend") {
            const wchar_t* v = need(L"--backend"); if (!v) return false;
            const std::wstring b(v);
            if (b == L"dml") out.backend = RvmBackend::DirectML;
            else if (b == L"cpu") out.backend = RvmBackend::CPU;
            else return false;
        } else if (a == L"--model") {
            const wchar_t* v = need(L"--model"); if (!v) return false;
            out.model = v;
        } else if (a == L"--device") {
            const wchar_t* v = need(L"--device"); if (!v) return false;
            out.device = _wtoi(v);
        } else if (a == L"--qp" || a == L"--crf") {
            const wchar_t* v = need(a.c_str()); if (!v) return false;
            out.qp = std::clamp(_wtoi(v), 0, 63);
        } else if (a == L"--downsample") {
            const wchar_t* v = need(L"--downsample"); if (!v) return false;
            out.downsample = static_cast<float>(_wtof(v));
        } else if (a == L"--start-ms") {
            const wchar_t* v = need(L"--start-ms"); if (!v) return false;
            out.startMs = std::max<int64_t>(0, _wtoi64(v));
        } else if (a == L"--output-mode") {
            const wchar_t* v = need(L"--output-mode"); if (!v) return false;
            const std::wstring mode(v);
            if (mode == L"alpha-packed") out.outputMode = Args::OutputMode::AlphaPacked;
            else if (mode == L"webm-alpha") out.outputMode = Args::OutputMode::WebmVp9Alpha;
            else if (mode == L"chroma-key") out.outputMode = Args::OutputMode::ChromaKey;
            else return false;
        } else if (a == L"--resident") {
            out.resident = true;
        } else if (a == L"--no-preview") {
            out.noPreview = true;
        } else {
            return false;
        }
    }

    if (out.model.empty()) {
        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        out.model = std::filesystem::path(exe).parent_path() /
                    L"rvm_mobilenetv3_fp32.onnx";
    }
    return true;
}

bool isTsAudioCopySupported(AVCodecID id) {
    return id == AV_CODEC_ID_AAC;
}

bool codecSupportsPixelFormat(const AVCodec* codec, AVPixelFormat wanted) {
    if (!codec || !codec->pix_fmts) return false;
    for (const AVPixelFormat* pf = codec->pix_fmts;
         *pf != AV_PIX_FMT_NONE; ++pf) {
        if (*pf == wanted) return true;
    }
    return false;
}


struct Pipeline {
    Args args;
    AVFormatContext* inFmt = nullptr;
    AVCodecContext* dec = nullptr;
    AVCodecContext* enc = nullptr;
    AVCodecContext* audioDec = nullptr;
    AVCodecContext* audioEnc = nullptr;
    AVFormatContext* outFmt = nullptr;
    AVBufferRef* d3d11Device = nullptr;
    AVBufferRef* externalD3d11Device = nullptr;
    AVBufferRef* encFrames = nullptr;
    AVStream* inVideo = nullptr;
    AVStream* outVideo = nullptr;
    AVStream* inAudio = nullptr;
    AVStream* outAudio = nullptr;
    SwrContext* audioSwr = nullptr;
    AVAudioFifo* audioFifo = nullptr;
    bool audioTranscode = false;
    int64_t audioNextPts = AV_NOPTS_VALUE;
    int videoIndex = -1;
    int audioIndex = -1;

    std::unique_ptr<RvmOrt> ownedRvm;
    RvmOrt* rvm = nullptr;
    RvmOrt* externalRvm = nullptr;

    HANDLE outputHandle = INVALID_HANDLE_VALUE;
    std::atomic<bool>* cancelFlag = nullptr;

    bool encoderGpuDirect = false;
    UINT selectedVendorId = 0;
    std::wstring selectedAdapterName;

    AVFrame* webmCpuFrame = nullptr;
    AVFrame* cpuNv12Frame = nullptr;
    AVFrame* legacyEncodeFrame = nullptr;
    SwsContext* toRgb = nullptr;
    SwsContext* rgbToOutput = nullptr;
    std::vector<uint8_t> rgb;
    std::vector<float> nchw;

    int width = 0;
    int height = 0;
    AVRational frameRate{30, 1};
    int64_t firstVideoPts = AV_NOPTS_VALUE;
    std::chrono::steady_clock::time_point wallStart{};
    uint64_t processed = 0;
    uint64_t dropped = 0;
    uint64_t videoPacketsRead = 0;
    uint64_t decodedFrames = 0;
    uint64_t encodedPackets = 0;
    int64_t requestedStartTimestampUs = 0;
    bool firstFrameTrace = true;
    bool playbackClockStarted = false;
    bool videoOutputStarted = false;
    int64_t currentVideoSourceUs = AV_NOPTS_VALUE;
    int64_t firstVideoOutputUs = AV_NOPTS_VALUE;
    int64_t latestVideoOutputUs = AV_NOPTS_VALUE;
    int64_t lastSubmittedVideoPts = AV_NOPTS_VALUE;
    int64_t lastMuxVideoDts = AV_NOPTS_VALUE;
    std::deque<AVPacket*> pendingAudioPackets;
    double inferMsAccum = 0.0;
    double packMsAccum = 0.0;
    double encodeMsAccum = 0.0;
    std::chrono::steady_clock::time_point statsStart{};
    std::chrono::steady_clock::time_point lastProgressLog{};

    void logConversionProgress(bool force = false) {
        if (!args.offlineConvert) return;
        const auto now = std::chrono::steady_clock::now();
        if (!force && lastProgressLog.time_since_epoch().count() != 0 &&
            now - lastProgressLog < std::chrono::milliseconds(200)) {
            return;
        }
        logLine("PROGRESS frames=" + std::to_string(processed));
        lastProgressLog = now;
    }

    ~Pipeline() { cleanup(); }

    void cleanup() {
        for (AVPacket* packet : pendingAudioPackets) av_packet_free(&packet);
        pendingAudioPackets.clear();

        if (outFmt) {
            if (outFmt->pb) {
                avio_flush(outFmt->pb);
                if (outFmt->flags & AVFMT_FLAG_CUSTOM_IO) {
                    av_freep(&outFmt->pb->buffer);
                    avio_context_free(&outFmt->pb);
                } else {
                    avio_closep(&outFmt->pb);
                }
            }
            avformat_free_context(outFmt);
            outFmt = nullptr;
        }

        if (audioFifo) av_audio_fifo_free(audioFifo);
        audioFifo = nullptr;
        if (audioSwr) swr_free(&audioSwr);
        if (audioEnc) avcodec_free_context(&audioEnc);
        if (audioDec) avcodec_free_context(&audioDec);
        if (enc) avcodec_free_context(&enc);
        if (dec) avcodec_free_context(&dec);
        if (inFmt) avformat_close_input(&inFmt);
        if (encFrames) av_buffer_unref(&encFrames);
        if (d3d11Device) av_buffer_unref(&d3d11Device);

        if (webmCpuFrame) av_frame_free(&webmCpuFrame);
        if (cpuNv12Frame) av_frame_free(&cpuNv12Frame);
        if (legacyEncodeFrame) av_frame_free(&legacyEncodeFrame);
        if (toRgb) sws_freeContext(toRgb);
        toRgb = nullptr;
        if (rgbToOutput) sws_freeContext(rgbToOutput);
        rgbToOutput = nullptr;

        ownedRvm.reset();
        rvm = nullptr;
    }

    bool initInput(std::string& error) {
        const std::string inputUtf8 = args.input.u8string();
        int rc = avformat_open_input(&inFmt, inputUtf8.c_str(), nullptr, nullptr);
        if (rc < 0) { error = "avformat_open_input: " + fferr(rc); return false; }
        rc = avformat_find_stream_info(inFmt, nullptr);
        if (rc < 0) { error = "avformat_find_stream_info: " + fferr(rc); return false; }

        videoIndex = av_find_best_stream(inFmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (videoIndex < 0) { error = "No video stream"; return false; }
        inVideo = inFmt->streams[videoIndex];
        width = inVideo->codecpar->width;
        height = inVideo->codecpar->height;
        frameRate = av_guess_frame_rate(inFmt, inVideo, nullptr);
        if (frameRate.num <= 0 || frameRate.den <= 0) frameRate = AVRational{30, 1};

        audioIndex = av_find_best_stream(inFmt, AVMEDIA_TYPE_AUDIO, -1, videoIndex, nullptr, 0);
        if (audioIndex >= 0) inAudio = inFmt->streams[audioIndex];

        int64_t durationMs = 0;
        if (inFmt->duration != AV_NOPTS_VALUE && inFmt->duration > 0) {
            durationMs = av_rescale_q(inFmt->duration, AV_TIME_BASE_Q, AVRational{1, 1000});
        }
        logLine("META durationMs=" + std::to_string(durationMs));

        if (args.startMs > 0) {
            const int64_t baseUs = inFmt->start_time != AV_NOPTS_VALUE ? inFmt->start_time : 0;
            requestedStartTimestampUs = baseUs + args.startMs * 1000LL;
            rc = avformat_seek_file(
                inFmt, -1, INT64_MIN, requestedStartTimestampUs, INT64_MAX, AVSEEK_FLAG_BACKWARD);
            if (rc < 0) {
                error = "avformat_seek_file startMs=" + std::to_string(args.startMs) + ": " + fferr(rc);
                return false;
            }
            logLine("input seek requested startMs=" + std::to_string(args.startMs));
        }
        return true;
    }


    bool initD3d11Decoder(std::string& error) {
        const AVCodec* decoder = avcodec_find_decoder(inVideo->codecpar->codec_id);
        if (!decoder) {
            error = "Video decoder not found";
            return false;
        }

        dec = avcodec_alloc_context3(decoder);
        if (!dec) {
            error = "avcodec_alloc_context3 decoder failed";
            return false;
        }

        int rc = avcodec_parameters_to_context(dec, inVideo->codecpar);
        if (rc < 0) {
            error = "avcodec_parameters_to_context: " + fferr(rc);
            return false;
        }
        dec->pkt_timebase = inVideo->time_base;

        if (args.backend == RvmBackend::DirectML) {
            if (externalD3d11Device) {
                d3d11Device = av_buffer_ref(externalD3d11Device);
                if (!d3d11Device) {
                    error = "av_buffer_ref resident D3D11 device failed";
                    return false;
                }
                logLine("resident FFmpeg D3D11 device reused");
            } else {
                const auto adapters = enumerateAdapters();
                if (args.device < 0) args.device = chooseDefaultHardwareAdapter(adapters);
                if (args.device < 0) {
                    error = "No hardware DXGI adapter was found";
                    return false;
                }
                const auto selected = std::find_if(
                    adapters.begin(), adapters.end(),
                    [&](const AdapterInfo& a) { return a.index == args.device; });
                if (selected == adapters.end()) {
                    error = "DXGI adapter " + std::to_string(args.device) + " not found";
                    return false;
                }
                selectedVendorId = selected->vendorId;
                selectedAdapterName = selected->description;

                AVBufferRef* raw = nullptr;
                const std::string adapterText = std::to_string(args.device);
                rc = av_hwdevice_ctx_create(
                    &raw, AV_HWDEVICE_TYPE_D3D11VA,
                    adapterText.c_str(), nullptr, 0);
                if (rc < 0 || !raw) {
                    if (raw) av_buffer_unref(&raw);
                    error = "av_hwdevice_ctx_create(D3D11VA): " + fferr(rc);
                    return false;
                }
                d3d11Device = raw;
            }

            if (selectedAdapterName.empty()) {
                const auto adapters = enumerateAdapters();
                const auto selected = std::find_if(
                    adapters.begin(), adapters.end(),
                    [&](const AdapterInfo& a) { return a.index == args.device; });
                if (selected != adapters.end()) {
                    selectedVendorId = selected->vendorId;
                    selectedAdapterName = selected->description;
                }
            }

            dec->hw_device_ctx = av_buffer_ref(d3d11Device);
            if (!dec->hw_device_ctx) {
                error = "av_buffer_ref decoder D3D11 device failed";
                return false;
            }
            dec->get_format = chooseD3d11Format;
        }

        rc = avcodec_open2(dec, decoder, nullptr);
        if (rc < 0) {
            error = "avcodec_open2 decoder: " + fferr(rc);
            return false;
        }

        std::ostringstream d;
        d << "decoder selected name=" << decoder->name
          << " backend=" << (args.backend == RvmBackend::DirectML ? "D3D11VA" : "CPU");
        logLine(d.str());
        return true;
    }

    bool initRvm(std::string& error) {
        if (externalRvm) {
            rvm = externalRvm;
            if (!rvm->resetState(error)) return false;
            logLine("RVM resident session reused");
            return true;
        }

        ownedRvm = std::make_unique<RvmOrt>();
        if (!ownedRvm->initialize(
                args.model.wstring(),
                width, height,
                args.downsample > 0.0f ? args.downsample : 0.25f,
                args.backend,
                args.backend == RvmBackend::DirectML ? args.device : -1,
                error)) {
            ownedRvm.reset();
            return false;
        }
        rvm = ownedRvm.get();

        if (!rvm->resetState(error)) return false;

        if (args.backend == RvmBackend::DirectML) {
            logLine("RVM backend=DirectML device=" + std::to_string(args.device) +
                    (selectedAdapterName.empty()
                        ? std::string{}
                        : " name=\"" + wideToUtf8(selectedAdapterName) + "\""));
        } else {
            logLine("RVM backend=CPU");
        }
        return true;
    }

    bool initPreview(std::string&) {
        // The DLNA worker is headless. Keep the CUDA Pipeline call site unchanged.
        return true;
    }


    bool initAudioTranscode(AVCodecID outputCodecId,
                            const char* preferredEncoderName,
                            AVSampleFormat outputSampleFormat,
                            std::string& error) {
        const char* sourceCodecName = avcodec_get_name(inAudio->codecpar->codec_id);
        const AVCodec* decCodec = avcodec_find_decoder(inAudio->codecpar->codec_id);
        if (!decCodec) {
            error = std::string("audio decoder not found for ") + sourceCodecName;
            return false;
        }
        audioDec = avcodec_alloc_context3(decCodec);
        if (!audioDec) { error = "avcodec_alloc_context3 audio decoder failed"; return false; }
        int rc = avcodec_parameters_to_context(audioDec, inAudio->codecpar);
        if (rc < 0) { error = "avcodec_parameters_to_context audio: " + fferr(rc); return false; }
        audioDec->pkt_timebase = inAudio->time_base;
        rc = avcodec_open2(audioDec, decCodec, nullptr);
        if (rc < 0) {
            error = std::string("avcodec_open2 audio decoder ") + sourceCodecName + ": " + fferr(rc);
            return false;
        }

        if (audioDec->sample_rate <= 0) audioDec->sample_rate = 48000;
        if (audioDec->ch_layout.nb_channels <= 0) {
            const int fallbackChannels =
                inAudio->codecpar->ch_layout.nb_channels > 0
                    ? inAudio->codecpar->ch_layout.nb_channels : 2;
            av_channel_layout_default(&audioDec->ch_layout, fallbackChannels);
        }

        const AVCodec* encCodec = preferredEncoderName
            ? avcodec_find_encoder_by_name(preferredEncoderName) : nullptr;
        if (!encCodec) encCodec = avcodec_find_encoder(outputCodecId);
        const char* outputCodecName = avcodec_get_name(outputCodecId);
        if (!encCodec) {
            error = std::string("audio encoder not found for ") + outputCodecName;
            return false;
        }
        audioEnc = avcodec_alloc_context3(encCodec);
        if (!audioEnc) { error = "avcodec_alloc_context3 audio encoder failed"; return false; }

        const int outChannels = audioDec->ch_layout.nb_channels == 1 ? 1 : 2;
        audioEnc->sample_rate = 48000;
        audioEnc->sample_fmt = outputSampleFormat;
        audioEnc->bit_rate = outChannels == 1 ? 128000 : 192000;
        audioEnc->time_base = AVRational{1, audioEnc->sample_rate};
        av_channel_layout_default(&audioEnc->ch_layout, outChannels);
        if (outFmt && (outFmt->oformat->flags & AVFMT_GLOBALHEADER)) {
            audioEnc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        }

        rc = avcodec_open2(audioEnc, encCodec, nullptr);
        if (rc < 0) {
            error = std::string("avcodec_open2 audio encoder ") +
                    outputCodecName + ": " + fferr(rc);
            return false;
        }

        const int fifoInitial = std::max(4096, audioEnc->frame_size * 4);
        audioFifo = av_audio_fifo_alloc(
            audioEnc->sample_fmt,
            audioEnc->ch_layout.nb_channels,
            fifoInitial);
        if (!audioFifo) { error = "av_audio_fifo_alloc failed"; return false; }

        outAudio = avformat_new_stream(outFmt, nullptr);
        if (!outAudio) { error = "avformat_new_stream transcoded audio failed"; return false; }
        rc = avcodec_parameters_from_context(outAudio->codecpar, audioEnc);
        if (rc < 0) { error = "avcodec_parameters_from_context audio: " + fferr(rc); return false; }
        outAudio->codecpar->codec_tag = 0;
        outAudio->time_base = audioEnc->time_base;
        audioTranscode = true;

        std::ostringstream oss;
        oss << "audio transcode " << sourceCodecName << "->" << outputCodecName
            << " rate=" << audioEnc->sample_rate
            << " channels=" << audioEnc->ch_layout.nb_channels
            << " bitrate=" << audioEnc->bit_rate;
        logLine(oss.str());
        return true;
    }


    bool initEncoderAndMuxer(std::string& error) {
        const bool webmAlphaOutput =
            args.outputMode == Args::OutputMode::WebmVp9Alpha;
        const char* muxerName = webmAlphaOutput
            ? "webm" : (args.offlineConvert ? "mp4" : "mpegts");

        const std::string outputPath = args.outputFile.empty()
            ? std::string{} : wideToUtf8(args.outputFile.wstring());
        int rc = avformat_alloc_output_context2(
            &outFmt, nullptr, muxerName,
            outputPath.empty() ? nullptr : outputPath.c_str());
        if (rc < 0 || !outFmt) {
            error = std::string("avformat_alloc_output_context2 ") +
                    muxerName + " failed";
            return false;
        }
        outFmt->flags |= AVFMT_FLAG_FLUSH_PACKETS;

        std::vector<const char*> encoderCandidates;
        if (webmAlphaOutput) {
            encoderCandidates.push_back("libvpx-vp9");
        } else if (args.backend == RvmBackend::DirectML) {
            if (selectedVendorId == 0x10DE) encoderCandidates.push_back("hevc_nvenc");
            if (selectedVendorId == 0x1002) encoderCandidates.push_back("hevc_amf");
            if (selectedVendorId == 0x8086) encoderCandidates.push_back("hevc_qsv");
            encoderCandidates.push_back("hevc_mf");
            encoderCandidates.push_back("libx265");
        } else {
            encoderCandidates = {
                "libx265", "hevc_mf", "hevc_nvenc", "hevc_amf", "hevc_qsv"
            };
        }

        const AVCodec* chosenEncoder = nullptr;
        std::string encoderOpenErrors;

        auto choosePixelFormat = [&](const AVCodec* codec) -> AVPixelFormat {
            if (webmAlphaOutput) return AV_PIX_FMT_YUVA420P;
            if (!codec || !codec->pix_fmts) return AV_PIX_FMT_YUV420P;
            bool hasNv12 = false;
            bool hasYuv420p = false;
            for (const AVPixelFormat* pf = codec->pix_fmts;
                 *pf != AV_PIX_FMT_NONE; ++pf) {
                if (*pf == AV_PIX_FMT_NV12) hasNv12 = true;
                if (*pf == AV_PIX_FMT_YUV420P) hasYuv420p = true;
            }
            if (hasNv12) return AV_PIX_FMT_NV12;
            if (hasYuv420p) return AV_PIX_FMT_YUV420P;
            return AV_PIX_FMT_YUV420P;
        };

        auto tryOpenEncoder = [&](const char* encoderName, bool tryD3D11) -> bool {
            const AVCodec* candidate =
                avcodec_find_encoder_by_name(encoderName);
            if (!candidate) return false;

            if (tryD3D11 &&
                !codecSupportsPixelFormat(candidate, AV_PIX_FMT_D3D11)) {
                return false;
            }

            AVCodecContext* candidateCtx = avcodec_alloc_context3(candidate);
            if (!candidateCtx) return false;

            candidateCtx->width = width;
            candidateCtx->height = height;
            candidateCtx->pix_fmt =
                tryD3D11 ? AV_PIX_FMT_D3D11 : choosePixelFormat(candidate);
            candidateCtx->time_base = inVideo->time_base;
            if (candidateCtx->time_base.num <= 0 ||
                candidateCtx->time_base.den <= 0) {
                candidateCtx->time_base = av_inv_q(frameRate);
            }
            candidateCtx->framerate = frameRate;
            candidateCtx->gop_size =
                std::max(1, static_cast<int>(
                    std::lround(av_q2d(frameRate))));
            candidateCtx->max_b_frames = 0;
            candidateCtx->thread_count = static_cast<int>(
                std::max(2u, std::thread::hardware_concurrency()));
            candidateCtx->bit_rate = 0;
            if (outFmt->oformat->flags & AVFMT_GLOBALHEADER) {
                candidateCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
            }

            AVBufferRef* framesRef = nullptr;
            if (tryD3D11) {
                framesRef = av_hwframe_ctx_alloc(d3d11Device);
                if (!framesRef) {
                    avcodec_free_context(&candidateCtx);
                    return false;
                }
                auto* frames =
                    reinterpret_cast<AVHWFramesContext*>(framesRef->data);
                frames->format = AV_PIX_FMT_D3D11;
                frames->sw_format = AV_PIX_FMT_NV12;
                frames->width = width;
                frames->height = height;
                frames->initial_pool_size = 16;
                auto* d3d11Frames =
                    reinterpret_cast<AVD3D11VAFramesContext*>(frames->hwctx);
                if (d3d11Frames) {
                    d3d11Frames->BindFlags |= D3D11_BIND_VIDEO_ENCODER;
                }
                const int framesRc = av_hwframe_ctx_init(framesRef);
                if (framesRc < 0) {
                    av_buffer_unref(&framesRef);
                    avcodec_free_context(&candidateCtx);
                    return false;
                }
                candidateCtx->hw_frames_ctx = av_buffer_ref(framesRef);
                candidateCtx->hw_device_ctx = av_buffer_ref(d3d11Device);
            }

            AVDictionary* opts = nullptr;
            if (webmAlphaOutput) {
                candidateCtx->flags |=
                    AV_CODEC_FLAG_LOW_DELAY | AV_CODEC_FLAG_QSCALE;
                candidateCtx->global_quality =
                    std::clamp(args.qp, 0, 63) * FF_QP2LAMBDA;
                av_dict_set(&opts, "crf",
                            std::to_string(args.qp).c_str(), 0);
                av_dict_set(&opts, "deadline",
                            args.offlineConvert ? "good" : "realtime", 0);
                av_dict_set(&opts, "cpu-used",
                            args.offlineConvert ? "4" : "8", 0);
                av_dict_set(&opts, "lag-in-frames",
                            args.offlineConvert ? "25" : "0", 0);
                av_dict_set(&opts, "auto-alt-ref",
                            args.offlineConvert ? "1" : "0", 0);
                av_dict_set(&opts, "row-mt", "1", 0);
                av_dict_set(&opts, "tile-columns", "2", 0);
                av_dict_set(&opts, "error-resilient", "1", 0);
            } else if (std::string(encoderName) == "hevc_nvenc") {
                // Exactly the same low-latency policy as the CUDA worker.
                av_dict_set(&opts, "preset", "p1", 0);
                av_dict_set(&opts, "tune", "ull", 0);
                av_dict_set(&opts, "rc", "constqp", 0);
                av_dict_set(&opts, "qp",
                            std::to_string(args.qp).c_str(), 0);
                av_dict_set(&opts, "zerolatency", "1", 0);
                av_dict_set(&opts, "delay", "0", 0);
                av_dict_set(&opts, "bf", "0", 0);
            } else if (std::string(encoderName) == "libx265") {
                av_dict_set(&opts, "preset", "ultrafast", 0);
                av_dict_set(&opts, "crf",
                            std::to_string(args.qp).c_str(), 0);
                av_dict_set(&opts, "x265-params",
                            "log-level=error:bframes=0", 0);
            } else {
                av_dict_set(&opts, "preset", "fast", 0);
                av_dict_set(&opts, "qp",
                            std::to_string(args.qp).c_str(), 0);
            }

            const int openRc = avcodec_open2(candidateCtx, candidate, &opts);
            av_dict_free(&opts);
            if (openRc < 0) {
                if (!encoderOpenErrors.empty()) encoderOpenErrors += "; ";
                encoderOpenErrors += std::string(encoderName) +
                    (tryD3D11 ? "[d3d11]=" : "[legacy]=") +
                    fferr(openRc);
                if (framesRef) av_buffer_unref(&framesRef);
                avcodec_free_context(&candidateCtx);
                return false;
            }

            enc = candidateCtx;
            chosenEncoder = candidate;
            encoderGpuDirect = tryD3D11;
            if (tryD3D11) {
                encFrames = framesRef;
            } else if (framesRef) {
                av_buffer_unref(&framesRef);
            }
            return true;
        };

        if (webmAlphaOutput) {
            for (const char* name : encoderCandidates) {
                if (tryOpenEncoder(name, false)) break;
            }
        } else {
            if (args.backend == RvmBackend::DirectML && d3d11Device) {
                for (const char* name : encoderCandidates) {
                    if (tryOpenEncoder(name, true)) break;
                }
            }
            if (!enc) {
                for (const char* name : encoderCandidates) {
                    if (tryOpenEncoder(name, false)) break;
                }
            }
        }

        if (!enc || !chosenEncoder) {
            error = "no usable video encoder";
            if (!encoderOpenErrors.empty()) {
                error += " (" + encoderOpenErrors + ")";
            }
            return false;
        }

        logLine(std::string("Output encoder: ") + chosenEncoder->name +
                (encoderGpuDirect ? " pixfmt=d3d11 GPU_DIRECT" : ""));

        outVideo = avformat_new_stream(outFmt, nullptr);
        if (!outVideo) {
            error = "avformat_new_stream video failed";
            return false;
        }
        rc = avcodec_parameters_from_context(outVideo->codecpar, enc);
        if (rc < 0) {
            error = "avcodec_parameters_from_context: " + fferr(rc);
            return false;
        }
        outVideo->time_base = enc->time_base;
        outVideo->codecpar->codec_tag = 0;
        if (args.offlineConvert && !webmAlphaOutput) {
            outVideo->codecpar->codec_tag = MKTAG('h', 'v', 'c', '1');
        }
        if (webmAlphaOutput) {
            av_dict_set(&outVideo->metadata, "alpha_mode", "1", 0);
        }

        if (webmAlphaOutput && inAudio) {
            if (!initAudioTranscode(
                    AV_CODEC_ID_OPUS, "libopus",
                    AV_SAMPLE_FMT_FLT, error)) {
                return false;
            }
        } else if (inAudio &&
                   isTsAudioCopySupported(inAudio->codecpar->codec_id)) {
            outAudio = avformat_new_stream(outFmt, nullptr);
            if (!outAudio) {
                error = "avformat_new_stream audio failed";
                return false;
            }
            rc = avcodec_parameters_copy(outAudio->codecpar, inAudio->codecpar);
            if (rc < 0) {
                error = "avcodec_parameters_copy audio: " + fferr(rc);
                return false;
            }
            outAudio->codecpar->codec_tag = 0;
            outAudio->time_base = inAudio->time_base;
            std::ostringstream oss;
            oss << "audio copy codec="
                << avcodec_get_name(inAudio->codecpar->codec_id)
                << " rate=" << inAudio->codecpar->sample_rate;
            logLine(oss.str());
        } else if (inAudio) {
            if (!initAudioTranscode(
                    AV_CODEC_ID_AAC, nullptr,
                    AV_SAMPLE_FMT_FLTP, error)) {
                return false;
            }
        } else {
            logLine("input has no audio stream");
        }

        if (!args.outputFile.empty()) {
            const int rcOpen = avio_open2(
                &outFmt->pb, outputPath.c_str(),
                AVIO_FLAG_WRITE, nullptr, nullptr);
            if (rcOpen < 0) {
                error = "avio_open2 output file: " + fferr(rcOpen);
                return false;
            }
        } else if (outputHandle != INVALID_HANDLE_VALUE) {
            constexpr int kAvioBufferSize = 64 * 1024;
            auto* avioBuffer =
                static_cast<unsigned char*>(av_malloc(kAvioBufferSize));
            if (!avioBuffer) {
                error = "av_malloc custom AVIO buffer failed";
                return false;
            }
            outFmt->pb = avio_alloc_context(
                avioBuffer, kAvioBufferSize, 1, outputHandle,
                nullptr, writeHandleAvio, nullptr);
            if (!outFmt->pb) {
                av_free(avioBuffer);
                error = "avio_alloc_context named pipe failed";
                return false;
            }
            outFmt->flags |= AVFMT_FLAG_CUSTOM_IO;
        } else {
            const int rcOpen = avio_open2(
                &outFmt->pb, "pipe:1", AVIO_FLAG_WRITE,
                nullptr, nullptr);
            if (rcOpen < 0) {
                error = "avio_open2(pipe:1): " + fferr(rcOpen);
                return false;
            }
        }

        AVDictionary* muxOpts = nullptr;
        if (webmAlphaOutput && !args.offlineConvert) {
            av_dict_set(&muxOpts, "live", "1", 0);
            av_dict_set(&muxOpts, "cluster_time_limit", "100", 0);
            av_dict_set(&muxOpts, "cluster_size_limit", "1048576", 0);
        } else if (!args.offlineConvert) {
            av_dict_set(&muxOpts, "muxdelay", "0", 0);
            av_dict_set(&muxOpts, "muxpreload", "0", 0);
            av_dict_set(&muxOpts, "mpegts_flags", "resend_headers", 0);
        } else if (!webmAlphaOutput) {
            av_dict_set(&muxOpts, "movflags", "+faststart", 0);
        }
        rc = avformat_write_header(outFmt, &muxOpts);
        av_dict_free(&muxOpts);
        if (rc < 0) {
            error = "avformat_write_header: " + fferr(rc);
            return false;
        }
        return true;
    }


    double videoSeconds(int64_t pts) const {
        if (pts == AV_NOPTS_VALUE || firstVideoPts == AV_NOPTS_VALUE) return 0.0;
        return static_cast<double>(pts - firstVideoPts) * av_q2d(inVideo->time_base);
    }

    bool paceOrDrop(int64_t pts) {
        if (args.offlineConvert) return false;
        if (pts == AV_NOPTS_VALUE) return false;
        if (firstVideoPts == AV_NOPTS_VALUE) {
            firstVideoPts = pts;
            return false;
        }
        // The cold/warm-up cost of the first RVM frame must not count as
        // playback time. The clock starts only after that frame is complete.
        if (!playbackClockStarted) return false;
        const double src = videoSeconds(pts);
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

    bool writeEncodedPackets(std::string& error) {
        std::unique_ptr<AVPacket, AvPacketCloser> pkt(av_packet_alloc());
        if (!pkt) { error = "av_packet_alloc encode failed"; return false; }
        for (;;) {
            int rc = avcodec_receive_packet(enc, pkt.get());
            if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return true;
            if (rc < 0) { error = "avcodec_receive_packet encoder: " + fferr(rc); return false; }
            ++encodedPackets;
            if (encodedPackets == 1) {
                std::ostringstream e;
                e << "first-frame stage=video_packet_ready encoder=" << avcodec_get_name(enc->codec_id)
                  << " bytes=" << pkt->size
                  << " key=" << ((pkt->flags & AV_PKT_FLAG_KEY) ? 1 : 0);
                logLine(e.str());
            }
            const int64_t packetVideoUs = pkt->pts == AV_NOPTS_VALUE
                ? currentVideoSourceUs
                : av_rescale_q(pkt->pts, enc->time_base, AV_TIME_BASE_Q);
            av_packet_rescale_ts(pkt.get(), enc->time_base, outVideo->time_base);
            if (pkt->dts != AV_NOPTS_VALUE) {
                if (lastMuxVideoDts != AV_NOPTS_VALUE && pkt->dts <= lastMuxVideoDts) {
                    const int64_t oldDts = pkt->dts;
                    pkt->dts = lastMuxVideoDts + 1;
                    if (pkt->pts != AV_NOPTS_VALUE && pkt->pts < pkt->dts) {
                        pkt->pts = pkt->dts;
                    }
                    logLine("corrected non-monotonic video DTS old=" +
                            std::to_string(oldDts) + " new=" +
                            std::to_string(pkt->dts));
                }
                lastMuxVideoDts = pkt->dts;
            }
            pkt->stream_index = outVideo->index;
            pkt->pos = -1;
            rc = av_interleaved_write_frame(outFmt, pkt.get());
            av_packet_unref(pkt.get());
            if (rc < 0) { error = "av_interleaved_write_frame video: " + fferr(rc); return false; }
            const bool firstVideoOutput = !videoOutputStarted;
            videoOutputStarted = true;
            if (firstVideoOutputUs == AV_NOPTS_VALUE) firstVideoOutputUs = packetVideoUs;
            latestVideoOutputUs = packetVideoUs;
            if (firstVideoOutput) {
                logLine("A/V sync video master startUs=" +
                        (packetVideoUs == AV_NOPTS_VALUE
                            ? std::string("unknown") : std::to_string(packetVideoUs)));
            }
            if (!drainPendingAudio(error)) return false;
            // stdout is a pipe. Force each low-latency container packet out so
            // the parent can stream MPEG-TS or WebM without muxer buffering.
            avio_flush(outFmt->pb);
            if (encodedPackets == 1) logLine("first-frame stage=container_flushed_to_stdout");
        }
    }


    bool ensureCpuNv12Frame(std::string& error) {
        if (cpuNv12Frame) return true;
        cpuNv12Frame = av_frame_alloc();
        if (!cpuNv12Frame) {
            error = "av_frame_alloc NV12 staging failed";
            return false;
        }
        cpuNv12Frame->format = AV_PIX_FMT_NV12;
        cpuNv12Frame->width = width;
        cpuNv12Frame->height = height;
        const int rc = av_frame_get_buffer(cpuNv12Frame, 64);
        if (rc < 0) {
            error = "av_frame_get_buffer NV12 staging: " + fferr(rc);
            return false;
        }
        return true;
    }

    bool ensureWebmFrame(std::string& error) {
        if (webmCpuFrame) return true;
        webmCpuFrame = av_frame_alloc();
        if (!webmCpuFrame) {
            error = "av_frame_alloc YUVA420P failed";
            return false;
        }
        webmCpuFrame->format = AV_PIX_FMT_YUVA420P;
        webmCpuFrame->width = width;
        webmCpuFrame->height = height;
        const int rc = av_frame_get_buffer(webmCpuFrame, 64);
        if (rc < 0) {
            error = "av_frame_get_buffer YUVA420P: " + fferr(rc);
            return false;
        }
        return true;
    }

    bool ensureLegacyEncodeFrame(std::string& error) {
        if (legacyEncodeFrame) return true;
        legacyEncodeFrame = av_frame_alloc();
        if (!legacyEncodeFrame) {
            error = "av_frame_alloc legacy output failed";
            return false;
        }
        legacyEncodeFrame->format = enc->pix_fmt;
        legacyEncodeFrame->width = width;
        legacyEncodeFrame->height = height;
        const int rc = av_frame_get_buffer(legacyEncodeFrame, 64);
        if (rc < 0) {
            error = "av_frame_get_buffer legacy output: " + fferr(rc);
            return false;
        }
        return true;
    }

    bool transferToCpuIfNeeded(
            AVFrame* input,
            std::unique_ptr<AVFrame, AvFrameCloser>& transferred,
            AVFrame*& work,
            std::string& error) {
        work = input;
        if (input->format != AV_PIX_FMT_D3D11) return true;
        transferred.reset(av_frame_alloc());
        if (!transferred) {
            error = "av_frame_alloc D3D11 transfer failed";
            return false;
        }
        const int rc = av_hwframe_transfer_data(transferred.get(), input, 0);
        if (rc < 0) {
            error = "av_hwframe_transfer_data D3D11->CPU: " + fferr(rc);
            return false;
        }
        av_frame_copy_props(transferred.get(), input);
        work = transferred.get();
        return true;
    }

    bool prepareWebmAlpha(
            AVFrame* input, AVFrame*& encodeFrame,
            double& inferMsThisFrame,
            double& prepareMsThisFrame,
            std::string& error) {
        std::unique_ptr<AVFrame, AvFrameCloser> transferred;
        AVFrame* work = nullptr;
        if (!transferToCpuIfNeeded(input, transferred, work, error)) {
            return false;
        }
        if (!ensureWebmFrame(error)) return false;
        if (av_frame_make_writable(webmCpuFrame) < 0) {
            error = "av_frame_make_writable YUVA420P failed";
            return false;
        }

        rgb.resize(
            static_cast<size_t>(width) *
            static_cast<size_t>(height) * 3u);
        nchw.resize(
            static_cast<size_t>(width) *
            static_cast<size_t>(height) * 3u);

        const auto prepStart = std::chrono::steady_clock::now();
        toRgb = sws_getCachedContext(
            toRgb,
            work->width, work->height,
            static_cast<AVPixelFormat>(work->format),
            width, height, AV_PIX_FMT_RGB24,
            SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!toRgb) {
            error = "sws_getCachedContext decode->RGB failed";
            return false;
        }
        uint8_t* rgbData[4]{rgb.data(), nullptr, nullptr, nullptr};
        int rgbLinesize[4]{width * 3, 0, 0, 0};
        if (sws_scale(
                toRgb,
                work->data, work->linesize,
                0, work->height,
                rgbData, rgbLinesize) != height) {
            error = "sws_scale decode->RGB failed";
            return false;
        }

        const size_t pixels =
            static_cast<size_t>(width) *
            static_cast<size_t>(height);
        for (size_t i = 0; i < pixels; ++i) {
            nchw[i] = rgb[i * 3 + 0] / 255.0f;
            nchw[pixels + i] = rgb[i * 3 + 1] / 255.0f;
            nchw[pixels * 2 + i] = rgb[i * 3 + 2] / 255.0f;
        }
        const auto inferStart = std::chrono::steady_clock::now();
        RvmOutput output{};
        if (!rvm->run(nchw.data(), output, error)) return false;
        const auto inferDone = std::chrono::steady_clock::now();
        if (!output.alphaFp32) {
            error = "RVM returned no alpha";
            return false;
        }

        rgbToOutput = sws_getCachedContext(
            rgbToOutput,
            width, height, AV_PIX_FMT_RGB24,
            width, height, AV_PIX_FMT_YUV420P,
            SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!rgbToOutput) {
            error = "sws_getCachedContext RGB->YUVA failed";
            return false;
        }
        uint8_t* yuvData[4]{
            webmCpuFrame->data[0],
            webmCpuFrame->data[1],
            webmCpuFrame->data[2],
            nullptr};
        int yuvLinesize[4]{
            webmCpuFrame->linesize[0],
            webmCpuFrame->linesize[1],
            webmCpuFrame->linesize[2],
            0};
        if (sws_scale(
                rgbToOutput,
                rgbData, rgbLinesize,
                0, height,
                yuvData, yuvLinesize) != height) {
            error = "sws_scale RGB->YUV420P failed";
            return false;
        }

        for (int y = 0; y < height; ++y) {
            uint8_t* alphaRow =
                webmCpuFrame->data[3] +
                static_cast<ptrdiff_t>(y) *
                webmCpuFrame->linesize[3];
            const float* src =
                output.alphaFp32 +
                static_cast<size_t>(y) * width;
            for (int x = 0; x < width; ++x) {
                alphaRow[x] = static_cast<uint8_t>(
                    std::lround(
                        std::clamp(src[x], 0.0f, 1.0f) * 255.0f));
            }
        }

        const auto prepDone = std::chrono::steady_clock::now();
        inferMsThisFrame =
            std::chrono::duration<double, std::milli>(
                inferDone - inferStart).count();
        prepareMsThisFrame =
            std::chrono::duration<double, std::milli>(
                inferStart - prepStart).count() +
            std::chrono::duration<double, std::milli>(
                prepDone - inferDone).count();
        encodeFrame = webmCpuFrame;
        return true;
    }

    bool prepareCpuLegacy(
            AVFrame* input, AVFrame*& encodeFrame,
            double& inferMsThisFrame,
            double& prepareMsThisFrame,
            std::string& error) {
        std::unique_ptr<AVFrame, AvFrameCloser> transferred;
        AVFrame* work = nullptr;
        if (!transferToCpuIfNeeded(input, transferred, work, error)) {
            return false;
        }
        if (!ensureLegacyEncodeFrame(error)) return false;
        if (av_frame_make_writable(legacyEncodeFrame) < 0) {
            error = "av_frame_make_writable legacy output failed";
            return false;
        }

        rgb.resize(
            static_cast<size_t>(width) *
            static_cast<size_t>(height) * 3u);
        nchw.resize(
            static_cast<size_t>(width) *
            static_cast<size_t>(height) * 3u);

        const auto prepStart = std::chrono::steady_clock::now();
        toRgb = sws_getCachedContext(
            toRgb,
            work->width, work->height,
            static_cast<AVPixelFormat>(work->format),
            width, height, AV_PIX_FMT_RGB24,
            SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!toRgb) {
            error = "sws_getCachedContext decode->RGB failed";
            return false;
        }

        uint8_t* rgbData[4]{rgb.data(), nullptr, nullptr, nullptr};
        int rgbLinesize[4]{width * 3, 0, 0, 0};
        if (sws_scale(
                toRgb,
                work->data, work->linesize,
                0, work->height,
                rgbData, rgbLinesize) != height) {
            error = "sws_scale decode->RGB failed";
            return false;
        }

        const size_t pixels =
            static_cast<size_t>(width) *
            static_cast<size_t>(height);
        for (size_t i = 0; i < pixels; ++i) {
            nchw[i] = rgb[i * 3 + 0] / 255.0f;
            nchw[pixels + i] = rgb[i * 3 + 1] / 255.0f;
            nchw[pixels * 2 + i] = rgb[i * 3 + 2] / 255.0f;
        }

        const auto inferStart = std::chrono::steady_clock::now();
        RvmOutput output{};
        if (!rvm->run(nchw.data(), output, error)) return false;
        const auto inferDone = std::chrono::steady_clock::now();
        if (!output.alphaFp32) {
            error = "RVM returned no alpha";
            return false;
        }

        if (args.outputMode == Args::OutputMode::ChromaKey) {
            constexpr float bgR = 0.35f;
            constexpr float bgG = 1.00f;
            constexpr float bgB = 0.00f;
            for (size_t i = 0; i < pixels; ++i) {
                const float a =
                    std::clamp(output.alphaFp32[i], 0.0f, 1.0f);
                const float inv = 1.0f - a;
                rgb[i * 3 + 0] = static_cast<uint8_t>(std::lround(
                    a * rgb[i * 3 + 0] + inv * (bgR * 255.0f)));
                rgb[i * 3 + 1] = static_cast<uint8_t>(std::lround(
                    a * rgb[i * 3 + 1] + inv * (bgG * 255.0f)));
                rgb[i * 3 + 2] = static_cast<uint8_t>(std::lround(
                    a * rgb[i * 3 + 2] + inv * (bgB * 255.0f)));
            }
        } else {
            auto alphaAt = [&](float u, float v) -> uint8_t {
                const int sx = std::clamp(
                    static_cast<int>(u * width), 0, width - 1);
                const int sy = std::clamp(
                    static_cast<int>(v * height), 0, height - 1);
                const float a = std::clamp(
                    output.alphaFp32[
                        static_cast<size_t>(sy) * width + sx],
                    0.0f, 1.0f);
                return static_cast<uint8_t>(
                    std::lround(a * 255.0f));
            };
            for (int y = 0; y < height; ++y) {
                const float py =
                    (static_cast<float>(y) + 0.5f) / height;
                float srcV = -1.0f;
                if (py < 0.2f) srcV = (0.2f + py) / 0.4f;
                else if (py >= 0.8f) srcV = (py - 0.8f) / 0.4f;
                if (srcV < 0.0f || srcV >= 1.0f) continue;

                for (int x = 0; x < width; ++x) {
                    const float px =
                        (static_cast<float>(x) + 0.5f) / width;
                    float srcU = -1.0f;
                    if (px < 0.1f) srcU = (0.3f + px) / 0.4f;
                    else if (px >= 0.4f && px < 0.6f)
                        srcU = (px - 0.4f) / 0.4f;
                    else if (px >= 0.9f)
                        srcU = (0.2f + px - 0.9f) / 0.4f;
                    if (srcU < 0.0f || srcU >= 1.0f) continue;
                    const uint8_t a = alphaAt(srcU, srcV);
                    const size_t di =
                        (static_cast<size_t>(y) * width + x) * 3u;
                    rgb[di + 0] = a;
                    rgb[di + 1] = a;
                    rgb[di + 2] = a;
                }
            }
        }

        rgbToOutput = sws_getCachedContext(
            rgbToOutput,
            width, height, AV_PIX_FMT_RGB24,
            width, height,
            static_cast<AVPixelFormat>(legacyEncodeFrame->format),
            SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!rgbToOutput) {
            error = "sws_getCachedContext RGB->output failed";
            return false;
        }

        uint8_t* dstData[4]{
            legacyEncodeFrame->data[0],
            legacyEncodeFrame->data[1],
            legacyEncodeFrame->data[2],
            nullptr};
        int dstLinesize[4]{
            legacyEncodeFrame->linesize[0],
            legacyEncodeFrame->linesize[1],
            legacyEncodeFrame->linesize[2],
            0};
        if (sws_scale(
                rgbToOutput,
                rgbData, rgbLinesize,
                0, height,
                dstData, dstLinesize) != height) {
            error = "sws_scale RGB->output failed";
            return false;
        }

        const auto prepDone = std::chrono::steady_clock::now();
        inferMsThisFrame =
            std::chrono::duration<double, std::milli>(
                inferDone - inferStart).count();
        prepareMsThisFrame =
            std::chrono::duration<double, std::milli>(
                inferStart - prepStart).count() +
            std::chrono::duration<double, std::milli>(
                prepDone - inferDone).count();
        encodeFrame = legacyEncodeFrame;
        return true;
    }

    bool prepareDirectMlPacked(
            AVFrame* input, AVFrame*& encodeFrame,
            std::unique_ptr<AVFrame, AvFrameCloser>& hwEncodeFrame,
            double& inferMsThisFrame,
            double& prepareMsThisFrame,
            std::string& error) {
        const RvmGpuCompositeMode mode =
            args.outputMode == Args::OutputMode::ChromaKey
                ? RvmGpuCompositeMode::ChromaKey
                : RvmGpuCompositeMode::AlphaPacked;

        // DirectML realtime fast path: keep decode, RVM/composite, and
        // encoder handoff on the selected GPU. RvmOrt owns D3D11/D3D12
        // synchronization so the normal path does not block the CPU per frame.
        if (encoderGpuDirect &&
            input->format == AV_PIX_FMT_D3D11 &&
            input->data[0]) {
            auto* sourceTexture =
                reinterpret_cast<ID3D11Texture2D*>(input->data[0]);
            D3D11_TEXTURE2D_DESC sourceDesc{};
            sourceTexture->GetDesc(&sourceDesc);
            if (sourceDesc.Format == DXGI_FORMAT_NV12) {
                const int sourceArraySlice =
                    static_cast<int>(
                        reinterpret_cast<intptr_t>(input->data[1]));

                hwEncodeFrame.reset(av_frame_alloc());
                if (!hwEncodeFrame) {
                    error = "av_frame_alloc D3D11 encoder frame failed";
                    return false;
                }
                int rc = av_hwframe_get_buffer(
                    encFrames, hwEncodeFrame.get(), 0);
                if (rc < 0) {
                    error = "av_hwframe_get_buffer D3D11 encoder: " +
                            fferr(rc);
                    return false;
                }

                auto* encoderTexture =
                    reinterpret_cast<ID3D11Texture2D*>(
                        hwEncodeFrame->data[0]);
                if (!encoderTexture) {
                    error = "D3D11 encoder texture is null";
                    return false;
                }
                const int encoderArraySlice =
                    static_cast<int>(
                        reinterpret_cast<intptr_t>(
                            hwEncodeFrame->data[1]));

                const auto inferStart =
                    std::chrono::steady_clock::now();
                if (!rvm->runGpuCompositeNv12(
                        mode,
                        RvmAlphaInputFormat::Nv12,
                        nullptr, 0, nullptr, 0, nullptr, 0,
                        nullptr, 0, nullptr, 0,
                        error,
                        sourceTexture,
                        sourceArraySlice,
                        nullptr,
                        encoderTexture,
                        encoderArraySlice)) {
                    return false;
                }
                const auto inferDone =
                    std::chrono::steady_clock::now();

                inferMsThisFrame =
                    std::chrono::duration<double, std::milli>(
                        inferDone - inferStart).count();
                prepareMsThisFrame = 0.0;
                encodeFrame = hwEncodeFrame.get();
                return true;
            }
        }

        // Same media flow as CUDA fallback: one decoded frame in, one completed
        // encoded frame out. Only the GPU implementation differs.
        std::unique_ptr<AVFrame, AvFrameCloser> transferred;
        AVFrame* work = nullptr;
        if (!transferToCpuIfNeeded(input, transferred, work, error)) {
            return false;
        }

        if ((work->format != AV_PIX_FMT_NV12 &&
             work->format != AV_PIX_FMT_YUV420P) ||
            args.backend != RvmBackend::DirectML) {
            return prepareCpuLegacy(
                work, encodeFrame,
                inferMsThisFrame, prepareMsThisFrame,
                error);
        }

        if (!ensureCpuNv12Frame(error)) return false;
        if (av_frame_make_writable(cpuNv12Frame) < 0) {
            error = "av_frame_make_writable NV12 staging failed";
            return false;
        }

        const RvmAlphaInputFormat inputFormat =
            work->format == AV_PIX_FMT_NV12
                ? RvmAlphaInputFormat::Nv12
                : RvmAlphaInputFormat::Yuv420p;

        const auto inferStart = std::chrono::steady_clock::now();
        if (!rvm->runGpuCompositeNv12(
                mode,
                inputFormat,
                work->data[0], work->linesize[0],
                work->data[1], work->linesize[1],
                work->data[2], work->linesize[2],
                cpuNv12Frame->data[0],
                cpuNv12Frame->linesize[0],
                cpuNv12Frame->data[1],
                cpuNv12Frame->linesize[1],
                error)) {
            return false;
        }
        const auto inferDone = std::chrono::steady_clock::now();

        if (encoderGpuDirect) {
            hwEncodeFrame.reset(av_frame_alloc());
            if (!hwEncodeFrame) {
                error = "av_frame_alloc D3D11 encoder frame failed";
                return false;
            }
            int rc = av_hwframe_get_buffer(
                encFrames, hwEncodeFrame.get(), 0);
            if (rc < 0) {
                error = "av_hwframe_get_buffer D3D11 encoder: " +
                        fferr(rc);
                return false;
            }
            const auto uploadStart =
                std::chrono::steady_clock::now();
            rc = av_hwframe_transfer_data(
                hwEncodeFrame.get(), cpuNv12Frame, 0);
            if (rc < 0) {
                error = "av_hwframe_transfer_data CPU->D3D11: " +
                        fferr(rc);
                return false;
            }
            const auto uploadDone =
                std::chrono::steady_clock::now();
            inferMsThisFrame =
                std::chrono::duration<double, std::milli>(
                    inferDone - inferStart).count();
            prepareMsThisFrame =
                std::chrono::duration<double, std::milli>(
                    uploadDone - uploadStart).count();
            encodeFrame = hwEncodeFrame.get();
            return true;
        }

        if (enc->pix_fmt == AV_PIX_FMT_NV12) {
            inferMsThisFrame =
                std::chrono::duration<double, std::milli>(
                    inferDone - inferStart).count();
            prepareMsThisFrame = 0.0;
            encodeFrame = cpuNv12Frame;
            return true;
        }

        if (!ensureLegacyEncodeFrame(error)) return false;
        if (av_frame_make_writable(legacyEncodeFrame) < 0) {
            error = "av_frame_make_writable legacy output failed";
            return false;
        }
        rgbToOutput = sws_getCachedContext(
            rgbToOutput,
            width, height, AV_PIX_FMT_NV12,
            width, height,
            static_cast<AVPixelFormat>(
                legacyEncodeFrame->format),
            SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!rgbToOutput) {
            error = "sws_getCachedContext NV12->output failed";
            return false;
        }
        const auto convertStart =
            std::chrono::steady_clock::now();
        if (sws_scale(
                rgbToOutput,
                cpuNv12Frame->data,
                cpuNv12Frame->linesize,
                0, height,
                legacyEncodeFrame->data,
                legacyEncodeFrame->linesize) != height) {
            error = "sws_scale NV12->output failed";
            return false;
        }
        const auto convertDone =
            std::chrono::steady_clock::now();
        inferMsThisFrame =
            std::chrono::duration<double, std::milli>(
                inferDone - inferStart).count();
        prepareMsThisFrame =
            std::chrono::duration<double, std::milli>(
                convertDone - convertStart).count();
        encodeFrame = legacyEncodeFrame;
        return true;
    }

    bool processFrame(AVFrame* frame, std::string& error) {
        const int64_t pts =
            frame->best_effort_timestamp != AV_NOPTS_VALUE
                ? frame->best_effort_timestamp
                : frame->pts;

        if (requestedStartTimestampUs > 0 &&
            pts != AV_NOPTS_VALUE) {
            const int64_t frameUs =
                av_rescale_q(
                    pts, inVideo->time_base,
                    AV_TIME_BASE_Q);
            if (frameUs < requestedStartTimestampUs) {
                return true;
            }
        }

        // This is copied from the working CUDA Pipeline.
        if (paceOrDrop(pts)) return true;

        ++decodedFrames;
        const bool trace = firstFrameTrace;
        if (trace) {
            std::ostringstream t;
            t << "first-frame stage=decoded frameFormat="
              << (av_get_pix_fmt_name(
                      static_cast<AVPixelFormat>(frame->format))
                    ? av_get_pix_fmt_name(
                          static_cast<AVPixelFormat>(frame->format))
                    : "unknown")
              << " pts=" << pts;
            logLine(t.str());
        }

        AVFrame* encodeFrame = nullptr;
        std::unique_ptr<AVFrame, AvFrameCloser> hwEncodeFrame;
        double inferMsThisFrame = 0.0;
        double prepareMsThisFrame = 0.0;

        if (args.outputMode ==
            Args::OutputMode::WebmVp9Alpha) {
            if (!prepareWebmAlpha(
                    frame, encodeFrame,
                    inferMsThisFrame,
                    prepareMsThisFrame,
                    error)) {
                return false;
            }
        } else if (args.backend ==
                   RvmBackend::DirectML) {
            if (!prepareDirectMlPacked(
                    frame, encodeFrame,
                    hwEncodeFrame,
                    inferMsThisFrame,
                    prepareMsThisFrame,
                    error)) {
                return false;
            }
        } else {
            if (!prepareCpuLegacy(
                    frame, encodeFrame,
                    inferMsThisFrame,
                    prepareMsThisFrame,
                    error)) {
                return false;
            }
        }

        if (!encodeFrame) {
            error = "video frame was not prepared";
            return false;
        }

        currentVideoSourceUs =
            pts == AV_NOPTS_VALUE
                ? AV_NOPTS_VALUE
                : av_rescale_q(
                    pts, inVideo->time_base,
                    AV_TIME_BASE_Q);

        int64_t encoderPts =
            pts == AV_NOPTS_VALUE
                ? static_cast<int64_t>(processed)
                : av_rescale_q(
                    pts, inVideo->time_base,
                    enc->time_base);

        if (lastSubmittedVideoPts != AV_NOPTS_VALUE &&
            encoderPts <= lastSubmittedVideoPts) {
            const int64_t nominalStep =
                std::max<int64_t>(
                    1,
                    av_rescale_q(
                        1, av_inv_q(frameRate),
                        enc->time_base));
            encoderPts =
                lastSubmittedVideoPts + nominalStep;
        }
        lastSubmittedVideoPts = encoderPts;
        encodeFrame->pts = encoderPts;

        const auto encodeStart =
            std::chrono::steady_clock::now();
        int rc = avcodec_send_frame(enc, encodeFrame);
        if (rc < 0) {
            error = "avcodec_send_frame: " + fferr(rc);
            return false;
        }
        if (!writeEncodedPackets(error)) return false;
        const auto encodeDone =
            std::chrono::steady_clock::now();

        inferMsAccum += inferMsThisFrame;
        packMsAccum += prepareMsThisFrame;
        encodeMsAccum +=
            std::chrono::duration<double, std::milli>(
                encodeDone - encodeStart).count();

        ++processed;
        if (!playbackClockStarted) {
            wallStart = std::chrono::steady_clock::now();
            statsStart = wallStart;
            playbackClockStarted = true;
        }

        if (trace) {
            logLine("first-frame stage=completed");
            firstFrameTrace = false;
        }

        const auto now = std::chrono::steady_clock::now();
        if (processed == 1 ||
            std::chrono::duration<double>(
                now - statsStart).count() >= 2.0) {
            const double total =
                static_cast<double>(processed + dropped);
            const double fpsOut =
                static_cast<double>(processed) /
                std::max(
                    0.001,
                    std::chrono::duration<double>(
                        now - wallStart).count());
            std::ostringstream s;
            s << std::fixed << std::setprecision(2)
              << "realtime fpsOut=" << fpsOut
              << " processed=" << processed
              << " dropped=" << dropped
              << " dropPct="
              << (total > 0.0
                    ? 100.0 *
                      static_cast<double>(dropped) /
                      total
                    : 0.0)
              << " avgRvmMs="
              << (processed
                    ? inferMsAccum /
                      static_cast<double>(processed)
                    : 0.0)
              << " avgPrepareMs="
              << (processed
                    ? packMsAccum /
                      static_cast<double>(processed)
                    : 0.0)
              << " avgEncodeMuxMs="
              << (processed
                    ? encodeMsAccum /
                      static_cast<double>(processed)
                    : 0.0);
            logLine(s.str());
            statsStart = now;
        }

        logConversionProgress();
        return true;
    }


    bool writeAudioEncoderPackets(std::string& error) {
        std::unique_ptr<AVPacket, AvPacketCloser> pkt(av_packet_alloc());
        if (!pkt) { error = "av_packet_alloc encoded audio failed"; return false; }
        for (;;) {
            const int rc = avcodec_receive_packet(audioEnc, pkt.get());
            if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return true;
            if (rc < 0) { error = "avcodec_receive_packet audio: " + fferr(rc); return false; }
            av_packet_rescale_ts(pkt.get(), audioEnc->time_base, outAudio->time_base);
            pkt->stream_index = outAudio->index;
            pkt->pos = -1;
            const int wr = av_interleaved_write_frame(outFmt, pkt.get());
            av_packet_unref(pkt.get());
            if (wr < 0) { error = "av_interleaved_write_frame encoded audio: " + fferr(wr); return false; }
        }
    }

    bool encodeAudioFifo(bool flushRemainder, std::string& error) {
        const int frameSamples = audioEnc->frame_size > 0 ? audioEnc->frame_size : 1024;
        while (av_audio_fifo_size(audioFifo) >= frameSamples ||
               (flushRemainder && av_audio_fifo_size(audioFifo) > 0)) {
            const int available = av_audio_fifo_size(audioFifo);
            const int take = std::min(frameSamples, available);

            std::unique_ptr<AVFrame, AvFrameCloser> frame(av_frame_alloc());
            if (!frame) { error = "av_frame_alloc encoded audio failed"; return false; }
            frame->nb_samples = frameSamples;
            frame->format = audioEnc->sample_fmt;
            frame->sample_rate = audioEnc->sample_rate;
            if (av_channel_layout_copy(&frame->ch_layout, &audioEnc->ch_layout) < 0) {
                error = "av_channel_layout_copy encoded audio failed"; return false;
            }
            int rc = av_frame_get_buffer(frame.get(), 0);
            if (rc < 0) { error = "av_frame_get_buffer encoded audio: " + fferr(rc); return false; }

            const int read = av_audio_fifo_read(
                audioFifo, reinterpret_cast<void**>(frame->data), take);
            if (read != take) { error = "av_audio_fifo_read returned " + std::to_string(read); return false; }

            if (take < frameSamples) {
                av_samples_set_silence(
                    frame->data, take, frameSamples - take,
                    audioEnc->ch_layout.nb_channels, audioEnc->sample_fmt);
            }

            if (audioNextPts == AV_NOPTS_VALUE) audioNextPts = 0;
            frame->pts = audioNextPts;
            audioNextPts += frameSamples;

            rc = avcodec_send_frame(audioEnc, frame.get());
            if (rc < 0) { error = "avcodec_send_frame audio encoder: " + fferr(rc); return false; }
            if (!writeAudioEncoderPackets(error)) return false;
        }
        return true;
    }

    bool consumeDecodedAudioFrame(AVFrame* frame, std::string& error) {
        const int64_t srcPts =
            frame->best_effort_timestamp != AV_NOPTS_VALUE
                ? frame->best_effort_timestamp : frame->pts;

        if (requestedStartTimestampUs > 0 && srcPts != AV_NOPTS_VALUE) {
            const int64_t frameUs = av_rescale_q(srcPts, inAudio->time_base, AV_TIME_BASE_Q);
            if (frameUs < requestedStartTimestampUs) return true;
        }

        if (audioNextPts == AV_NOPTS_VALUE && srcPts != AV_NOPTS_VALUE) {
            audioNextPts = av_rescale_q(srcPts, inAudio->time_base, audioEnc->time_base);
        }

        const int inputRate = frame->sample_rate > 0
            ? frame->sample_rate
            : (audioDec->sample_rate > 0 ? audioDec->sample_rate : 48000);
        const AVSampleFormat inputFormat = frame->format >= 0
            ? static_cast<AVSampleFormat>(frame->format)
            : audioDec->sample_fmt;
        const AVChannelLayout* inputLayout = frame->ch_layout.nb_channels > 0
            ? &frame->ch_layout : &audioDec->ch_layout;

        // Some decoders only reveal their actual sample format on the first
        // decoded frame. Delay SwrContext creation until that information is
        // available so Vorbis, FLAC, PCM, ALAC, DTS, TrueHD and similar codecs
        // all use the same conversion path reliably.
        if (!audioSwr) {
            int rc = swr_alloc_set_opts2(
                &audioSwr,
                &audioEnc->ch_layout, audioEnc->sample_fmt, audioEnc->sample_rate,
                inputLayout, inputFormat, inputRate,
                0, nullptr);
            if (rc < 0 || !audioSwr) {
                error = "swr_alloc_set_opts2 audio resample: " + fferr(rc);
                return false;
            }
            rc = swr_init(audioSwr);
            if (rc < 0) { error = "swr_init audio resample: " + fferr(rc); return false; }
        }

        const int64_t delay = swr_get_delay(audioSwr, inputRate);
        const int outSamples = static_cast<int>(av_rescale_rnd(
            delay + frame->nb_samples,
            audioEnc->sample_rate,
            inputRate,
            AV_ROUND_UP));

        std::unique_ptr<AVFrame, AvFrameCloser> converted(av_frame_alloc());
        if (!converted) { error = "av_frame_alloc resampled audio failed"; return false; }
        converted->nb_samples = std::max(1, outSamples);
        converted->format = audioEnc->sample_fmt;
        converted->sample_rate = audioEnc->sample_rate;
        if (av_channel_layout_copy(&converted->ch_layout, &audioEnc->ch_layout) < 0) {
            error = "av_channel_layout_copy resampled audio failed"; return false;
        }
        int rc = av_frame_get_buffer(converted.get(), 0);
        if (rc < 0) { error = "av_frame_get_buffer resampled audio: " + fferr(rc); return false; }

        const int inputPlaneCount = av_sample_fmt_is_planar(inputFormat)
            ? std::max(1, inputLayout->nb_channels) : 1;
        std::vector<const uint8_t*> inputPlanes(static_cast<size_t>(inputPlaneCount));
        for (size_t i = 0; i < inputPlanes.size(); ++i) {
            inputPlanes[i] = frame->extended_data[i];
        }
        rc = swr_convert(
            audioSwr,
            converted->data, converted->nb_samples,
            inputPlanes.data(), frame->nb_samples);
        if (rc < 0) { error = "swr_convert audio: " + fferr(rc); return false; }
        converted->nb_samples = rc;
        if (rc == 0) return true;

        if (av_audio_fifo_realloc(audioFifo, av_audio_fifo_size(audioFifo) + rc) < 0) {
            error = "av_audio_fifo_realloc failed"; return false;
        }
        const int written = av_audio_fifo_write(
            audioFifo, reinterpret_cast<void**>(converted->data), rc);
        if (written != rc) {
            error = "av_audio_fifo_write returned " + std::to_string(written);
            return false;
        }
        return encodeAudioFifo(false, error);
    }

    bool drainAudioDecoder(std::string& error) {
        std::unique_ptr<AVFrame, AvFrameCloser> frame(av_frame_alloc());
        if (!frame) { error = "av_frame_alloc decoded audio failed"; return false; }
        for (;;) {
            const int rc = avcodec_receive_frame(audioDec, frame.get());
            if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return true;
            if (rc < 0) { error = "avcodec_receive_frame audio: " + fferr(rc); return false; }
            if (!consumeDecodedAudioFrame(frame.get(), error)) return false;
            av_frame_unref(frame.get());
        }
    }

    bool transcodeAudioPacket(AVPacket* pkt, std::string& error) {
        int rc = avcodec_send_packet(audioDec, pkt);
        if (rc < 0 && rc != AVERROR(EAGAIN)) {
            error = "avcodec_send_packet audio: " + fferr(rc);
            return false;
        }
        return drainAudioDecoder(error);
    }

    bool flushAudioTranscode(std::string& error) {
        int rc = avcodec_send_packet(audioDec, nullptr);
        if (rc >= 0) {
            if (!drainAudioDecoder(error)) return false;
        } else if (rc != AVERROR_EOF) {
            error = "flush audio decoder: " + fferr(rc);
            return false;
        }

        if (!encodeAudioFifo(true, error)) return false;

        rc = avcodec_send_frame(audioEnc, nullptr);
        if (rc >= 0 || rc == AVERROR_EOF) {
            if (!writeAudioEncoderPackets(error)) return false;
        } else {
            error = "flush audio encoder: " + fferr(rc);
            return false;
        }
        return true;
    }

    bool copyAudioPacket(AVPacket* pkt, std::string& error) {
        if (!outAudio) return true;
        if (requestedStartTimestampUs > 0 && pkt->pts != AV_NOPTS_VALUE) {
            const int64_t packetUs = av_rescale_q(pkt->pts, inAudio->time_base, AV_TIME_BASE_Q);
            if (packetUs < requestedStartTimestampUs) {
                return true;
            }
        }
        av_packet_rescale_ts(pkt, inAudio->time_base, outAudio->time_base);
        pkt->stream_index = outAudio->index;
        pkt->pos = -1;
        int rc = av_interleaved_write_frame(outFmt, pkt);
        if (rc < 0) { error = "av_interleaved_write_frame audio: " + fferr(rc); return false; }
        return true;
    }

    int64_t audioPacketTimestampUs(const AVPacket* pkt) const {
        if (!pkt || !inAudio) return AV_NOPTS_VALUE;
        const int64_t timestamp = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts;
        if (timestamp == AV_NOPTS_VALUE) return AV_NOPTS_VALUE;
        return av_rescale_q(timestamp, inAudio->time_base, AV_TIME_BASE_Q);
    }

    bool writeAudioPacketNow(AVPacket* pkt, std::string& error) {
        if (audioTranscode) return transcodeAudioPacket(pkt, error);
        return copyAudioPacket(pkt, error);
    }

    bool queueAudioPacket(const AVPacket* pkt, std::string& error) {
        AVPacket* copy = av_packet_clone(pkt);
        if (!copy) {
            error = "av_packet_clone audio failed";
            return false;
        }
        pendingAudioPackets.push_back(copy);
        return drainPendingAudio(error);
    }

    bool drainPendingAudio(std::string& error) {
        if (!videoOutputStarted) return true;

        while (!pendingAudioPackets.empty()) {
            AVPacket* packet = pendingAudioPackets.front();
            const int64_t audioUs = audioPacketTimestampUs(packet);

            // The first output video packet is the A/V start boundary. Audio
            // from before that boundary is never sent, including after seek.
            if (firstVideoOutputUs != AV_NOPTS_VALUE &&
                audioUs != AV_NOPTS_VALUE && audioUs < firstVideoOutputUs) {
                pendingAudioPackets.pop_front();
                av_packet_free(&packet);
                continue;
            }

            // Keep audio behind the latest completed video packet. This makes
            // video the master clock instead of allowing demuxed audio to run
            // several seconds ahead while RVM/NVENC is busy.
            if (latestVideoOutputUs != AV_NOPTS_VALUE &&
                audioUs != AV_NOPTS_VALUE && audioUs > latestVideoOutputUs) {
                break;
            }

            pendingAudioPackets.pop_front();
            const bool ok = writeAudioPacketNow(packet, error);
            av_packet_free(&packet);
            if (!ok) return false;
        }
        return true;
    }

    void discardAudioAfterLastVideo() {
        const size_t count = pendingAudioPackets.size();
        for (AVPacket* packet : pendingAudioPackets) av_packet_free(&packet);
        pendingAudioPackets.clear();
        if (count > 0) {
            logLine("discarded audio packets beyond final video PTS=" +
                    std::to_string(count));
        }
    }

    bool drainDecoder(std::string& error) {
        std::unique_ptr<AVFrame, AvFrameCloser> frame(av_frame_alloc());
        if (!frame) { error = "av_frame_alloc decoder failed"; return false; }
        for (;;) {
            int rc = avcodec_receive_frame(dec, frame.get());
            if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return true;
            if (rc < 0) { error = "avcodec_receive_frame: " + fferr(rc); return false; }
            if (!processFrame(frame.get(), error)) return false;
            av_frame_unref(frame.get());
        }
    }

    bool run(std::string& error) {
        std::unique_ptr<AVPacket, AvPacketCloser> pkt(av_packet_alloc());
        if (!pkt) { error = "av_packet_alloc failed"; return false; }

        if (args.previewOnly) {
            logLine("Standalone preview is not used by the DirectML worker");
        } else if (args.outputMode == Args::OutputMode::WebmVp9Alpha) {
            logLine("GPU/CPU pipeline active: FFmpeg decode -> ONNX Runtime DirectML/CPU RVM -> YUVA420P -> libvpx-vp9 real alpha -> WebM");
        } else if (args.outputMode == Args::OutputMode::ChromaKey) {
            logLine(std::string("GPU pipeline active: FFmpeg D3D11VA -> ONNX Runtime DirectML RVM -> green chroma-key composite -> HEVC encoder -> ") +
                    (args.offlineConvert ? "MP4 file" : "MPEG-TS"));
        } else {
            logLine(std::string("GPU pipeline active: FFmpeg D3D11VA -> ONNX Runtime DirectML RVM -> AlphaPacked -> HEVC encoder -> ") +
                    (args.offlineConvert ? "MP4 file" : "MPEG-TS"));
        }
        {
            std::ostringstream oss;
            oss << "video=" << width << "x" << height << " fps=" << av_q2d(frameRate)
                << " codec=" << avcodec_get_name(inVideo->codecpar->codec_id);
            if (!args.previewOnly) {
                if (args.outputMode == Args::OutputMode::WebmVp9Alpha) {
                    oss << " output=WebM/VP9-Alpha/libvpx "
                        << (args.offlineConvert ? "offline" : "realtime")
                        << " CRF" << args.qp;
                } else if (args.outputMode == Args::OutputMode::ChromaKey) {
                    oss << " output=ChromaKey-Green/HEVC QP" << args.qp;
                } else {
                    oss << " output=HEVC QP" << args.qp;
                }
            }
            oss << " realtimeDrop=" << (args.offlineConvert ? "OFF" : "ON");
            logLine(oss.str());
        }

        for (;;) {
            if (cancelFlag && cancelFlag->load()) {
                logLine("stream cancellation requested");
                return true;
            }
            int rc = av_read_frame(inFmt, pkt.get());
            if (rc == AVERROR_EOF) break;
            if (rc < 0) { error = "av_read_frame: " + fferr(rc); return false; }

            if (pkt->stream_index == videoIndex) {
                ++videoPacketsRead;
                if (videoPacketsRead == 1) {
                    std::ostringstream v;
                    v << "first-frame stage=demux_video_packet bytes=" << pkt->size
                      << " pts=" << pkt->pts << " dts=" << pkt->dts;
                    logLine(v.str());
                }
                rc = avcodec_send_packet(dec, pkt.get());
                av_packet_unref(pkt.get());
                if (rc < 0 && rc != AVERROR(EAGAIN)) { error = "avcodec_send_packet: " + fferr(rc); return false; }
                if (!drainDecoder(error)) return false;
            } else if (!args.previewOnly && audioIndex >= 0 && pkt->stream_index == audioIndex && outAudio) {
                if (!queueAudioPacket(pkt.get(), error)) return false;
                av_packet_unref(pkt.get());
            } else {
                av_packet_unref(pkt.get());
            }
        }

        int rc = avcodec_send_packet(dec, nullptr);
        if (rc < 0 && rc != AVERROR_EOF) {
            error = "avcodec_send_packet flush: " + fferr(rc);
            return false;
        }
        if (rc >= 0 && !drainDecoder(error)) return false;
        if (!args.previewOnly) {
            rc = avcodec_send_frame(enc, nullptr);
            if (rc < 0 && rc != AVERROR_EOF) {
                error = "avcodec_send_frame flush: " + fferr(rc);
                return false;
            }
            if (rc >= 0 && !writeEncodedPackets(error)) return false;
            if (!drainPendingAudio(error)) return false;
            discardAudioAfterLastVideo();
            if (audioTranscode && !flushAudioTranscode(error)) return false;
            rc = av_write_trailer(outFmt);
            if (rc < 0) {
                error = "av_write_trailer: " + fferr(rc);
                return false;
            }
            if (outFmt->pb) avio_flush(outFmt->pb);
            if (args.offlineConvert) logLine("offline container trailer complete");
        }
        logConversionProgress(true);
        return true;
    }

};


int runResident(Args base) {
    Pipeline warm;
    warm.args = base;

    std::string error;
    if (!warm.initInput(error) ||
        !warm.initD3d11Decoder(error) ||
        !warm.initRvm(error)) {
        logLine("RESIDENT_ERROR init: " + error);
        return 20;
    }

    // Warm the same long-lived RVM session once, then reset recurrent state.
    std::vector<float> warmInput(
        static_cast<size_t>(warm.width) *
        static_cast<size_t>(warm.height) * 3u,
        0.0f);
    RvmOutput warmOutput{};
    logLine("resident RVM warmup start");
    if (!warm.rvm->run(
            warmInput.data(),
            warmOutput,
            error)) {
        logLine("RESIDENT_ERROR warmup: " + error);
        return 22;
    }
    if (!warm.rvm->resetState(error)) {
        logLine("RESIDENT_ERROR reset after warmup: " + error);
        return 23;
    }
    logLine("resident RVM warmup complete");

    std::unique_ptr<RvmOrt> residentRvm =
        std::move(warm.ownedRvm);
    AVBufferRef* residentD3d11Device =
        warm.d3d11Device;
    warm.rvm = nullptr;
    warm.d3d11Device = nullptr;
    warm.cleanup();

    logLine("RESIDENT_READY");

    std::atomic<bool> cancelStream{false};
    std::thread streamThread;
    uint64_t activeRequestId = 0;

    auto stopCurrentStream = [&]() {
        cancelStream.store(true);
        if (streamThread.joinable()) {
            streamThread.join();
        }
        cancelStream.store(false);
    };

    std::string command;
    while (std::getline(std::cin, command)) {
        if (!command.empty() &&
            command.back() == '\r') {
            command.pop_back();
        }

        if (command == "QUIT") {
            stopCurrentStream();
            break;
        }

        if (command.rfind("CANCEL ", 0) == 0) {
            char* end = nullptr;
            const unsigned long long requestId =
                std::strtoull(
                    command.c_str() + 7,
                    &end, 10);
            if (end != command.c_str() + 7 &&
                requestId == activeRequestId) {
                stopCurrentStream();
                activeRequestId = 0;
            }
            continue;
        }

        std::istringstream parser(command);
        std::string verb;
        unsigned long long requestId = 0;
        long long requestedMs = 0;
        std::string outputMode;
        std::string pipeName;
        std::string inputHex;
        parser >> verb >> requestId >>
            requestedMs >> outputMode >>
            pipeName >> inputHex;

        if (verb != "RUN" ||
            requestId == 0 ||
            pipeName.empty() ||
            (outputMode != "alpha-packed" &&
             outputMode != "webm-alpha" &&
             outputMode != "chroma-key")) {
            logLine(
                "RESIDENT_ERROR unknown command=" +
                command);
            continue;
        }

        std::filesystem::path streamInput =
            base.input;
        if (!inputHex.empty()) {
            std::string inputUtf8;
            if (!decodeHex(inputHex, inputUtf8)) {
                logLine(
                    "RESIDENT_ERROR invalid input path encoding");
                continue;
            }
            streamInput =
                std::filesystem::u8path(inputUtf8);
            if (!std::filesystem::is_regular_file(
                    streamInput)) {
                logLine(
                    "RESIDENT_ERROR input not found: " +
                    inputUtf8);
                continue;
            }
        }

        stopCurrentStream();
        activeRequestId = requestId;

        const int64_t startMs =
            std::max<long long>(
                0, requestedMs);

        streamThread = std::thread(
            [&, startMs, outputMode,
             pipeName, streamInput,
             requestId]() {
                HANDLE output =
                    INVALID_HANDLE_VALUE;

                const auto connectBegin =
                    std::chrono::steady_clock::now();

                while (!cancelStream.load()) {
                    output = CreateFileA(
                        pipeName.c_str(),
                        GENERIC_WRITE,
                        0,
                        nullptr,
                        OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL,
                        nullptr);

                    if (output != INVALID_HANDLE_VALUE) {
                        break;
                    }

                    const DWORD win32 =
                        GetLastError();

                    if (win32 != ERROR_PIPE_BUSY &&
                        win32 != ERROR_FILE_NOT_FOUND) {
                        logLine(
                            "RESIDENT_STREAM_ERROR open output pipe Win32=" +
                            std::to_string(win32));
                        return;
                    }

                    if (std::chrono::duration_cast<
                            std::chrono::seconds>(
                            std::chrono::steady_clock::now() -
                            connectBegin).count() >= 120) {
                        logLine(
                            "RESIDENT_STREAM_ERROR output pipe timeout");
                        return;
                    }

                    WaitNamedPipeA(
                        pipeName.c_str(), 50);
                }

                if (output == INVALID_HANDLE_VALUE) {
                    return;
                }

                int streamCode = 0;
                {
                    Pipeline p;
                    p.args = base;
                    p.args.input = streamInput;
                    p.args.resident = false;
                    p.args.previewOnly = false;
                    p.args.startMs = startMs;

                    if (outputMode == "webm-alpha") {
                        p.args.outputMode =
                            Args::OutputMode::WebmVp9Alpha;
                    } else if (
                        outputMode == "chroma-key") {
                        p.args.outputMode =
                            Args::OutputMode::ChromaKey;
                    } else {
                        p.args.outputMode =
                            Args::OutputMode::AlphaPacked;
                    }

                    p.externalRvm =
                        residentRvm.get();
                    p.externalD3d11Device =
                        residentD3d11Device;
                    p.outputHandle = output;
                    p.cancelFlag = &cancelStream;

                    std::string runError;
                    if (!p.initInput(runError) ||
                        !p.initD3d11Decoder(runError) ||
                        !p.initRvm(runError) ||
                        !p.initEncoderAndMuxer(runError) ||
                        !p.initPreview(runError)) {
                        logLine(
                            "ERROR resident stream init: " +
                            runError);
                        streamCode = 10;
                    } else if (!p.run(runError)) {
                        logLine(
                            "ERROR resident stream run: " +
                            runError);
                        streamCode = 11;
                    }
                }

                CloseHandle(output);
                logLine(
                    "RESIDENT_STREAM_DONE id=" +
                    std::to_string(requestId) +
                    " code=" +
                    std::to_string(streamCode));
            });
    }

    stopCurrentStream();
    residentRvm.reset();
    if (residentD3d11Device) {
        av_buffer_unref(
            &residentD3d11Device);
    }
    logLine("RESIDENT_EXIT");
    return 0;
}


} // namespace

struct R800zzDmlResidentSession {
    Args base;
    std::unique_ptr<RvmOrt> rvm;
    AVBufferRef* d3d11Device = nullptr;
};

R800zzDmlResidentSession* R800zzCreateDmlResidentSession(
    const std::filesystem::path& input,
    const std::filesystem::path& model,
    bool directml,
    int device,
    int qp,
    float downsample,
    std::string& error) {
    error.clear();
    if (!std::filesystem::is_regular_file(input)) {
        error = "input not found: " + input.u8string();
        return nullptr;
    }
    if (!std::filesystem::is_regular_file(model)) {
        error = "model not found: " + model.u8string();
        return nullptr;
    }

    Args base;
    base.input = input;
    base.model = model;
    base.qp = qp;
    base.device = device;
    base.downsample = downsample > 0.0f ? downsample : 0.25f;
    base.previewOnly = false;
    base.resident = false;
    base.offlineConvert = false;
    base.noPreview = true;
    base.backend = directml ? RvmBackend::DirectML : RvmBackend::CPU;
    base.outputMode = Args::OutputMode::AlphaPacked;

    Pipeline warm;
    warm.args = base;
    if (!warm.initInput(error) ||
        !warm.initD3d11Decoder(error) ||
        !warm.initRvm(error)) {
        return nullptr;
    }

    std::vector<float> warmInput(
        static_cast<size_t>(warm.width) *
        static_cast<size_t>(warm.height) * 3u,
        0.0f);
    RvmOutput warmOutput{};
    logLine("resident RVM warmup start");
    if (!warm.rvm->run(warmInput.data(), warmOutput, error)) {
        return nullptr;
    }
    if (!warm.rvm->resetState(error)) {
        return nullptr;
    }
    logLine("resident RVM warmup complete");

    auto* session = new (std::nothrow) R800zzDmlResidentSession();
    if (!session) {
        error = "resident session allocation failed";
        return nullptr;
    }

    session->base = warm.args;
    session->rvm = std::move(warm.ownedRvm);
    session->d3d11Device = warm.d3d11Device;
    warm.rvm = nullptr;
    warm.d3d11Device = nullptr;
    warm.cleanup();
    return session;
}

void R800zzDestroyDmlResidentSession(R800zzDmlResidentSession* session) {
    if (!session) return;
    session->rvm.reset();
    if (session->d3d11Device) {
        av_buffer_unref(&session->d3d11Device);
    }
    delete session;
}

int R800zzRunDmlResidentStream(
    R800zzDmlResidentSession* session,
    const std::filesystem::path& input,
    int64_t start_ms,
    const std::string& output_mode,
    HANDLE output,
    std::atomic<bool>* cancel_flag,
    std::string& error) {
    error.clear();
    if (!session || !session->rvm) {
        error = "resident DirectML/CPU session is not initialized";
        return 10;
    }
    if (!std::filesystem::is_regular_file(input)) {
        error = "input not found: " + input.u8string();
        return 10;
    }

    Pipeline p;
    p.args = session->base;
    p.args.input = input;
    p.args.resident = false;
    p.args.previewOnly = false;
    p.args.startMs = std::max<int64_t>(0, start_ms);
    if (output_mode == "webm-alpha") {
        p.args.outputMode = Args::OutputMode::WebmVp9Alpha;
    } else if (output_mode == "chroma-key") {
        p.args.outputMode = Args::OutputMode::ChromaKey;
    } else if (output_mode == "alpha-packed") {
        p.args.outputMode = Args::OutputMode::AlphaPacked;
    } else {
        error = "unknown output mode: " + output_mode;
        return 10;
    }
    p.externalRvm = session->rvm.get();
    p.externalD3d11Device = session->d3d11Device;
    p.outputHandle = output;
    p.cancelFlag = cancel_flag;

    if (!p.initInput(error) ||
        !p.initD3d11Decoder(error) ||
        !p.initRvm(error) ||
        !p.initEncoderAndMuxer(error) ||
        !p.initPreview(error)) {
        return 10;
    }
    if (!p.run(error)) {
        return 11;
    }
    return 0;
}

static int runDmlBackendWide(int argc, wchar_t** argv) {

    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
    av_log_set_level(AV_LOG_ERROR);
    avformat_network_init();

    Args args;
    if (!parseArgs(argc, argv, args)) {
        usage();
        return 2;
    }

    if (args.listDevices) {
        try {
            const auto adapters = enumerateAdapters();
            for (const auto& a : adapters) {
                std::cerr
                    << "[" << a.index << "] "
                    << wideToUtf8(a.description)
                    << " vendor="
                    << vendorName(a.vendorId)
                    << (a.software ? " SOFTWARE" : "")
                    << "\n";
            }
            return 0;
        } catch (const std::exception& e) {
            std::cerr
                << "ERROR: " << e.what()
                << "\n";
            return 10;
        }
    }

    if (!std::filesystem::is_regular_file(
            args.input)) {
        logLine(
            "ERROR input not found: " +
            args.input.u8string());
        return 3;
    }

    if (!std::filesystem::is_regular_file(
            args.model)) {
        logLine(
            "ERROR model not found: " +
            args.model.u8string());
        return 4;
    }

    std::filesystem::path finalOutput;
    std::filesystem::path partialOutput;
    if (args.offlineConvert) {
        if (args.outputFile.empty() ||
            args.outputFile == args.input) {
            logLine(
                "ERROR conversion output must be different from input");
            return 5;
        }
        finalOutput = args.outputFile;
        partialOutput =
            partialOutputPath(finalOutput);
        removePartialOutput(partialOutput);
        args.outputFile = partialOutput;
        logLine(
            "offline temporary output=" +
            partialOutput.u8string());
    }

    if (args.resident) {
        return runResident(args);
    }

    std::string error;
    int exitCode = 0;
    {
        Pipeline p;
        p.args = args;
        if (!p.initInput(error) ||
            !p.initD3d11Decoder(error) ||
            !p.initRvm(error) ||
            !p.initEncoderAndMuxer(error) ||
            !p.initPreview(error)) {
            logLine("ERROR init: " + error);
            exitCode = 10;
        } else if (!p.run(error)) {
            logLine("ERROR run: " + error);
            exitCode = 11;
        }
    }

    if (exitCode != 0) {
        removePartialOutput(partialOutput);
        return exitCode;
    }

    if (args.offlineConvert &&
        !commitPartialOutput(
            partialOutput,
            finalOutput,
            error)) {
        logLine(
            "ERROR conversion finalize: " +
            error);
        removePartialOutput(partialOutput);
        return 13;
    }

    logLine("completed");
    return 0;
}


int RunR800zzDmlBackend(int argc, char** argv) {
    std::vector<std::wstring> wideArgs;
    wideArgs.reserve(static_cast<size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        const char* src = argv[i] ? argv[i] : "";
        UINT codePage = CP_UTF8;
        DWORD flags = MB_ERR_INVALID_CHARS;
        int count = MultiByteToWideChar(codePage, flags, src, -1, nullptr, 0);
        if (count <= 0) {
            codePage = CP_ACP;
            flags = 0;
            count = MultiByteToWideChar(codePage, flags, src, -1, nullptr, 0);
        }
        if (count <= 0) return 2;
        std::wstring value(static_cast<size_t>(count), L'\0');
        if (MultiByteToWideChar(codePage, flags, src, -1, value.data(), count) <= 0) {
            return 2;
        }
        if (!value.empty() && value.back() == L'\0') value.pop_back();
        wideArgs.emplace_back(std::move(value));
    }
    std::vector<wchar_t*> wideArgv;
    wideArgv.reserve(wideArgs.size());
    for (auto& value : wideArgs) wideArgv.push_back(value.data());
    return runDmlBackendWide(argc, wideArgv.data());
}
