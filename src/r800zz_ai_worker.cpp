#include "cuda_kernels.h"
#include "cpu_converter.h"
#include "rvm_ort.h"
#include "simple_video_player.h"

#include <windows.h>
#include <fcntl.h>
#include <io.h>

#include <cuda_runtime.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/time.h>
#include <libswresample/swresample.h>
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
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr float kPackScale = 0.4f;
constexpr double kDropLagSeconds = 0.120;

std::string fferr(int code) {
    char buf[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, buf, sizeof(buf));
    return buf;
}

void logLine(const std::string& s) {
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

AVPixelFormat chooseCudaFormat(AVCodecContext*, const AVPixelFormat* formats) {
    for (const AVPixelFormat* p = formats; *p != AV_PIX_FMT_NONE; ++p) {
        if (*p == AV_PIX_FMT_CUDA) return *p;
    }
    return AV_PIX_FMT_NONE;
}

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string wideToUtf8(const std::wstring& s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
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

class PreviewWindow {
public:
    ~PreviewWindow() { destroy(); }

    bool create(int videoWidth, int videoHeight, std::string& error) {
        if (videoWidth <= 0 || videoHeight <= 0) return true;

        const double scale = std::min(1.0, std::min(960.0 / static_cast<double>(videoWidth),
                                                   540.0 / static_cast<double>(videoHeight)));
        width_ = std::max(1, static_cast<int>(std::lround(videoWidth * scale)));
        height_ = std::max(1, static_cast<int>(std::lround(videoHeight * scale)));
        bytes_ = static_cast<size_t>(width_) * static_cast<size_t>(height_) * 4u;

        if (cudaMalloc(&deviceBgra_, bytes_) != cudaSuccess) {
            error = "cudaMalloc preview buffer failed";
            return false;
        }
        if (cudaHostAlloc(reinterpret_cast<void**>(&hostBgra_), bytes_, cudaHostAllocDefault) != cudaSuccess) {
            error = "cudaHostAlloc preview buffer failed";
            return false;
        }

        HINSTANCE instance = GetModuleHandleW(nullptr);
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &PreviewWindow::WndProc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.lpszClassName = L"R800ZZAiOutputPreviewWindow";
        RegisterClassExW(&wc);

        RECT rect{0, 0, width_, height_};
        AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
        hwnd_ = CreateWindowExW(
            0, wc.lpszClassName, L"R800ZZ AI Output Preview",
            WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
            rect.right - rect.left, rect.bottom - rect.top,
            nullptr, nullptr, instance, this);
        if (!hwnd_) {
            error = "CreateWindowExW preview failed Win32=" + std::to_string(GetLastError());
            return false;
        }

        bmi_.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi_.bmiHeader.biWidth = width_;
        bmi_.bmiHeader.biHeight = -height_;
        bmi_.bmiHeader.biPlanes = 1;
        bmi_.bmiHeader.biBitCount = 32;
        bmi_.bmiHeader.biCompression = BI_RGB;
        ShowWindow(hwnd_, SW_SHOW);
        UpdateWindow(hwnd_);
        return true;
    }

    bool update(const R800ZZGpuFrame& frame, cudaStream_t stream, std::string& error) {
        pumpMessages();
        if (!hwnd_ || closed_) return true;

        cudaError_t ce = r800zzNv12ToBgraPreview(frame, deviceBgra_, width_, height_, stream);
        if (ce != cudaSuccess) {
            error = std::string("preview conversion: ") + cudaGetErrorString(ce);
            return false;
        }
        ce = cudaMemcpyAsync(hostBgra_, deviceBgra_, bytes_, cudaMemcpyDeviceToHost, stream);
        if (ce != cudaSuccess) {
            error = std::string("preview copy: ") + cudaGetErrorString(ce);
            return false;
        }
        ce = cudaStreamSynchronize(stream);
        if (ce != cudaSuccess) {
            error = std::string("preview sync: ") + cudaGetErrorString(ce);
            return false;
        }

        InvalidateRect(hwnd_, nullptr, FALSE);
        UpdateWindow(hwnd_);
        pumpMessages();
        return true;
    }

    void pumpMessages() {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        PreviewWindow* self = reinterpret_cast<PreviewWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = static_cast<PreviewWindow*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(hwnd, message, wParam, lParam);

        switch (message) {
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd, &ps);
            if (self->hostBgra_) {
                RECT client{};
                GetClientRect(hwnd, &client);
                SetStretchBltMode(dc, HALFTONE);
                StretchDIBits(dc, 0, 0, client.right, client.bottom,
                              0, 0, self->width_, self->height_,
                              self->hostBgra_, &self->bmi_, DIB_RGB_COLORS, SRCCOPY);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            self->hwnd_ = nullptr;
            self->closed_ = true;
            return 0;
        default:
            return DefWindowProcW(hwnd, message, wParam, lParam);
        }
    }

    void destroy() {
        if (hwnd_) {
            DestroyWindow(hwnd_);
            hwnd_ = nullptr;
        }
        if (hostBgra_) {
            cudaFreeHost(hostBgra_);
            hostBgra_ = nullptr;
        }
        if (deviceBgra_) {
            cudaFree(deviceBgra_);
            deviceBgra_ = nullptr;
        }
    }

    HWND hwnd_ = nullptr;
    uint8_t* deviceBgra_ = nullptr;
    uint8_t* hostBgra_ = nullptr;
    size_t bytes_ = 0;
    int width_ = 0;
    int height_ = 0;
    bool closed_ = false;
    BITMAPINFO bmi_{};
};

struct Args {
    enum class OutputMode {
        AlphaPacked,
        WebmVp9Alpha,
        ChromaKey,
    };

    std::filesystem::path input;
    std::filesystem::path model;
    int qp = 20;
    int device = 0;
    float downsample = 0.0f;
    int64_t startMs = 0;
    bool previewOnly = false;
    bool resident = false;
    bool offlineConvert = false;
    bool noPreview = false;
    bool cpu = false;
    bool videoPlayer = false;
    int playerLanguage = 0;
    std::filesystem::path outputFile;
    OutputMode outputMode = OutputMode::AlphaPacked;
};

int probeGpuSupport(const std::filesystem::path& model, int device) {
    if (!std::filesystem::is_regular_file(model)) {
        logLine("PROBE_ERROR RVM model not found: " + model.u8string());
        return 30;
    }

    const wchar_t* dependencyDlls[] = {
        L"cudart64_12.dll",
        L"cublas64_12.dll",
        L"cublasLt64_12.dll",
        L"cudnn64_9.dll"
    };
    for (const wchar_t* dll : dependencyDlls) {
        HMODULE module = LoadLibraryW(dll);
        if (!module) {
            logLine("PROBE_ERROR required DLL could not be loaded: " +
                    wideToUtf8(dll) + " Win32=" + std::to_string(GetLastError()));
            return 31;
        }
        FreeLibrary(module);
    }

    HMODULE cudnnModule = LoadLibraryW(L"cudnn64_9.dll");
    using CudnnGetVersionFn = size_t (__cdecl*)();
    auto cudnnGetVersionFn = cudnnModule
        ? reinterpret_cast<CudnnGetVersionFn>(
              GetProcAddress(cudnnModule, "cudnnGetVersion"))
        : nullptr;
    if (!cudnnGetVersionFn) {
        if (cudnnModule) FreeLibrary(cudnnModule);
        logLine("PROBE_ERROR cuDNN version could not be read");
        return 32;
    }
    const size_t cudnnVersion = cudnnGetVersionFn();
    FreeLibrary(cudnnModule);
    if (cudnnVersion < 90700) {
        logLine("PROBE_ERROR cuDNN is too old: version=" +
                std::to_string(cudnnVersion) + " requires >=90700");
        return 33;
    }

    int deviceCount = 0;
    cudaError_t ce = cudaGetDeviceCount(&deviceCount);
    if (ce != cudaSuccess || deviceCount <= 0) {
        logLine("PROBE_ERROR NVIDIA CUDA device not found: " +
                std::string(cudaGetErrorString(ce)));
        return 34;
    }
    if (device < 0 || device >= deviceCount) {
        logLine("PROBE_ERROR requested CUDA device is unavailable: " +
                std::to_string(device));
        return 35;
    }

    AVBufferRef* cudaDevice = nullptr;
    AVBufferRef* encoderFrames = nullptr;
    AVCodecContext* encoderContext = nullptr;
    cudaStream_t stream = nullptr;
    auto cleanup = [&]() {
        if (encoderContext) avcodec_free_context(&encoderContext);
        if (encoderFrames) av_buffer_unref(&encoderFrames);
        if (stream) cudaStreamDestroy(stream);
        if (cudaDevice) av_buffer_unref(&cudaDevice);
    };
    auto fail = [&](int code, const std::string& message) {
        logLine("PROBE_ERROR " + message);
        cleanup();
        return code;
    };

    // FFmpeg must establish the primary CUDA context before CUDA/ORT uses it.
    AVDictionary* deviceOptions = nullptr;
    av_dict_set(&deviceOptions, "primary_ctx", "1", 0);
    const std::string deviceText = std::to_string(device);
    int rc = av_hwdevice_ctx_create(
        &cudaDevice, AV_HWDEVICE_TYPE_CUDA, deviceText.c_str(), deviceOptions, 0);
    av_dict_free(&deviceOptions);
    if (rc < 0) {
        return fail(36, "FFmpeg CUDA device initialization failed: " + fferr(rc));
    }
    ce = cudaSetDevice(device);
    if (ce != cudaSuccess) {
        return fail(37, "cudaSetDevice failed: " + std::string(cudaGetErrorString(ce)));
    }
    cudaDeviceProp properties{};
    ce = cudaGetDeviceProperties(&properties, device);
    if (ce != cudaSuccess) {
        return fail(38, "cudaGetDeviceProperties failed: " +
                        std::string(cudaGetErrorString(ce)));
    }
    ce = cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking);
    if (ce != cudaSuccess) {
        return fail(39, "CUDA stream creation failed: " +
                        std::string(cudaGetErrorString(ce)));
    }

    constexpr int probeWidth = 256;
    constexpr int probeHeight = 256;
    int rvmFailureCode = 0;
    std::string rvmFailure;
    {
        RvmOrtGpu rvm;
        std::string error;
        if (!rvm.initialize(model.wstring(), probeWidth, probeHeight, 0.25f,
                            device, stream, error)) {
            rvmFailureCode = 40;
            rvmFailure = "ONNX Runtime CUDA EP/RVM initialization failed: " + error;
        } else {
            void* input = nullptr;
            const size_t inputBytes = static_cast<size_t>(probeWidth) *
                                      static_cast<size_t>(probeHeight) * 3u *
                                      sizeof(uint16_t);
            ce = cudaMalloc(&input, inputBytes);
            if (ce != cudaSuccess) {
                rvmFailureCode = 41;
                rvmFailure = "RVM probe input allocation failed: " +
                    std::string(cudaGetErrorString(ce));
            } else {
                ce = cudaMemsetAsync(input, 0, inputBytes, stream);
                if (ce != cudaSuccess) {
                    rvmFailureCode = 42;
                    rvmFailure = "RVM probe input initialization failed: " +
                        std::string(cudaGetErrorString(ce));
                } else {
                    RvmGpuOutput output{};
                    if (!rvm.run(input, stream, output, error)) {
                        rvmFailureCode = 43;
                        rvmFailure = "ONNX Runtime CUDA EP/RVM inference failed: " + error;
                    } else {
                        ce = cudaStreamSynchronize(stream);
                        if (ce != cudaSuccess) {
                            rvmFailureCode = 44;
                            rvmFailure = "RVM probe synchronization failed: " +
                                std::string(cudaGetErrorString(ce));
                        }
                    }
                }
                cudaFree(input);
            }
        }
    }
    if (rvmFailureCode != 0) return fail(rvmFailureCode, rvmFailure);

    const AVCodec* encoder = avcodec_find_encoder_by_name("hevc_nvenc");
    if (!encoder) return fail(45, "FFmpeg hevc_nvenc encoder was not found");

    encoderFrames = av_hwframe_ctx_alloc(cudaDevice);
    if (!encoderFrames) return fail(46, "NVENC CUDA frame pool allocation failed");
    auto* framesContext = reinterpret_cast<AVHWFramesContext*>(encoderFrames->data);
    framesContext->format = AV_PIX_FMT_CUDA;
    framesContext->sw_format = AV_PIX_FMT_NV12;
    framesContext->width = 640;
    framesContext->height = 360;
    framesContext->initial_pool_size = 2;
    rc = av_hwframe_ctx_init(encoderFrames);
    if (rc < 0) return fail(47, "NVENC CUDA frame pool initialization failed: " + fferr(rc));

    encoderContext = avcodec_alloc_context3(encoder);
    if (!encoderContext) return fail(48, "NVENC context allocation failed");
    encoderContext->width = 640;
    encoderContext->height = 360;
    encoderContext->pix_fmt = AV_PIX_FMT_CUDA;
    encoderContext->time_base = AVRational{1, 30};
    encoderContext->framerate = AVRational{30, 1};
    encoderContext->gop_size = 30;
    encoderContext->max_b_frames = 0;
    encoderContext->hw_frames_ctx = av_buffer_ref(encoderFrames);
    encoderContext->hw_device_ctx = av_buffer_ref(cudaDevice);
    if (!encoderContext->hw_frames_ctx || !encoderContext->hw_device_ctx) {
        return fail(49, "NVENC hardware context reference failed");
    }
    av_opt_set(encoderContext->priv_data, "preset", "p1", 0);
    av_opt_set(encoderContext->priv_data, "tune", "ull", 0);
    av_opt_set(encoderContext->priv_data, "rc", "constqp", 0);
    av_opt_set_int(encoderContext->priv_data, "qp", 20, 0);
    av_opt_set_int(encoderContext->priv_data, "zerolatency", 1, 0);
    rc = avcodec_open2(encoderContext, encoder, nullptr);
    if (rc < 0) return fail(50, "HEVC/NVENC initialization failed: " + fferr(rc));

    std::ostringstream detail;
    detail << "NVIDIA GPU=\"" << properties.name << "\" cc="
           << properties.major << '.' << properties.minor
           << " CUDA/RVM/NVENC ready";
    logLine("PROBE_OK " + detail.str());
    cleanup();
    return 0;
}

bool parseArgs(int argc, char** argv, Args& out) {
    if (argc < 2) return false;
    out.input = std::filesystem::u8path(argv[1]);
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        auto needValue = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << "Missing value after " << name << "\n";
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "--model") {
            const char* v = needValue("--model"); if (!v) return false; out.model = std::filesystem::u8path(v);
        } else if (a == "--qp") {
            const char* v = needValue("--qp"); if (!v) return false; out.qp = std::atoi(v);
        } else if (a == "--device") {
            const char* v = needValue("--device"); if (!v) return false; out.device = std::atoi(v);
        } else if (a == "--downsample") {
            const char* v = needValue("--downsample"); if (!v) return false; out.downsample = static_cast<float>(std::atof(v));
        } else if (a == "--start-ms") {
            const char* v = needValue("--start-ms"); if (!v) return false;
            out.startMs = std::max<int64_t>(0, std::strtoll(v, nullptr, 10));
        } else if (a == "--preview-only") {
            out.previewOnly = true;
        } else if (a == "--resident") {
            out.resident = true;
        } else if (a == "--convert-output") {
            const char* v = needValue("--convert-output"); if (!v) return false;
            out.outputFile = std::filesystem::u8path(v);
            out.offlineConvert = true;
        } else if (a == "--no-preview") {
            out.noPreview = true;
        } else if (a == "--cpu") {
            out.cpu = true;
        } else if (a == "--video-player") {
            out.videoPlayer = true;
        } else if (a == "--player-language") {
            const char* v = needValue("--player-language"); if (!v) return false;
            out.playerLanguage = std::clamp(std::atoi(v), 0, 6);
        } else if (a == "--output-mode") {
            const char* v = needValue("--output-mode"); if (!v) return false;
            const std::string mode(v);
            if (mode == "webm-alpha") out.outputMode = Args::OutputMode::WebmVp9Alpha;
            else if (mode == "alpha-packed") out.outputMode = Args::OutputMode::AlphaPacked;
            else if (mode == "chroma-key") out.outputMode = Args::OutputMode::ChromaKey;
            else {
                std::cerr << "Unknown --output-mode: " << mode << "\n";
                return false;
            }
        }
    }
    if (out.model.empty()) {
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        out.model = std::filesystem::path(path).parent_path() / L"rvm_mobilenetv3_fp16.onnx";
    }
    return true;
}

