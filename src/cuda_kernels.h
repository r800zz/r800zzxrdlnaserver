#pragma once

#include <cuda_runtime.h>
#include <cstddef>
#include <cstdint>

struct R800ZZGpuFrame {
    const uint8_t* y = nullptr;
    const uint8_t* uv = nullptr;
    int pitchY = 0;
    int pitchUV = 0;
    int width = 0;
    int height = 0;
};

struct R800ZZGpuFrameMutable {
    uint8_t* y = nullptr;
    uint8_t* uv = nullptr;
    int pitchY = 0;
    int pitchUV = 0;
    int width = 0;
    int height = 0;
};

cudaError_t r800zzNv12ToRgbHalfNchw(
    const R800ZZGpuFrame& src,
    void* dstHalf,
    cudaStream_t stream);

cudaError_t r800zzBuildProjectionMap(
    void* mapFloat2,
    int width,
    int height,
    cudaStream_t stream);

cudaError_t r800zzPackAlphaNv12(
    const R800ZZGpuFrame& src,
    const void* alphaHalf,
    const void* projectionMapFloat2,
    const R800ZZGpuFrameMutable& dst,
    float packScale,
    cudaStream_t stream);

// Composite the RVM foreground over a pure green background and keep the
// result in NV12 on the GPU for the HEVC/NVENC chroma-key stream.
cudaError_t r800zzCompositeChromaKeyNv12(
    const R800ZZGpuFrame& src,
    const void* alphaHalf,
    const R800ZZGpuFrameMutable& dst,
    cudaStream_t stream);

// Prepare the software VP9 encoder planes on the GPU. The Y plane remains the
// decoder's NV12 Y plane; this kernel splits NV12 UV and converts RVM FP16
// alpha to an 8-bit full-resolution alpha plane.
cudaError_t r800zzPrepareYuva420p(
    const R800ZZGpuFrame& src,
    const void* alphaHalf,
    uint8_t* dstU,
    int pitchU,
    uint8_t* dstV,
    int pitchV,
    uint8_t* dstAlpha,
    int pitchAlpha,
    cudaStream_t stream);

// Convert the processed NV12 output frame to a small BGRA preview image.
cudaError_t r800zzNv12ToBgraPreview(
    const R800ZZGpuFrame& src,
    uint8_t* dstBgra,
    int dstWidth,
    int dstHeight,
    cudaStream_t stream);
