#pragma once
#include <cstdint>
#include <memory>
#include <string>

enum class RvmBackend {
    DirectML,
    CPU
};

enum class RvmAlphaInputFormat {
    Rgba8,
    Yuv420p,
    Nv12
};

enum class RvmGpuCompositeMode {
    AlphaPacked,
    ChromaKey
};

struct RvmOutput {
    const float* alphaFp32 = nullptr;
};

class RvmOrt {
public:
    RvmOrt();
    ~RvmOrt();

    RvmOrt(const RvmOrt&) = delete;
    RvmOrt& operator=(const RvmOrt&) = delete;

    bool initialize(const std::wstring& modelPath,
                    int width,
                    int height,
                    float downsampleRatio,
                    RvmBackend backend,
                    int dxgiDeviceId,
                    std::string& error);

    bool run(const float* srcFp32Nchw,
             RvmOutput& output,
             std::string& error);

    // DirectML GPU composite path shared by Alpha Packed and Chroma Key.
    // YUV/RGBA->NCHW preprocessing, RVM inference, compositing and RGBA->NV12
    // stay on GPU. D3D11 input/output enables the zero-copy realtime path.
    // A supplied D3D11 encoder destination uses shared GPU fences so the
    // D3D11<->D3D12 handoff does not block the CPU every frame.
    bool runGpuCompositeNv12(RvmGpuCompositeMode outputMode,
                             RvmAlphaInputFormat inputFormat,
                             const uint8_t* plane0, int stride0,
                             const uint8_t* plane1, int stride1,
                             const uint8_t* plane2, int stride2,
                             uint8_t* dstY, int dstYStride,
                             uint8_t* dstUV, int dstUVStride,
                             std::string& error,
                             void* d3d11SourceTexture = nullptr,
                             int d3d11SourceArraySlice = 0,
                             void** d3d11OutputTexture = nullptr,
                             void* d3d11DestinationTexture = nullptr,
                             int d3d11DestinationArraySlice = 0);

    // DirectML-only fast path for Alpha Packed output. Common YUV420P/NV12
    // decoder frames are uploaded without a CPU colorspace conversion.
    // YUV/RGBA->NCHW preprocessing, RVM src/pha tensors, alpha packing and
    // RGBA->NV12 conversion stay on GPU. Only the final NV12 bytes are copied
    // to the caller-provided encoder frame planes.
    bool runAlphaPackedNv12(RvmAlphaInputFormat inputFormat,
                            const uint8_t* plane0, int stride0,
                            const uint8_t* plane1, int stride1,
                            const uint8_t* plane2, int stride2,
                            uint8_t* dstY, int dstYStride,
                            uint8_t* dstUV, int dstUVStride,
                            std::string& error,
                            void* d3d11SourceTexture = nullptr,
                            int d3d11SourceArraySlice = 0,
                            void** d3d11OutputTexture = nullptr,
                            void* d3d11DestinationTexture = nullptr,
                            int d3d11DestinationArraySlice = 0);

    bool resetState(std::string& error);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