bool isTsAudioCopySupported(AVCodecID id) {
    // The R800ZZ player-side MPEG-TS path consumes ADTS AAC. Even though the
    // MPEG-TS muxer can carry several other audio codecs, copying those codecs
    // would produce a stream that this client cannot decode through that path.
    return id == AV_CODEC_ID_AAC;
}

struct Pipeline {
    Args args;
    AVFormatContext* inFmt = nullptr;
    AVCodecContext* dec = nullptr;
    AVCodecContext* enc = nullptr;
    AVCodecContext* audioDec = nullptr;
    AVCodecContext* audioEnc = nullptr;
    AVFormatContext* outFmt = nullptr;
    AVBufferRef* cudaDevice = nullptr;
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
    cudaStream_t cudaStream = nullptr;
    void* rvmInput = nullptr;
    size_t rvmInputBytes = 0;
    void* projectionMap = nullptr;
    size_t projectionMapBytes = 0;
    uint8_t* webmU = nullptr;
    uint8_t* webmV = nullptr;
    uint8_t* webmAlpha = nullptr;
    size_t webmChromaBytes = 0;
    size_t webmAlphaBytes = 0;
    AVFrame* webmCpuFrame = nullptr;
    std::unique_ptr<RvmOrtGpu> ownedRvm;
    RvmOrtGpu* rvm = nullptr;
    RvmOrtGpu* externalRvm = nullptr;
    AVBufferRef* externalCudaDevice = nullptr;
    cudaStream_t externalCudaStream = nullptr;
    bool ownsCudaStream = true;
    HANDLE outputHandle = INVALID_HANDLE_VALUE;
    std::unique_ptr<PreviewWindow> preview;
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
    std::atomic<bool>* cancelFlag = nullptr;
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
        if (cudaDevice) av_buffer_unref(&cudaDevice);
        if (projectionMap) cudaFree(projectionMap);
        projectionMap = nullptr;
        if (webmU) cudaFree(webmU);
        if (webmV) cudaFree(webmV);
        if (webmAlpha) cudaFree(webmAlpha);
        webmU = webmV = webmAlpha = nullptr;
        if (webmCpuFrame) av_frame_free(&webmCpuFrame);
        // Destroy an owned ORT session while its user CUDA stream still exists.
        ownedRvm.reset();
        rvm = nullptr;
        if (rvmInput) cudaFree(rvmInput);
        rvmInput = nullptr;
        if (cudaStream && ownsCudaStream) cudaStreamDestroy(cudaStream);
        cudaStream = nullptr;
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

    bool initCudaDecoder(std::string& error) {
        int rc = 0;
        if (externalCudaDevice) {
            cudaDevice = av_buffer_ref(externalCudaDevice);
            if (!cudaDevice) {
                error = "av_buffer_ref resident CUDA device failed";
                return false;
            }
            logLine("resident FFmpeg CUDA device reused");
        } else {
            AVDictionary* deviceOpts = nullptr;
            av_dict_set(&deviceOpts, "primary_ctx", "1", 0);
            const std::string dev = std::to_string(args.device);
            rc = av_hwdevice_ctx_create(
                &cudaDevice, AV_HWDEVICE_TYPE_CUDA, dev.c_str(), deviceOpts, 0);
            av_dict_free(&deviceOpts);
            if (rc < 0) {
                error = "av_hwdevice_ctx_create(CUDA): " + fferr(rc);
                return false;
            }
        }

        const char* cuvidName = nullptr;
        switch (inVideo->codecpar->codec_id) {
        case AV_CODEC_ID_VP9:  cuvidName = "vp9_cuvid"; break;
        case AV_CODEC_ID_H264: cuvidName = "h264_cuvid"; break;
        case AV_CODEC_ID_HEVC: cuvidName = "hevc_cuvid"; break;
#ifdef AV_CODEC_ID_AV1
        case AV_CODEC_ID_AV1:  cuvidName = "av1_cuvid"; break;
#endif
        default: break;
        }
        const AVCodec* decoder = cuvidName ? avcodec_find_decoder_by_name(cuvidName) : nullptr;
        bool explicitCuvid = decoder != nullptr;
        if (!decoder) decoder = avcodec_find_decoder(inVideo->codecpar->codec_id);
        if (!decoder) { error = "Video decoder not found"; return false; }
        {
            std::ostringstream d;
            d << "decoder selected name=" << decoder->name
              << " mode=" << (explicitCuvid ? "explicit-CUVID/NVDEC" : "generic-hwaccel-fallback");
            logLine(d.str());
        }
        dec = avcodec_alloc_context3(decoder);
        if (!dec) { error = "avcodec_alloc_context3 decoder failed"; return false; }
        rc = avcodec_parameters_to_context(dec, inVideo->codecpar);
        if (rc < 0) { error = "avcodec_parameters_to_context: " + fferr(rc); return false; }
        dec->hw_device_ctx = av_buffer_ref(cudaDevice);
        dec->get_format = chooseCudaFormat;
        dec->pkt_timebase = inVideo->time_base;
        rc = avcodec_open2(dec, decoder, nullptr);
        if (rc < 0) { error = "avcodec_open2 decoder: " + fferr(rc); return false; }
        return true;
    }

    bool initRvm(std::string& error) {
        if (externalRvm && externalCudaStream) {
            cudaStream = externalCudaStream;
            ownsCudaStream = false;
            rvm = externalRvm;
            if (!rvm->resetState(cudaStream, error)) return false;
            logLine("RVM resident session reused");
        } else {
            // ONNX Runtime must load the provider DLLs itself. Only the NVIDIA
            // runtime dependencies are preflighted here.
            const wchar_t* dependencyDlls[] = {
                L"cudart64_12.dll",
                L"cublas64_12.dll",
                L"cublasLt64_12.dll",
                L"cudnn64_9.dll"
            };
            for (const wchar_t* dll : dependencyDlls) {
                HMODULE module = LoadLibraryW(dll);
                if (!module) {
                    const DWORD win32 = GetLastError();
                    error = "native dependency preflight failed dll=" + wideToUtf8(dll) +
                            " Win32=" + std::to_string(win32);
                    return false;
                }
                FreeLibrary(module);
            }
            logLine("native dependency preflight OK (CUDA/cuDNN); ORT will load CUDA EP provider DLLs");

            HMODULE cudnnModule = LoadLibraryW(L"cudnn64_9.dll");
            if (!cudnnModule) {
                error = "LoadLibrary(cudnn64_9.dll) failed Win32=" + std::to_string(GetLastError());
                return false;
            }
            using CudnnGetVersionFn = size_t (__cdecl*)();
            auto cudnnGetVersionFn = reinterpret_cast<CudnnGetVersionFn>(
                GetProcAddress(cudnnModule, "cudnnGetVersion"));
            if (!cudnnGetVersionFn) {
                error = "GetProcAddress(cudnnGetVersion) failed Win32=" + std::to_string(GetLastError());
                FreeLibrary(cudnnModule);
                return false;
            }
            const size_t cudnnVersion = cudnnGetVersionFn();
            {
                std::ostringstream c;
                c << "cuDNN version=" << cudnnVersion
                  << " (requires >= 90700 for Blackwell CC 12.0)";
                logLine(c.str());
            }
            if (cudnnVersion < 90700) {
                error = "cuDNN is too old for Blackwell CC 12.0: version=" +
                        std::to_string(cudnnVersion);
                FreeLibrary(cudnnModule);
                return false;
            }
            FreeLibrary(cudnnModule);

            if (cudaSetDevice(args.device) != cudaSuccess) {
                error = "cudaSetDevice failed";
                return false;
            }
            cudaDeviceProp props{};
            if (cudaGetDeviceProperties(&props, args.device) != cudaSuccess) {
                error = "cudaGetDeviceProperties failed";
                return false;
            }
            {
                std::ostringstream gpu;
                gpu << "CUDA device=" << args.device << " name=\"" << props.name << "\""
                    << " cc=" << props.major << "." << props.minor
                    << " vramMiB="
                    << (static_cast<unsigned long long>(props.totalGlobalMem) /
                        (1024ULL * 1024ULL));
                logLine(gpu.str());
            }
            if (cudaStreamCreateWithFlags(&cudaStream, cudaStreamNonBlocking) != cudaSuccess) {
                error = "cudaStreamCreate failed";
                return false;
            }
            ownsCudaStream = true;

            const float ratio = args.downsample > 0.0f ? args.downsample : 0.25f;
            logLine("RVM CUDA EP init start (official ONNX, FP16, GPU I/O binding)");
            ownedRvm = std::make_unique<RvmOrtGpu>();
            rvm = ownedRvm.get();
            if (!rvm->initialize(args.model.wstring(), width, height, ratio,
                                 args.device, cudaStream, error)) {
                return false;
            }
            std::ostringstream oss;
            oss << "RVM CUDA EP ready model=" << wideToUtf8(args.model.wstring())
                << " downsample=" << std::fixed << std::setprecision(4) << ratio;
            logLine(oss.str());
        }

        const size_t pixels = static_cast<size_t>(width) * static_cast<size_t>(height);
        rvmInputBytes = pixels * 3 * sizeof(uint16_t);
        if (cudaMalloc(&rvmInput, rvmInputBytes) != cudaSuccess) {
            error = "cudaMalloc RVM input failed"; return false;
        }
        projectionMapBytes = pixels * sizeof(float) * 2;
        if (cudaMalloc(&projectionMap, projectionMapBytes) != cudaSuccess) {
            error = "cudaMalloc projection map failed"; return false;
        }
        cudaError_t mapError = r800zzBuildProjectionMap(
            projectionMap, width, height, cudaStream);
        if (mapError != cudaSuccess) {
            error = std::string("projection map kernel failed: ") + cudaGetErrorString(mapError);
            return false;
        }
        const cudaError_t mapSync = cudaStreamSynchronize(cudaStream);
        if (mapSync != cudaSuccess) {
            error = std::string("projection map sync failed: ") + cudaGetErrorString(mapSync);
            return false;
        }
        {
            std::ostringstream mapLog;
            mapLog << "projection map cached on GPU bytes=" << projectionMapBytes;
            logLine(mapLog.str());
        }
        return true;
    }


    bool initPreview(std::string& error) {
        if (args.noPreview) return true;
        preview = std::make_unique<PreviewWindow>();
        if (!preview->create(width, height, error)) return false;
        std::ostringstream oss;
        oss << "preview window ready source=" << width << "x" << height;
        logLine(oss.str());
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

    bool initOutputFramePool(std::string& error) {
        if (encFrames) return true;
        encFrames = av_hwframe_ctx_alloc(cudaDevice);
        if (!encFrames) { error = "av_hwframe_ctx_alloc failed"; return false; }
        auto* fc = reinterpret_cast<AVHWFramesContext*>(encFrames->data);
        fc->format = AV_PIX_FMT_CUDA;
        fc->sw_format = AV_PIX_FMT_NV12;
        fc->width = width;
        fc->height = height;
        fc->initial_pool_size = 10;
        const int rc = av_hwframe_ctx_init(encFrames);
        if (rc < 0) { error = "av_hwframe_ctx_init output pool: " + fferr(rc); return false; }
        return true;
    }

    bool initWebmTransferBuffers(std::string& error) {
        if ((width & 1) != 0 || (height & 1) != 0) {
            error = "WebM VP9 Alpha requires even video dimensions";
            return false;
        }
        const int chromaWidth = width / 2;
        const int chromaHeight = height / 2;
        webmChromaBytes = static_cast<size_t>(chromaWidth) *
                          static_cast<size_t>(chromaHeight);
        webmAlphaBytes = static_cast<size_t>(width) * static_cast<size_t>(height);
        if (cudaMalloc(reinterpret_cast<void**>(&webmU), webmChromaBytes) != cudaSuccess ||
            cudaMalloc(reinterpret_cast<void**>(&webmV), webmChromaBytes) != cudaSuccess ||
            cudaMalloc(reinterpret_cast<void**>(&webmAlpha), webmAlphaBytes) != cudaSuccess) {
            error = "cudaMalloc WebM YUVA staging planes failed";
            return false;
        }

        webmCpuFrame = av_frame_alloc();
        if (!webmCpuFrame) {
            error = "av_frame_alloc WebM YUVA frame failed";
            return false;
        }
        webmCpuFrame->format = AV_PIX_FMT_YUVA420P;
        webmCpuFrame->width = width;
        webmCpuFrame->height = height;
        const int rc = av_frame_get_buffer(webmCpuFrame, 64);
        if (rc < 0) {
            error = "av_frame_get_buffer WebM YUVA: " + fferr(rc);
            return false;
        }
        return true;
    }

    bool initEncoderAndMuxer(std::string& error) {
        const bool webmAlphaOutput =
            args.outputMode == Args::OutputMode::WebmVp9Alpha;
        const char* muxerName = webmAlphaOutput
            ? "webm" : (args.offlineConvert ? "mp4" : "mpegts");
        // The MP4 muxer's +faststart trailer pass reopens AVFormatContext::url
        // to move the moov atom ahead of mdat. Passing only the path to
        // avio_open2() leaves that URL empty and makes trailer finalization
        // fail after all frames have already been encoded.
        const std::string outputPath = args.outputFile.empty()
            ? std::string{} : wideToUtf8(args.outputFile.wstring());
        int rc = avformat_alloc_output_context2(
            &outFmt, nullptr, muxerName,
            outputPath.empty() ? nullptr : outputPath.c_str());
        if (rc < 0 || !outFmt) {
            error = std::string("avformat_alloc_output_context2 ") + muxerName + " failed";
            return false;
        }
        outFmt->flags |= AVFMT_FLAG_FLUSH_PACKETS;

        const AVCodec* encoder = webmAlphaOutput
            ? avcodec_find_encoder_by_name("libvpx-vp9")
            : avcodec_find_encoder_by_name("hevc_nvenc");
        if (!encoder) {
            error = webmAlphaOutput
                ? "libvpx-vp9 encoder not found in linked FFmpeg"
                : "hevc_nvenc encoder not found in linked FFmpeg";
            return false;
        }
        enc = avcodec_alloc_context3(encoder);
        if (!enc) { error = "avcodec_alloc_context3 encoder failed"; return false; }
        enc->width = width;
        enc->height = height;
        enc->pix_fmt = webmAlphaOutput ? AV_PIX_FMT_YUVA420P : AV_PIX_FMT_CUDA;
        enc->time_base = inVideo->time_base;
        if (enc->time_base.num <= 0 || enc->time_base.den <= 0) enc->time_base = av_inv_q(frameRate);
        enc->framerate = frameRate;
        enc->gop_size = std::max(1, static_cast<int>(std::lround(av_q2d(frameRate))));
        enc->max_b_frames = 0;
        if (outFmt->oformat->flags & AVFMT_GLOBALHEADER) {
            enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        }

        if (webmAlphaOutput) {
            enc->thread_count = static_cast<int>(
                std::max(4u, std::thread::hardware_concurrency()));
            enc->bit_rate = 0;
            enc->flags |= AV_CODEC_FLAG_LOW_DELAY | AV_CODEC_FLAG_QSCALE;
            enc->global_quality = std::max(0, std::min(63, args.qp)) * FF_QP2LAMBDA;
            av_opt_set_int(enc->priv_data, "crf", std::max(0, std::min(63, args.qp)), 0);
            av_opt_set(enc->priv_data, "deadline",
                       args.offlineConvert ? "good" : "realtime", 0);
            av_opt_set_int(enc->priv_data, "cpu-used",
                           args.offlineConvert ? 4 : 8, 0);
            av_opt_set_int(enc->priv_data, "lag-in-frames",
                           args.offlineConvert ? 25 : 0, 0);
            av_opt_set_int(enc->priv_data, "auto-alt-ref",
                           args.offlineConvert ? 1 : 0, 0);
            av_opt_set_int(enc->priv_data, "row-mt", 1, 0);
            av_opt_set_int(enc->priv_data, "tile-columns", 2, 0);
            av_opt_set_int(enc->priv_data, "error-resilient", 1, 0);
        } else {
            if (!initOutputFramePool(error)) return false;
            enc->hw_frames_ctx = av_buffer_ref(encFrames);
            enc->hw_device_ctx = av_buffer_ref(cudaDevice);

            av_opt_set(enc->priv_data, "preset", "p1", 0);
            av_opt_set(enc->priv_data, "tune", "ull", 0);
            av_opt_set(enc->priv_data, "rc", "constqp", 0);
            av_opt_set_int(enc->priv_data, "qp", args.qp, 0);
            av_opt_set_int(enc->priv_data, "zerolatency", 1, 0);
            av_opt_set_int(enc->priv_data, "delay", 0, 0);
            av_opt_set_int(enc->priv_data, "bf", 0, 0);
        }

        rc = avcodec_open2(enc, encoder, nullptr);
        if (rc < 0) {
            error = std::string("avcodec_open2 ") + encoder->name + ": " + fferr(rc);
            return false;
        }
        if (webmAlphaOutput && !initWebmTransferBuffers(error)) return false;

        outVideo = avformat_new_stream(outFmt, nullptr);
        if (!outVideo) { error = "avformat_new_stream video failed"; return false; }
        rc = avcodec_parameters_from_context(outVideo->codecpar, enc);
        if (rc < 0) { error = "avcodec_parameters_from_context: " + fferr(rc); return false; }
        outVideo->time_base = enc->time_base;
        outVideo->codecpar->codec_tag = 0;
        if (args.offlineConvert && !webmAlphaOutput) {
            outVideo->codecpar->codec_tag = MKTAG('h', 'v', 'c', '1');
        }
        if (webmAlphaOutput) av_dict_set(&outVideo->metadata, "alpha_mode", "1", 0);

        if (webmAlphaOutput && inAudio) {
            if (!initAudioTranscode(AV_CODEC_ID_OPUS, "libopus",
                                    AV_SAMPLE_FMT_FLT, error)) return false;
        } else if (inAudio && isTsAudioCopySupported(inAudio->codecpar->codec_id)) {
            outAudio = avformat_new_stream(outFmt, nullptr);
            if (!outAudio) { error = "avformat_new_stream audio failed"; return false; }
            rc = avcodec_parameters_copy(outAudio->codecpar, inAudio->codecpar);
            if (rc < 0) { error = "avcodec_parameters_copy audio: " + fferr(rc); return false; }
            outAudio->codecpar->codec_tag = 0;
            outAudio->time_base = inAudio->time_base;
            std::ostringstream oss;
            oss << "audio copy codec=" << avcodec_get_name(inAudio->codecpar->codec_id)
                << " rate=" << inAudio->codecpar->sample_rate;
            logLine(oss.str());
        } else if (inAudio) {
            if (!initAudioTranscode(AV_CODEC_ID_AAC, nullptr,
                                    AV_SAMPLE_FMT_FLTP, error)) return false;
        } else {
            logLine("input has no audio stream");
        }

        if (!args.outputFile.empty()) {
            const int rcOpen = avio_open2(
                &outFmt->pb, outputPath.c_str(), AVIO_FLAG_WRITE, nullptr, nullptr);
            if (rcOpen < 0) {
                error = "avio_open2 output file: " + fferr(rcOpen);
                return false;
            }
        } else if (outputHandle != INVALID_HANDLE_VALUE) {
            constexpr int kAvioBufferSize = 64 * 1024;
            auto* avioBuffer = static_cast<unsigned char*>(av_malloc(kAvioBufferSize));
            if (!avioBuffer) { error = "av_malloc custom AVIO buffer failed"; return false; }
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
            const int rcOpen = avio_open2(&outFmt->pb, "pipe:1", AVIO_FLAG_WRITE, nullptr, nullptr);
            if (rcOpen < 0) { error = "avio_open2(pipe:1): " + fferr(rcOpen); return false; }
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
        if (rc < 0) { error = "avformat_write_header: " + fferr(rc); return false; }
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

    bool processFrame(AVFrame* frame, std::string& error) {
        const int64_t pts = frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp : frame->pts;
        if (requestedStartTimestampUs > 0 && pts != AV_NOPTS_VALUE) {
            const int64_t frameUs = av_rescale_q(pts, inVideo->time_base, AV_TIME_BASE_Q);
            if (frameUs < requestedStartTimestampUs) {
                return true;
            }
        }
        if (paceOrDrop(pts)) return true;

        ++decodedFrames;
        const bool trace = firstFrameTrace;
        if (trace) {
            std::ostringstream t;
            t << "first-frame stage=decoded frameFormat="
              << (av_get_pix_fmt_name(static_cast<AVPixelFormat>(frame->format)) ? av_get_pix_fmt_name(static_cast<AVPixelFormat>(frame->format)) : "unknown")
              << " pts=" << pts;
            logLine(t.str());
        }

        if (frame->format != AV_PIX_FMT_CUDA || !frame->hw_frames_ctx) {
            error = "Decoder did not return AV_PIX_FMT_CUDA frame";
            return false;
        }
        const auto* hwfc = reinterpret_cast<const AVHWFramesContext*>(frame->hw_frames_ctx->data);
        if (hwfc->sw_format != AV_PIX_FMT_NV12) {
            const char* n = av_get_pix_fmt_name(hwfc->sw_format);
            error = std::string("Only CUDA NV12 decode surfaces are supported in the first realtime build; got ") + (n ? n : "unknown");
            return false;
        }

        R800ZZGpuFrame src{};
        src.y = frame->data[0];
        src.uv = frame->data[1];
        src.pitchY = frame->linesize[0];
        src.pitchUV = frame->linesize[1];
        src.width = width;
        src.height = height;

        auto t0 = std::chrono::steady_clock::now();
        if (trace) logLine("first-frame stage=nv12_to_fp16_start");
        cudaError_t ce = r800zzNv12ToRgbHalfNchw(src, rvmInput, cudaStream);
        if (ce != cudaSuccess) { error = std::string("NV12->RVM kernel: ") + cudaGetErrorString(ce); return false; }
        if (trace) logLine("first-frame stage=nv12_to_fp16_enqueued");
        // RVM uses the same CUDA stream, so the conversion kernel can feed it
        // without a CPU-side synchronization.
        RvmGpuOutput rvmOut{};
        if (trace) logLine("first-frame stage=rvm_run_start");
        if (!rvm->run(rvmInput, cudaStream, rvmOut, error)) return false;
        if (trace) logLine("first-frame stage=rvm_run_done");
        auto t1 = std::chrono::steady_clock::now();

        const bool webmAlphaOutput =
            args.outputMode == Args::OutputMode::WebmVp9Alpha && !args.previewOnly;
        std::unique_ptr<AVFrame, AvFrameCloser> packedFrame;
        AVFrame* encodeFrame = nullptr;
        R800ZZGpuFrame previewFrame = src;
        int rc = 0;

        if (webmAlphaOutput) {
            if (trace) logLine("first-frame stage=yuva420p_gpu_prepare_start");
            rc = av_frame_make_writable(webmCpuFrame);
            if (rc < 0) {
                error = "av_frame_make_writable WebM YUVA: " + fferr(rc);
                return false;
            }

            ce = r800zzPrepareYuva420p(
                src, rvmOut.alphaFp16,
                webmU, width / 2,
                webmV, width / 2,
                webmAlpha, width,
                cudaStream);
            if (ce != cudaSuccess) {
                error = std::string("WebM YUVA kernel: ") + cudaGetErrorString(ce);
                return false;
            }

            ce = cudaMemcpy2DAsync(
                webmCpuFrame->data[0], webmCpuFrame->linesize[0],
                src.y, src.pitchY, width, height,
                cudaMemcpyDeviceToHost, cudaStream);
            if (ce == cudaSuccess) ce = cudaMemcpy2DAsync(
                webmCpuFrame->data[1], webmCpuFrame->linesize[1],
                webmU, width / 2, width / 2, height / 2,
                cudaMemcpyDeviceToHost, cudaStream);
            if (ce == cudaSuccess) ce = cudaMemcpy2DAsync(
                webmCpuFrame->data[2], webmCpuFrame->linesize[2],
                webmV, width / 2, width / 2, height / 2,
                cudaMemcpyDeviceToHost, cudaStream);
            if (ce == cudaSuccess) ce = cudaMemcpy2DAsync(
                webmCpuFrame->data[3], webmCpuFrame->linesize[3],
                webmAlpha, width, width, height,
                cudaMemcpyDeviceToHost, cudaStream);
            if (ce != cudaSuccess) {
                error = std::string("WebM GPU->CPU plane copy: ") + cudaGetErrorString(ce);
                return false;
            }
            ce = cudaStreamSynchronize(cudaStream);
            if (ce != cudaSuccess) {
                error = std::string("WebM YUVA transfer sync: ") + cudaGetErrorString(ce);
                return false;
            }
            if (trace) logLine("first-frame stage=yuva420p_gpu_to_cpu_done");
            encodeFrame = webmCpuFrame;
        } else {
            packedFrame.reset(av_frame_alloc());
            if (!packedFrame) { error = "av_frame_alloc output failed"; return false; }
            rc = av_hwframe_get_buffer(encFrames, packedFrame.get(), 0);
            if (rc < 0) { error = "av_hwframe_get_buffer: " + fferr(rc); return false; }

            R800ZZGpuFrameMutable dst{};
            dst.y = packedFrame->data[0];
            dst.uv = packedFrame->data[1];
            dst.pitchY = packedFrame->linesize[0];
            dst.pitchUV = packedFrame->linesize[1];
            dst.width = width;
            dst.height = height;

            const bool chromaKeyOutput =
                args.outputMode == Args::OutputMode::ChromaKey;
            if (trace) logLine(chromaKeyOutput
                ? "first-frame stage=chroma_key_composite_start"
                : "first-frame stage=alpha_pack_start");
            if (chromaKeyOutput) {
                ce = r800zzCompositeChromaKeyNv12(
                    src, rvmOut.alphaFp16, dst, cudaStream);
            } else {
                ce = r800zzPackAlphaNv12(
                    src, rvmOut.alphaFp16, projectionMap, dst,
                    kPackScale, cudaStream);
            }
            if (ce != cudaSuccess) {
                error = std::string(chromaKeyOutput
                    ? "Chroma key composite kernel: "
                    : "Alpha packed kernel: ") + cudaGetErrorString(ce);
                return false;
            }
            ce = cudaStreamSynchronize(cudaStream);
            if (ce != cudaSuccess) {
                error = std::string(chromaKeyOutput
                    ? "Chroma key composite sync: "
                    : "Alpha packed sync: ") + cudaGetErrorString(ce);
                return false;
            }
            if (trace) logLine(chromaKeyOutput
                ? "first-frame stage=chroma_key_composite_done"
                : "first-frame stage=alpha_pack_done");

            previewFrame.y = dst.y;
            previewFrame.uv = dst.uv;
            previewFrame.pitchY = dst.pitchY;
            previewFrame.pitchUV = dst.pitchUV;
            previewFrame.width = dst.width;
            previewFrame.height = dst.height;
            encodeFrame = packedFrame.get();
        }
        auto t2 = std::chrono::steady_clock::now();

        if (preview && !preview->update(previewFrame, cudaStream, error)) return false;
        if (trace) logLine("first-frame stage=preview_updated");

        if (args.previewOnly) {
            ++processed;
            if (!playbackClockStarted) {
                wallStart = std::chrono::steady_clock::now();
                statsStart = wallStart;
                playbackClockStarted = true;
            }
            if (trace) {
                firstFrameTrace = false;
                logLine("first-frame stage=complete preview-only");
            }
            inferMsAccum += std::chrono::duration<double, std::milli>(t1 - t0).count();
            packMsAccum += std::chrono::duration<double, std::milli>(t2 - t1).count();
            const auto now = std::chrono::steady_clock::now();
            const double statsElapsed = std::chrono::duration<double>(now - statsStart).count();
            if (statsElapsed >= 2.0) {
                const double total = static_cast<double>(processed + dropped);
                const double fps = processed / std::max(0.001, std::chrono::duration<double>(now - wallStart).count());
                std::ostringstream oss;
                oss << std::fixed << std::setprecision(2)
                    << "preview fpsOut=" << fps
                    << " processed=" << processed
                    << " dropped=" << dropped
                    << " dropPct=" << (total > 0.0 ? 100.0 * dropped / total : 0.0)
                    << " avgRvmMs=" << (processed ? inferMsAccum / processed : 0.0)
                    << " avgPackMs=" << (processed ? packMsAccum / processed : 0.0);
                logLine(oss.str());
                statsStart = now;
            }
            return true;
        }

        currentVideoSourceUs = pts == AV_NOPTS_VALUE
            ? AV_NOPTS_VALUE
            : av_rescale_q(pts, inVideo->time_base, AV_TIME_BASE_Q);
        int64_t encoderPts = pts == AV_NOPTS_VALUE
            ? static_cast<int64_t>(processed)
            : av_rescale_q(pts, inVideo->time_base, enc->time_base);
        if (lastSubmittedVideoPts != AV_NOPTS_VALUE && encoderPts <= lastSubmittedVideoPts) {
            const int64_t nominalStep = std::max<int64_t>(
                1, av_rescale_q(1, av_inv_q(frameRate), enc->time_base));
            const int64_t oldPts = encoderPts;
            encoderPts = lastSubmittedVideoPts + nominalStep;
            logLine("corrected non-monotonic decoded video PTS old=" +
                    std::to_string(oldPts) + " new=" +
                    std::to_string(encoderPts));
        }
        lastSubmittedVideoPts = encoderPts;
        encodeFrame->pts = encoderPts;
        if (trace) logLine(std::string("first-frame stage=encoder_send_start encoder=") +
                           avcodec_get_name(enc->codec_id));
        rc = avcodec_send_frame(enc, encodeFrame);
        if (rc < 0) { error = "avcodec_send_frame encoder: " + fferr(rc); return false; }
        if (trace) logLine("first-frame stage=encoder_send_done");
        if (!writeEncodedPackets(error)) return false;
        auto t3 = std::chrono::steady_clock::now();

        ++processed;
        logConversionProgress();
        if (!playbackClockStarted) {
            wallStart = std::chrono::steady_clock::now();
            statsStart = wallStart;
            playbackClockStarted = true;
        }
        if (trace) {
            firstFrameTrace = false;
            std::ostringstream done;
            done << "first-frame stage=complete encodedPackets=" << encodedPackets;
            logLine(done.str());
        }
        inferMsAccum += std::chrono::duration<double, std::milli>(t1 - t0).count();
        packMsAccum += std::chrono::duration<double, std::milli>(t2 - t1).count();
        encodeMsAccum += std::chrono::duration<double, std::milli>(t3 - t2).count();

        const auto now = std::chrono::steady_clock::now();
        const double statsElapsed = std::chrono::duration<double>(now - statsStart).count();
        if (statsElapsed >= 2.0) {
            const double total = static_cast<double>(processed + dropped);
            const double fps = processed / std::max(0.001, std::chrono::duration<double>(now - wallStart).count());
            std::ostringstream oss;
            oss << std::fixed << std::setprecision(2)
                << "realtime fpsOut=" << fps
                << " processed=" << processed
                << " dropped=" << dropped
                << " dropPct=" << (total > 0.0 ? 100.0 * dropped / total : 0.0)
                << " avgRvmMs=" << (processed ? inferMsAccum / processed : 0.0)
                << (webmAlphaOutput ? " avgYuvaTransferMs=" : " avgPackMs=")
                << (processed ? packMsAccum / processed : 0.0)
                << " avgEncodeMuxMs=" << (processed ? encodeMsAccum / processed : 0.0);
            logLine(oss.str());
            statsStart = now;
        }
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
            logLine("Standalone preview active: FFmpeg NVDEC(CUDA) -> ONNX Runtime CUDA EP RVM -> CUDA AlphaPacked -> preview window");
        } else if (args.outputMode == Args::OutputMode::WebmVp9Alpha) {
            logLine("GPU/CPU pipeline active: FFmpeg NVDEC(CUDA) -> ONNX Runtime CUDA EP RVM -> CUDA YUVA prepare -> GPU-to-CPU YUVA420P -> libvpx-vp9 real alpha -> WebM");
        } else if (args.outputMode == Args::OutputMode::ChromaKey) {
            logLine(std::string("GPU pipeline active: FFmpeg NVDEC(CUDA) -> ONNX Runtime CUDA EP RVM -> CUDA green chroma-key composite -> FFmpeg NVENC -> ") +
                    (args.offlineConvert ? "MP4 file" : "MPEG-TS"));
        } else {
            logLine(std::string("GPU pipeline active: FFmpeg NVDEC(CUDA) -> ONNX Runtime CUDA EP RVM -> CUDA AlphaPacked (input projection preserved) -> FFmpeg NVENC -> ") +
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
                    oss << " output=ChromaKey-Green/HEVC/NVENC QP" << args.qp;
                } else {
                    oss << " output=HEVC/NVENC QP" << args.qp;
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

} // namespace

int main(int argc, char** argv) {
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
    av_log_set_level(AV_LOG_ERROR);
    logLine("build=DeoVR_Search_NoEraseRedraw_SkyBlueUI_TitleLinks");

    if (argc >= 2 && std::string(argv[1]) == "--probe-gpu") {
        std::filesystem::path model;
        int device = 0;
        for (int i = 2; i < argc; ++i) {
            const std::string option(argv[i]);
            if (option == "--model" && i + 1 < argc) {
                model = std::filesystem::u8path(argv[++i]);
            } else if (option == "--device" && i + 1 < argc) {
                device = std::atoi(argv[++i]);
            }
        }
        if (model.empty()) {
            wchar_t executable[MAX_PATH]{};
            GetModuleFileNameW(nullptr, executable, MAX_PATH);
            model = std::filesystem::path(executable).parent_path() /
                    L"rvm_mobilenetv3_fp16.onnx";
        }
        return probeGpuSupport(model, device);
    }

    Args args;
    if (!parseArgs(argc, argv, args)) {
        std::cerr << "Usage: r800zz_ai_worker.exe <video> [--model rvm_mobilenetv3_fp16.onnx] [--qp 20] [--device 0] [--downsample 0.25] [--start-ms 0] [--output-mode alpha-packed|webm-alpha|chroma-key] [--preview-only] [--resident] [--convert-output file] [--no-preview] [--cpu] [--video-player] [--player-language 0..6]\n"
                  << "       r800zz_ai_worker.exe --probe-gpu [--model rvm_mobilenetv3_fp16.onnx] [--device 0]\n";
        return 2;
    }
    if (!std::filesystem::is_regular_file(args.input)) {
        logLine("ERROR input not found: " + args.input.u8string());
        return 3;
    }
    if (args.videoPlayer) {
        std::string playerError;
        if (!RunSimpleVideoPlayer(args.input, args.playerLanguage, playerError)) {
            logLine("ERROR Video Player: " + playerError);
            return 14;
        }
        return 0;
    }
    if (!std::filesystem::is_regular_file(args.model)) {
        logLine("ERROR model not found: " + args.model.u8string());
        return 4;
    }
    if (args.offlineConvert) {
        if (args.outputFile.empty() || args.outputFile == args.input) {
            logLine("ERROR conversion output must be different from the input file");
            return 5;
        }
        logLine("offline conversion output=" + args.outputFile.u8string());
    }

    std::filesystem::path finalOutput;
    std::filesystem::path partialOutput;
    if (args.offlineConvert) {
        finalOutput = args.outputFile;
        partialOutput = partialOutputPath(finalOutput);
        removePartialOutput(partialOutput);
        args.outputFile = partialOutput;
        logLine("offline temporary output=" + partialOutput.u8string());
    }

    avformat_network_init();

    if (args.cpu) {
        if (!args.offlineConvert || args.previewOnly || args.resident) {
            logLine("ERROR --cpu is supported only with --convert-output");
            return 6;
        }
        CpuConversionOptions cpuOptions;
        cpuOptions.input = args.input;
        cpuOptions.output = args.outputFile;
        cpuOptions.model = args.model;
        cpuOptions.quality = args.qp;
        cpuOptions.downsample = args.downsample > 0.0f
            ? args.downsample : 0.25f;
        if (args.outputMode == Args::OutputMode::WebmVp9Alpha) {
            cpuOptions.mode = CpuConversionMode::WebmVp9Alpha;
        } else if (args.outputMode == Args::OutputMode::ChromaKey) {
            cpuOptions.mode = CpuConversionMode::ChromaKey;
        } else {
            cpuOptions.mode = CpuConversionMode::AlphaPacked;
        }
        std::string cpuError;
        if (!RunCpuConversion(cpuOptions, cpuError)) {
            logLine("ERROR CPU conversion: " + cpuError);
            removePartialOutput(partialOutput);
            return 12;
        }
        if (!commitPartialOutput(partialOutput, finalOutput, cpuError)) {
            logLine("ERROR CPU conversion: " + cpuError);
            removePartialOutput(partialOutput);
            return 13;
        }
        logLine("completed");
        return 0;
    }

    if (args.resident) {
        // Create the CUDA/ORT session once and execute one warm-up inference.
        // RUN commands reuse that session, while each request still gets fresh
        // demuxer, decoder, encoder and muxer state.
        Pipeline warm;
        warm.args = args;
        std::string error;
        // FFmpeg's primary CUDA context must be created first. It selects
        // CU_CTX_SCHED_BLOCKING_SYNC; creating ORT/CUDA first activates the
        // primary context with incompatible flags and makes later NVDEC fail.
        if (!warm.initInput(error) || !warm.initCudaDecoder(error) ||
            !warm.initRvm(error)) {
            logLine("RESIDENT_ERROR init: " + error);
            return 20;
        }

        if (cudaMemsetAsync(warm.rvmInput, 0, warm.rvmInputBytes,
                            warm.cudaStream) != cudaSuccess) {
            logLine("RESIDENT_ERROR warmup cudaMemset failed");
            return 21;
        }
        RvmGpuOutput warmOutput{};
        logLine("resident RVM warmup start");
        if (!warm.rvm->run(warm.rvmInput, warm.cudaStream, warmOutput, error)) {
            logLine("RESIDENT_ERROR warmup: " + error);
            return 22;
        }
        if (!warm.rvm->resetState(warm.cudaStream, error)) {
            logLine("RESIDENT_ERROR reset after warmup: " + error);
            return 23;
        }
        logLine("resident RVM warmup complete");

        std::unique_ptr<RvmOrtGpu> residentRvm = std::move(warm.ownedRvm);
        AVBufferRef* residentCudaDevice = warm.cudaDevice;
        cudaStream_t residentStream = warm.cudaStream;
        warm.rvm = nullptr;
        warm.cudaDevice = nullptr;
        warm.cudaStream = nullptr;
        warm.ownsCudaStream = false;
        warm.cleanup();
        logLine("RESIDENT_READY");

        std::atomic<bool> cancelStream{false};
        std::thread streamThread;
        uint64_t activeRequestId = 0;

        auto stopCurrentStream = [&]() {
            cancelStream.store(true);
            if (streamThread.joinable()) streamThread.join();
            cancelStream.store(false);
        };

        std::string command;
        while (std::getline(std::cin, command)) {
            if (!command.empty() && command.back() == '\r') command.pop_back();
            if (command == "QUIT") {
                stopCurrentStream();
                break;
            }
            if (command.rfind("CANCEL ", 0) == 0) {
                char* end = nullptr;
                const unsigned long long requestId =
                    std::strtoull(command.c_str() + 7, &end, 10);
                if (end != command.c_str() + 7 && requestId == activeRequestId) {
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
            parser >> verb >> requestId >> requestedMs >> outputMode >> pipeName >> inputHex;
            if (verb != "RUN" || requestId == 0 || pipeName.empty() ||
                (outputMode != "alpha-packed" && outputMode != "webm-alpha" &&
                 outputMode != "chroma-key")) {
                logLine("RESIDENT_ERROR unknown command=" + command);
                continue;
            }

            std::filesystem::path streamInput = args.input;
            if (!inputHex.empty()) {
                std::string inputUtf8;
                if (!decodeHex(inputHex, inputUtf8)) {
                    logLine("RESIDENT_ERROR invalid input path encoding");
                    continue;
                }
                streamInput = std::filesystem::u8path(inputUtf8);
                if (!std::filesystem::is_regular_file(streamInput)) {
                    logLine("RESIDENT_ERROR input not found: " + inputUtf8);
                    continue;
                }
            }

            stopCurrentStream();
            activeRequestId = requestId;
            const int64_t startMs = std::max<long long>(0, requestedMs);
            streamThread = std::thread([&, startMs, outputMode, pipeName, streamInput, requestId]() {
                HANDLE output = INVALID_HANDLE_VALUE;
                const auto connectBegin = std::chrono::steady_clock::now();
                while (!cancelStream.load()) {
                    output = CreateFileA(
                        pipeName.c_str(), GENERIC_WRITE, 0, nullptr,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                    if (output != INVALID_HANDLE_VALUE) break;
                    const DWORD win32 = GetLastError();
                    if (win32 != ERROR_PIPE_BUSY && win32 != ERROR_FILE_NOT_FOUND) {
                        logLine("RESIDENT_STREAM_ERROR open output pipe Win32=" +
                                std::to_string(win32));
                        return;
                    }
                    if (std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::steady_clock::now() - connectBegin).count() >= 120) {
                        logLine("RESIDENT_STREAM_ERROR output pipe timeout");
                        return;
                    }
                    WaitNamedPipeA(pipeName.c_str(), 50);
                }
                if (output == INVALID_HANDLE_VALUE) return;

                int streamCode = 0;
                {
                    Pipeline p;
                    p.args = args;
                    p.args.input = streamInput;
                    p.args.resident = false;
                    p.args.previewOnly = false;
                    p.args.startMs = startMs;
                    if (outputMode == "webm-alpha") {
                        p.args.outputMode = Args::OutputMode::WebmVp9Alpha;
                    } else if (outputMode == "chroma-key") {
                        p.args.outputMode = Args::OutputMode::ChromaKey;
                    } else {
                        p.args.outputMode = Args::OutputMode::AlphaPacked;
                    }
                    p.externalRvm = residentRvm.get();
                    p.externalCudaDevice = residentCudaDevice;
                    p.externalCudaStream = residentStream;
                    p.outputHandle = output;
                    p.cancelFlag = &cancelStream;

                    std::string runError;
                    if (!p.initInput(runError) || !p.initCudaDecoder(runError) ||
                        !p.initRvm(runError) || !p.initEncoderAndMuxer(runError) ||
                        !p.initPreview(runError)) {
                        logLine("ERROR resident stream init: " + runError);
                        streamCode = 10;
                    } else if (!p.run(runError)) {
                        logLine("ERROR resident stream run: " + runError);
                        streamCode = 11;
                    }
                }
                CloseHandle(output);
                logLine("RESIDENT_STREAM_DONE id=" + std::to_string(requestId) +
                        " code=" + std::to_string(streamCode));
            });
        }

        stopCurrentStream();
        residentRvm.reset();
        if (residentStream) cudaStreamDestroy(residentStream);
        if (residentCudaDevice) av_buffer_unref(&residentCudaDevice);
        logLine("RESIDENT_EXIT");
        return 0;
    }

    std::string error;
    int exitCode = 0;
    {
        Pipeline p;
        p.args = args;
        if (!p.initInput(error) || !p.initCudaDecoder(error) || !p.initRvm(error) ||
            !(args.previewOnly ? p.initOutputFramePool(error) : p.initEncoderAndMuxer(error)) ||
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
        !commitPartialOutput(partialOutput, finalOutput, error)) {
        logLine("ERROR conversion finalize: " + error);
        removePartialOutput(partialOutput);
        return 13;
    }
    logLine("completed");
    return 0;
}
