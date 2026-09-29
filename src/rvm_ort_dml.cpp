#include "rvm_ort_dml.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <vector>

#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <onnxruntime_cxx_api.h>
#include <dml_provider_factory.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;

namespace {

std::string ortError(const Ort::Exception& e) {
    std::ostringstream oss;
    oss << "ONNX Runtime error: " << e.what()
        << " code=" << static_cast<int>(e.GetOrtErrorCode());
    return oss.str();
}

std::string ortStatusError(OrtStatus* status) {
    if (!status) return {};
    const OrtApi& api = Ort::GetApi();
    const char* text = api.GetErrorMessage(status);
    std::string result = text ? text : "Unknown ONNX Runtime error";
    api.ReleaseStatus(status);
    return result;
}

std::string hrError(const char* what, HRESULT hr) {
    std::ostringstream oss;
    oss << what << " failed hr=0x" << std::hex
        << static_cast<unsigned long>(hr);
    return oss.str();
}

Ort::Value makeCpuTensor(const Ort::MemoryInfo& mem,
                         float* ptr,
                         size_t count,
                         const std::vector<int64_t>& shape) {
    return Ort::Value::CreateTensor<float>(
        mem, ptr, count, shape.data(), shape.size());
}

bool hasOfficialRvmInputs(Ort::Session& session, std::string& error) {
    bool hasSrc = false;
    bool hasRatio = false;
    Ort::AllocatorWithDefaultOptions allocator;
    const size_t inputCount = session.GetInputCount();

    for (size_t i = 0; i < inputCount; ++i) {
        auto name = session.GetInputNameAllocated(i, allocator);
        if (!name) continue;
        if (std::strcmp(name.get(), "src") == 0) hasSrc = true;
        if (std::strcmp(name.get(), "downsample_ratio") == 0) hasRatio = true;
    }

    if (!hasSrc || !hasRatio) {
        error =
            "RVM model mismatch: official FP32 ONNX with src and "
            "downsample_ratio inputs is required";
        return false;
    }
    return true;
}

ComPtr<ID3D12Resource> createBuffer(ID3D12Device* device,
                                    size_t byteSize,
                                    D3D12_HEAP_TYPE heapType,
                                    D3D12_RESOURCE_STATES initialState,
                                    D3D12_RESOURCE_FLAGS flags,
                                    std::string& error) {
    const size_t alignment =
        static_cast<size_t>(DML_MINIMUM_BUFFER_TENSOR_ALIGNMENT);
    byteSize = std::max(byteSize, alignment);
    byteSize = (byteSize + alignment - 1u) & ~(alignment - 1u);

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = heapType;
    heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = static_cast<UINT64>(byteSize);
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;

    ComPtr<ID3D12Resource> result;
    const HRESULT hr = device->CreateCommittedResource(
        &heap,
        D3D12_HEAP_FLAG_NONE,
        &desc,
        initialState,
        nullptr,
        IID_PPV_ARGS(&result));
    if (FAILED(hr)) {
        error = hrError("CreateCommittedResource", hr);
        return {};
    }
    return result;
}

bool compileComputeShader(const char* source,
                          ComPtr<ID3DBlob>& shader,
                          std::string& error) {
    ComPtr<ID3DBlob> errors;
    const HRESULT hr = D3DCompile(
        source,
        std::strlen(source),
        nullptr,
        nullptr,
        nullptr,
        "main",
        "cs_5_1",
        D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0,
        &shader,
        &errors);
    if (FAILED(hr)) {
        if (errors && errors->GetBufferPointer()) {
            error.assign(
                static_cast<const char*>(errors->GetBufferPointer()),
                errors->GetBufferSize());
        } else {
            error = hrError("D3DCompile", hr);
        }
        return false;
    }
    return true;
}

constexpr const char* kPreprocessShader = R"HLSL(
ByteAddressBuffer srcRgba : register(t0);
ByteAddressBuffer unusedAlpha : register(t1);
RWByteAddressBuffer dstNchw : register(u0);
cbuffer Params : register(b0) { uint width; uint height; };

[numthreads(16, 16, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    uint x = tid.x;
    uint y = tid.y;
    if (x >= width || y >= height) return;
    uint pixels = width * height;
    uint i = y * width + x;

    uint rgba = srcRgba.Load(i * 4);
    float r = float( rgba        & 0xffu) * (1.0 / 255.0);
    float g = float((rgba >>  8) & 0xffu) * (1.0 / 255.0);
    float b = float((rgba >> 16) & 0xffu) * (1.0 / 255.0);

    dstNchw.Store((i) * 4, asuint(r));
    dstNchw.Store((pixels + i) * 4, asuint(g));
    dstNchw.Store((pixels * 2 + i) * 4, asuint(b));
}
)HLSL";

constexpr const char* kYuv420pToRgbaShader = R"HLSL(
ByteAddressBuffer srcYuv : register(t0);
ByteAddressBuffer unusedAlpha : register(t1);
RWByteAddressBuffer dstRgba : register(u0);
cbuffer Params : register(b0) { uint width; uint height; };

uint loadByte(uint offset) {
    uint word = srcYuv.Load(offset & ~3u);
    return (word >> ((offset & 3u) * 8u)) & 0xffu;
}

uint packRgba(float3 rgb) {
    uint r = (uint)round(clamp(rgb.r, 0.0, 255.0));
    uint g = (uint)round(clamp(rgb.g, 0.0, 255.0));
    uint b = (uint)round(clamp(rgb.b, 0.0, 255.0));
    return r | (g << 8) | (b << 16) | 0xff000000u;
}

float3 yuvToRgb(uint yv, uint uv, uint vv) {
    float y = 1.16438356 * max(0.0, float(yv) - 16.0);
    float u = float(uv) - 128.0;
    float v = float(vv) - 128.0;
    return float3(
        y + 1.59602678 * v,
        y - 0.39176229 * u - 0.81296764 * v,
        y + 2.01723214 * u);
}

[numthreads(16, 16, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    uint x = tid.x;
    uint y = tid.y;
    if (x >= width || y >= height) return;

    uint pixels = width * height;
    uint chromaW = width / 2;
    uint chromaIndex = (y / 2) * chromaW + (x / 2);
    uint yv = loadByte(y * width + x);
    uint uv = loadByte(pixels + chromaIndex);
    uint vv = loadByte(pixels + pixels / 4 + chromaIndex);
    dstRgba.Store((y * width + x) * 4, packRgba(yuvToRgb(yv, uv, vv)));
}
)HLSL";

constexpr const char* kNv12ToRgbaShader = R"HLSL(
ByteAddressBuffer srcYuv : register(t0);
ByteAddressBuffer unusedAlpha : register(t1);
RWByteAddressBuffer dstRgba : register(u0);
cbuffer Params : register(b0) { uint width; uint height; };

uint loadByte(uint offset) {
    uint word = srcYuv.Load(offset & ~3u);
    return (word >> ((offset & 3u) * 8u)) & 0xffu;
}

uint packRgba(float3 rgb) {
    uint r = (uint)round(clamp(rgb.r, 0.0, 255.0));
    uint g = (uint)round(clamp(rgb.g, 0.0, 255.0));
    uint b = (uint)round(clamp(rgb.b, 0.0, 255.0));
    return r | (g << 8) | (b << 16) | 0xff000000u;
}

float3 yuvToRgb(uint yv, uint uv, uint vv) {
    float y = 1.16438356 * max(0.0, float(yv) - 16.0);
    float u = float(uv) - 128.0;
    float v = float(vv) - 128.0;
    return float3(
        y + 1.59602678 * v,
        y - 0.39176229 * u - 0.81296764 * v,
        y + 2.01723214 * u);
}

[numthreads(16, 16, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    uint x = tid.x;
    uint y = tid.y;
    if (x >= width || y >= height) return;

    uint pixels = width * height;
    uint uvBase = pixels + (y / 2) * width + (x & ~1u);
    uint yv = loadByte(y * width + x);
    uint uv = loadByte(uvBase);
    uint vv = loadByte(uvBase + 1);
    dstRgba.Store((y * width + x) * 4, packRgba(yuvToRgb(yv, uv, vv)));
}
)HLSL";

constexpr const char* kNv12PitchedToRgbaShader = R"HLSL(
ByteAddressBuffer srcYuv : register(t0);
RWByteAddressBuffer dstRgba : register(u0);
cbuffer Params : register(b0) {
    uint width; uint height;
    uint yOffset; uint uvOffset;
    uint yPitch; uint uvPitch;
};

uint loadByte(uint offset) {
    uint word = srcYuv.Load(offset & ~3u);
    return (word >> ((offset & 3u) * 8u)) & 0xffu;
}
uint packRgba(float3 rgb) {
    uint r = (uint)round(clamp(rgb.r, 0.0, 255.0));
    uint g = (uint)round(clamp(rgb.g, 0.0, 255.0));
    uint b = (uint)round(clamp(rgb.b, 0.0, 255.0));
    return r | (g << 8) | (b << 16) | 0xff000000u;
}
float3 yuvToRgb(uint yv, uint uv, uint vv) {
    float y = 1.16438356 * max(0.0, float(yv) - 16.0);
    float u = float(uv) - 128.0;
    float v = float(vv) - 128.0;
    return float3(
        y + 1.59602678 * v,
        y - 0.39176229 * u - 0.81296764 * v,
        y + 2.01723214 * u);
}
[numthreads(16, 16, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    uint x = tid.x;
    uint y = tid.y;
    if (x >= width || y >= height) return;
    uint yv = loadByte(yOffset + y * yPitch + x);
    uint uvBase = uvOffset + (y / 2) * uvPitch + (x & ~1u);
    uint uv = loadByte(uvBase);
    uint vv = loadByte(uvBase + 1);
    dstRgba.Store((y * width + x) * 4, packRgba(yuvToRgb(yv, uv, vv)));
}
)HLSL";

constexpr const char* kAlphaPackedShader = R"HLSL(
ByteAddressBuffer srcRgba : register(t0);
ByteAddressBuffer alphaFp32 : register(t1);
RWByteAddressBuffer dstRgba : register(u0);
cbuffer Params : register(b0) { uint width; uint height; };

[numthreads(16, 16, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    uint x = tid.x;
    uint y = tid.y;
    if (x >= width || y >= height) return;
    uint i = y * width + x;
    uint rgba = srcRgba.Load(i * 4);

    float py = (float(y) + 0.5) / float(height);
    float srcV = -1.0;
    if (py < 0.2) {
        srcV = (0.2 + py) / 0.4;
    } else if (py >= 0.8) {
        srcV = (py - 0.8) / 0.4;
    }

    if (srcV >= 0.0 && srcV < 1.0) {
        float px = (float(x) + 0.5) / float(width);
        float srcU = -1.0;
        if (px < 0.1) {
            srcU = (0.3 + px) / 0.4;
        } else if (px >= 0.4 && px < 0.6) {
            srcU = (px - 0.4) / 0.4;
        } else if (px >= 0.9) {
            srcU = (0.2 + px - 0.9) / 0.4;
        }

        if (srcU >= 0.0 && srcU < 1.0) {
            uint sx = min(uint(srcU * float(width)), width - 1);
            uint sy = min(uint(srcV * float(height)), height - 1);
            float a = saturate(asfloat(alphaFp32.Load((sy * width + sx) * 4)));
            uint gray = uint(a * 255.0 + 0.5);
            rgba = (rgba & 0xff000000u) |
                   gray | (gray << 8) | (gray << 16);
        }
    }

    dstRgba.Store(i * 4, rgba);
}
)HLSL";


constexpr const char* kChromaKeyShader = R"HLSL(
ByteAddressBuffer srcRgba : register(t0);
ByteAddressBuffer alphaFp32 : register(t1);
RWByteAddressBuffer dstRgba : register(u0);
cbuffer Params : register(b0) { uint width; uint height; };

[numthreads(16, 16, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    uint x = tid.x;
    uint y = tid.y;
    if (x >= width || y >= height) return;

    uint i = y * width + x;
    uint rgba = srcRgba.Load(i * 4);
    float3 src = float3(
        float( rgba        & 0xffu),
        float((rgba >>  8) & 0xffu),
        float((rgba >> 16) & 0xffu));

    float a = saturate(asfloat(alphaFp32.Load(i * 4)));
    float3 bg = float3(0.35 * 255.0, 255.0, 0.0);
    float3 outRgb = src * a + bg * (1.0 - a);

    uint r = (uint)round(clamp(outRgb.r, 0.0, 255.0));
    uint g = (uint)round(clamp(outRgb.g, 0.0, 255.0));
    uint b = (uint)round(clamp(outRgb.b, 0.0, 255.0));
    dstRgba.Store(i * 4, r | (g << 8) | (b << 16) | 0xff000000u);
}
)HLSL";

constexpr const char* kRgbaToNv12Shader = R"HLSL(
ByteAddressBuffer srcRgba : register(t0);
ByteAddressBuffer unusedAlpha : register(t1);
RWByteAddressBuffer dstNv12 : register(u0);
cbuffer Params : register(b0) { uint width; uint height; };

uint loadRgba(uint x, uint y) {
    x = min(x, width - 1);
    y = min(y, height - 1);
    return srcRgba.Load((y * width + x) * 4);
}

float3 unpackRgb(uint rgba) {
    return float3(
        float( rgba        & 0xffu),
        float((rgba >>  8) & 0xffu),
        float((rgba >> 16) & 0xffu));
}

uint yByte(float3 rgb) {
    float y = 16.0 + 0.257 * rgb.r + 0.504 * rgb.g + 0.098 * rgb.b;
    return (uint)round(clamp(y, 0.0, 255.0));
}

uint uByte(float3 rgb) {
    float u = 128.0 - 0.148 * rgb.r - 0.291 * rgb.g + 0.439 * rgb.b;
    return (uint)round(clamp(u, 0.0, 255.0));
}

uint vByte(float3 rgb) {
    float v = 128.0 + 0.439 * rgb.r - 0.368 * rgb.g - 0.071 * rgb.b;
    return (uint)round(clamp(v, 0.0, 255.0));
}

// Each thread writes one 32-bit word. First the Y plane (4 Y pixels/word),
// then the interleaved UV plane (2 chroma samples/word). Width/height are
// expected to be even for NV12, as required by the video encoder path.
[numthreads(256, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    uint yBytes = width * height;
    uint uvBytes = yBytes / 2;
    uint yWords = (yBytes + 3) / 4;
    uint uvWords = (uvBytes + 3) / 4;
    uint wordIndex = tid.x;

    if (wordIndex < yWords) {
        uint basePixel = wordIndex * 4;
        uint packed = 0;
        [unroll]
        for (uint j = 0; j < 4; ++j) {
            uint pixelIndex = basePixel + j;
            uint value = 0;
            if (pixelIndex < yBytes) {
                uint x = pixelIndex % width;
                uint y = pixelIndex / width;
                value = yByte(unpackRgb(loadRgba(x, y)));
            }
            packed |= value << (j * 8);
        }
        dstNv12.Store(wordIndex * 4, packed);
        return;
    }

    uint uvWord = wordIndex - yWords;
    if (uvWord >= uvWords) return;

    uint chromaSamplesPerRow = width / 2;
    uint firstSample = uvWord * 2;
    uint totalSamples = (width / 2) * (height / 2);
    uint packedUv = 0;

    [unroll]
    for (uint j = 0; j < 2; ++j) {
        uint sample = firstSample + j;
        uint u = 128;
        uint v = 128;
        if (sample < totalSamples) {
            uint cx = sample % chromaSamplesPerRow;
            uint cy = sample / chromaSamplesPerRow;
            uint sx = cx * 2;
            uint sy = cy * 2;
            float3 avg = (
                unpackRgb(loadRgba(sx,     sy)) +
                unpackRgb(loadRgba(sx + 1, sy)) +
                unpackRgb(loadRgba(sx,     sy + 1)) +
                unpackRgb(loadRgba(sx + 1, sy + 1))) * 0.25;
            u = uByte(avg);
            v = vByte(avg);
        }
        packedUv |= u << (j * 16);
        packedUv |= v << (j * 16 + 8);
    }
    dstNv12.Store(yBytes + uvWord * 4, packedUv);
}
)HLSL";

constexpr const char* kRgbaToNv12PitchedShader = R"HLSL(
ByteAddressBuffer srcRgba : register(t0);
RWByteAddressBuffer dstNv12 : register(u0);
cbuffer Params : register(b0) {
    uint width; uint height;
    uint yOffset; uint uvOffset;
    uint yPitch; uint uvPitch;
};
uint loadRgba(uint x, uint y) {
    x = min(x, width - 1);
    y = min(y, height - 1);
    return srcRgba.Load((y * width + x) * 4);
}
float3 unpackRgb(uint rgba) {
    return float3(float(rgba & 0xffu), float((rgba >> 8) & 0xffu), float((rgba >> 16) & 0xffu));
}
uint yByte(float3 rgb) {
    return (uint)round(clamp(16.0 + 0.257 * rgb.r + 0.504 * rgb.g + 0.098 * rgb.b, 0.0, 255.0));
}
uint uByte(float3 rgb) {
    return (uint)round(clamp(128.0 - 0.148 * rgb.r - 0.291 * rgb.g + 0.439 * rgb.b, 0.0, 255.0));
}
uint vByte(float3 rgb) {
    return (uint)round(clamp(128.0 + 0.439 * rgb.r - 0.368 * rgb.g - 0.071 * rgb.b, 0.0, 255.0));
}
[numthreads(256, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    uint yWordsPerRow = (width + 3u) / 4u;
    uint yWords = yWordsPerRow * height;
    uint uvWordsPerRow = (width + 3u) / 4u;
    uint uvRows = height / 2u;
    uint uvWords = uvWordsPerRow * uvRows;
    uint wi = tid.x;
    if (wi < yWords) {
        uint row = wi / yWordsPerRow;
        uint word = wi % yWordsPerRow;
        uint baseX = word * 4u;
        uint packed = 0;
        [unroll]
        for (uint j = 0; j < 4; ++j) {
            uint x = baseX + j;
            uint v = x < width ? yByte(unpackRgb(loadRgba(x, row))) : 0;
            packed |= v << (j * 8u);
        }
        dstNv12.Store(yOffset + row * yPitch + word * 4u, packed);
        return;
    }
    uint uwi = wi - yWords;
    if (uwi >= uvWords) return;
    uint row = uwi / uvWordsPerRow;
    uint word = uwi % uvWordsPerRow;
    uint firstSample = word * 2u;
    uint packedUv = 0;
    [unroll]
    for (uint j = 0; j < 2; ++j) {
        uint sample = firstSample + j;
        uint u = 128, v = 128;
        if (sample < width / 2u) {
            uint sx = sample * 2u;
            uint sy = row * 2u;
            float3 avg = (unpackRgb(loadRgba(sx, sy)) + unpackRgb(loadRgba(sx + 1, sy)) +
                          unpackRgb(loadRgba(sx, sy + 1)) + unpackRgb(loadRgba(sx + 1, sy + 1))) * 0.25;
            u = uByte(avg); v = vByte(avg);
        }
        packedUv |= u << (j * 16u);
        packedUv |= v << (j * 16u + 8u);
    }
    dstNv12.Store(uvOffset + row * uvPitch + word * 4u, packedUv);
}
)HLSL";

} // namespace

struct RvmOrt::Impl {
    Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "r800zz-rvm"};
    Ort::SessionOptions options;
    std::unique_ptr<Ort::Session> session;

    Ort::MemoryInfo cpuMem{
        Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault)};

    int width = 0;
    int height = 0;
    int deviceId = 0;
    float downsample = 0.25f;
    RvmBackend backend = RvmBackend::DirectML;

    std::vector<float> alpha;
    std::array<float, 4> initialRec{};
    std::vector<Ort::Value> rec;

    // DirectML Alpha Packed GPU path. This is deliberately isolated from
    // the CPU backend and from the normal run() path.
    const OrtDmlApi* dmlApi = nullptr;
    std::unique_ptr<Ort::MemoryInfo> dmlMem;
    ComPtr<ID3D12CommandQueue> dmlQueue;
    ComPtr<ID3D12Device> d3d12Device;
    ComPtr<ID3D12CommandAllocator> preprocessAllocator;
    ComPtr<ID3D12GraphicsCommandList> preprocessList;
    ComPtr<ID3D12CommandAllocator> packAllocator;
    ComPtr<ID3D12GraphicsCommandList> packList;
    ComPtr<ID3D12Fence> fence;
    HANDLE fenceEvent = nullptr;
    UINT64 fenceValue = 0;

    ComPtr<ID3D12RootSignature> rootSignature;
    ComPtr<ID3D12PipelineState> preprocessPso;
    ComPtr<ID3D12PipelineState> yuv420pToRgbaPso;
    ComPtr<ID3D12PipelineState> nv12ToRgbaPso;
    ComPtr<ID3D12PipelineState> alphaPackPso;
    ComPtr<ID3D12PipelineState> chromaKeyPso;
    ComPtr<ID3D12PipelineState> rgbaToNv12Pso;
    ComPtr<ID3D12RootSignature> interopRootSignature;
    ComPtr<ID3D12PipelineState> nv12PitchedToRgbaPso;
    ComPtr<ID3D12PipelineState> rgbaToNv12PitchedPso;

    ComPtr<ID3D12Resource> uploadRgba;
    ComPtr<ID3D12Resource> gpuInputYuv;
    ComPtr<ID3D12Resource> gpuRgba;
    ComPtr<ID3D12Resource> gpuNchw;
    ComPtr<ID3D12Resource> gpuAlpha;
    ComPtr<ID3D12Resource> gpuPackedRgba;
    ComPtr<ID3D12Resource> gpuNv12;
    ComPtr<ID3D12Resource> readbackNv12;

    // D3D11VA <-> D3D12 interop for zero-copy Alpha Packed.
    // Output is a ring because a hardware encoder may still be reading
    // previous surfaces when the next frame is produced.
    static constexpr size_t kInteropOutputSurfaceCount = 8;
    ComPtr<ID3D11Device> interopD3d11Device;
    ComPtr<ID3D11DeviceContext> interopD3d11Context;
    ComPtr<ID3D11Device5> interopD3d11Device5;
    ComPtr<ID3D11DeviceContext4> interopD3d11Context4;
    ComPtr<ID3D11Fence> interopFence11;
    ComPtr<ID3D11Texture2D> interopInputD3d11;
    std::array<ComPtr<ID3D11Texture2D>, kInteropOutputSurfaceCount> interopOutputD3d11;
    ComPtr<ID3D11Query> interopCopyQuery;
    std::array<ComPtr<ID3D12CommandAllocator>, kInteropOutputSurfaceCount>
        interopPreprocessAllocators;
    std::array<ComPtr<ID3D12GraphicsCommandList>, kInteropOutputSurfaceCount>
        interopPreprocessLists;
    std::array<ComPtr<ID3D12CommandAllocator>, kInteropOutputSurfaceCount>
        interopPackAllocators;
    std::array<ComPtr<ID3D12GraphicsCommandList>, kInteropOutputSurfaceCount>
        interopPackLists;
    std::array<UINT64, kInteropOutputSurfaceCount> interopFrameCompleteValues{};
    bool interopGpuFenceReady = false;
    ComPtr<ID3D12Resource> interopInputD3d12;
    std::array<ComPtr<ID3D12Resource>, kInteropOutputSurfaceCount> interopOutputD3d12;
    size_t interopOutputCursor = 0;
    ComPtr<ID3D12Resource> gpuInputPitched;
    ComPtr<ID3D12Resource> gpuOutputPitched;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT interopInputFootprints[2]{};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT interopOutputFootprints[2]{};
    UINT interopInputRows[2]{};
    UINT interopOutputRows[2]{};
    UINT64 interopInputRowBytes[2]{};
    UINT64 interopOutputRowBytes[2]{};
    UINT64 interopInputTotalBytes = 0;
    UINT64 interopOutputTotalBytes = 0;
    D3D12_RESOURCE_STATES interopInputD3d12State = D3D12_RESOURCE_STATE_COMMON;
    std::array<D3D12_RESOURCE_STATES, kInteropOutputSurfaceCount>
        interopOutputD3d12State{};
    D3D12_RESOURCE_STATES gpuInputPitchedState = D3D12_RESOURCE_STATE_COPY_DEST;
    D3D12_RESOURCE_STATES gpuOutputPitchedState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    bool d3d11InteropReady = false;

    D3D12_RESOURCE_STATES gpuInputYuvState = D3D12_RESOURCE_STATE_COPY_DEST;
    D3D12_RESOURCE_STATES gpuRgbaState = D3D12_RESOURCE_STATE_COPY_DEST;
    D3D12_RESOURCE_STATES gpuPackedState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES gpuNv12State = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    void* dmlNchwWrapper = nullptr;
    void* dmlAlphaWrapper = nullptr;
    std::unique_ptr<Ort::Value> dmlNchwTensor;
    std::unique_ptr<Ort::Value> dmlAlphaTensor;
    bool gpuAlphaPackedReady = false;

    ~Impl() {
        dmlNchwTensor.reset();
        dmlAlphaTensor.reset();
        if (dmlApi && dmlNchwWrapper) {
            dmlApi->FreeGPUAllocation(dmlNchwWrapper);
            dmlNchwWrapper = nullptr;
        }
        if (dmlApi && dmlAlphaWrapper) {
            dmlApi->FreeGPUAllocation(dmlAlphaWrapper);
            dmlAlphaWrapper = nullptr;
        }
        if (fenceEvent) {
            CloseHandle(fenceEvent);
            fenceEvent = nullptr;
        }
    }
};

RvmOrt::RvmOrt() : impl_(std::make_unique<Impl>()) {}
RvmOrt::~RvmOrt() = default;

bool RvmOrt::initialize(const std::wstring& modelPath,
                        int width,
                        int height,
                        float downsampleRatio,
                        RvmBackend backend,
                        int dxgiDeviceId,
                        std::string& error) {
    try {
        impl_->width = width;
        impl_->height = height;
        impl_->downsample = downsampleRatio;
        impl_->deviceId = dxgiDeviceId;
        impl_->backend = backend;

        impl_->options.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
        impl_->options.DisableMemPattern();
        impl_->options.SetGraphOptimizationLevel(
            GraphOptimizationLevel::ORT_ENABLE_ALL);

        if (backend == RvmBackend::DirectML) {
            if (OrtStatus* status =
                    OrtSessionOptionsAppendExecutionProvider_DML(
                        impl_->options, dxgiDeviceId)) {
                error = "DirectML EP initialization failed: " +
                        ortStatusError(status);
                return false;
            }
        }
        // CPU backend: do not append another EP.
        // ONNX Runtime's built-in CPU EP is used.

        impl_->session = std::make_unique<Ort::Session>(
            impl_->env, modelPath.c_str(), impl_->options);

        if (!hasOfficialRvmInputs(*impl_->session, error)) return false;

        const size_t pixels =
            static_cast<size_t>(width) * static_cast<size_t>(height);
        impl_->alpha.assign(pixels, 0.0f);

        return resetState(error);
    } catch (const Ort::Exception& e) {
        error = ortError(e);
        return false;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

bool RvmOrt::resetState(std::string& error) {
    try {
        impl_->rec.clear();
        impl_->rec.reserve(4);
        impl_->initialRec.fill(0.0f);

        const std::vector<int64_t> shape{1, 1, 1, 1};
        for (float& value : impl_->initialRec) {
            impl_->rec.emplace_back(makeCpuTensor(
                impl_->cpuMem, &value, 1, shape));
        }
        return true;
    } catch (const Ort::Exception& e) {
        error = ortError(e);
        return false;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

bool RvmOrt::run(const float* srcFp32Nchw,
                 RvmOutput& output,
                 std::string& error) {
    try {
        const size_t pixels =
            static_cast<size_t>(impl_->width) *
            static_cast<size_t>(impl_->height);

        const std::vector<int64_t> srcShape{
            1, 3, impl_->height, impl_->width};
        auto src = makeCpuTensor(
            impl_->cpuMem,
            const_cast<float*>(srcFp32Nchw),
            pixels * 3,
            srcShape);

        const std::vector<int64_t> alphaShape{
            1, 1, impl_->height, impl_->width};
        auto alpha = makeCpuTensor(
            impl_->cpuMem,
            impl_->alpha.data(),
            pixels,
            alphaShape);

        const std::vector<int64_t> ratioShape{1};
        auto ratio = makeCpuTensor(
            impl_->cpuMem,
            &impl_->downsample,
            1,
            ratioShape);

        Ort::IoBinding binding(*impl_->session);
        binding.BindInput("src", src);
        binding.BindInput("r1i", impl_->rec[0]);
        binding.BindInput("r2i", impl_->rec[1]);
        binding.BindInput("r3i", impl_->rec[2]);
        binding.BindInput("r4i", impl_->rec[3]);
        binding.BindInput("downsample_ratio", ratio);

        binding.BindOutput("pha", alpha);
        binding.BindOutput("r1o", impl_->cpuMem);
        binding.BindOutput("r2o", impl_->cpuMem);
        binding.BindOutput("r3o", impl_->cpuMem);
        binding.BindOutput("r4o", impl_->cpuMem);

        Ort::RunOptions runOptions;
        impl_->session->Run(runOptions, binding);

        auto outputs = binding.GetOutputValues();
        if (outputs.size() != 5) {
            error =
                "RVM returned unexpected output count: " +
                std::to_string(outputs.size());
            return false;
        }

        std::vector<Ort::Value> nextRec;
        nextRec.reserve(4);
        for (size_t i = 1; i < outputs.size(); ++i) {
            nextRec.emplace_back(std::move(outputs[i]));
        }
        impl_->rec = std::move(nextRec);

        output.alphaFp32 = impl_->alpha.data();
        return true;
    } catch (const Ort::Exception& e) {
        error = ortError(e);
        return false;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

bool RvmOrt::runGpuCompositeNv12(RvmGpuCompositeMode outputMode,
                                 RvmAlphaInputFormat inputFormat,
                                 const uint8_t* plane0, int stride0,
                                 const uint8_t* plane1, int stride1,
                                 const uint8_t* plane2, int stride2,
                                 uint8_t* dstY, int dstYStride,
                                 uint8_t* dstUV, int dstUVStride,
                                 std::string& error,
                                 void* d3d11SourceTexture,
                                 int d3d11SourceArraySlice,
                                 void** d3d11OutputTexture,
                                 void* d3d11DestinationTexture,
                                 int d3d11DestinationArraySlice) {
    const bool d3d11Interop =
        d3d11SourceTexture != nullptr &&
        (d3d11OutputTexture != nullptr || d3d11DestinationTexture != nullptr);
    if ((!d3d11Interop && (!plane0 || !dstY || !dstUV)) ||
        (d3d11Interop &&
         (d3d11SourceArraySlice < 0 || d3d11DestinationArraySlice < 0))) {
        error = "runAlphaPackedNv12: invalid source/destination";
        return false;
    }
    if (!d3d11Interop && inputFormat == RvmAlphaInputFormat::Yuv420p && (!plane1 || !plane2)) {
        error = "runAlphaPackedNv12: YUV420P planes missing";
        return false;
    }
    if (!d3d11Interop && inputFormat == RvmAlphaInputFormat::Nv12 && !plane1) {
        error = "runAlphaPackedNv12: NV12 UV plane missing";
        return false;
    }
    if (impl_->backend != RvmBackend::DirectML) {
        error = "runAlphaPackedNv12 requires DirectML backend";
        return false;
    }
    if ((impl_->width & 1) != 0 || (impl_->height & 1) != 0) {
        error = "runAlphaPackedNv12 requires even width/height";
        return false;
    }
    if (!d3d11Interop && (dstYStride < impl_->width || dstUVStride < impl_->width)) {
        error = "runAlphaPackedNv12: destination stride is too small";
        return false;
    }

    try {
        const size_t pixels =
            static_cast<size_t>(impl_->width) *
            static_cast<size_t>(impl_->height);
        const size_t rgbaBytes = pixels * 4u;
        const size_t yuv420Bytes = pixels + pixels / 2u;
        const size_t nchwBytes = pixels * 3u * sizeof(float);
        const size_t alphaBytes = pixels * sizeof(float);
        const size_t nv12Bytes = pixels + pixels / 2u;

        if (!impl_->gpuAlphaPackedReady) {
            const void* rawApi = nullptr;
            if (OrtStatus* status = Ort::GetApi().GetExecutionProviderApi(
                    "DML",
                    ORT_API_VERSION,
                    &rawApi)) {
                error = "GetExecutionProviderApi(DML) failed: " +
                        ortStatusError(status);
                return false;
            }
            impl_->dmlApi = static_cast<const OrtDmlApi*>(rawApi);
            if (!impl_->dmlApi) {
                error = "DirectML provider API unavailable";
                return false;
            }

            ID3D12CommandQueue* rawQueue = nullptr;
            if (OrtStatus* status = impl_->dmlApi->GetDMLCommandQueue(
                    impl_->options,
                    &rawQueue)) {
                error = "GetDMLCommandQueue failed: " + ortStatusError(status);
                return false;
            }
            if (!rawQueue) {
                error = "DirectML command queue unavailable";
                return false;
            }
            rawQueue->AddRef();
            impl_->dmlQueue.Attach(rawQueue);

            HRESULT hr = impl_->dmlQueue->GetDevice(
                IID_PPV_ARGS(&impl_->d3d12Device));
            if (FAILED(hr)) {
                error = hrError("ID3D12CommandQueue::GetDevice", hr);
                return false;
            }

            hr = impl_->d3d12Device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_PPV_ARGS(&impl_->preprocessAllocator));
            if (FAILED(hr)) {
                error = hrError("CreateCommandAllocator(preprocess)", hr);
                return false;
            }
            hr = impl_->d3d12Device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_PPV_ARGS(&impl_->packAllocator));
            if (FAILED(hr)) {
                error = hrError("CreateCommandAllocator(alpha-pack)", hr);
                return false;
            }

            hr = impl_->d3d12Device->CreateCommandList(
                0,
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                impl_->preprocessAllocator.Get(),
                nullptr,
                IID_PPV_ARGS(&impl_->preprocessList));
            if (FAILED(hr)) {
                error = hrError("CreateCommandList(preprocess)", hr);
                return false;
            }
            impl_->preprocessList->Close();

            hr = impl_->d3d12Device->CreateCommandList(
                0,
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                impl_->packAllocator.Get(),
                nullptr,
                IID_PPV_ARGS(&impl_->packList));
            if (FAILED(hr)) {
                error = hrError("CreateCommandList(alpha-pack)", hr);
                return false;
            }
            impl_->packList->Close();

            hr = impl_->d3d12Device->CreateFence(
                0,
                D3D12_FENCE_FLAG_SHARED,
                IID_PPV_ARGS(&impl_->fence));
            if (FAILED(hr)) {
                // Older/limited drivers can still use the existing interop
                // fallback; only the fully asynchronous path needs sharing.
                hr = impl_->d3d12Device->CreateFence(
                    0,
                    D3D12_FENCE_FLAG_NONE,
                    IID_PPV_ARGS(&impl_->fence));
            }
            if (FAILED(hr)) {
                error = hrError("CreateFence", hr);
                return false;
            }
            impl_->fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!impl_->fenceEvent) {
                error = "CreateEventW failed";
                return false;
            }

            D3D12_ROOT_PARAMETER params[4]{};
            params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
            params[0].Descriptor.ShaderRegister = 0;
            params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
            params[1].Descriptor.ShaderRegister = 1;
            params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
            params[2].Descriptor.ShaderRegister = 0;
            params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            params[3].Constants.ShaderRegister = 0;
            params[3].Constants.Num32BitValues = 2;
            params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

            D3D12_ROOT_SIGNATURE_DESC rootDesc{};
            rootDesc.NumParameters = 4;
            rootDesc.pParameters = params;
            rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

            ComPtr<ID3DBlob> rootBlob;
            ComPtr<ID3DBlob> rootErrors;
            hr = D3D12SerializeRootSignature(
                &rootDesc,
                D3D_ROOT_SIGNATURE_VERSION_1,
                &rootBlob,
                &rootErrors);
            if (FAILED(hr)) {
                if (rootErrors && rootErrors->GetBufferPointer()) {
                    error.assign(
                        static_cast<const char*>(rootErrors->GetBufferPointer()),
                        rootErrors->GetBufferSize());
                } else {
                    error = hrError("D3D12SerializeRootSignature", hr);
                }
                return false;
            }

            hr = impl_->d3d12Device->CreateRootSignature(
                0,
                rootBlob->GetBufferPointer(),
                rootBlob->GetBufferSize(),
                IID_PPV_ARGS(&impl_->rootSignature));
            if (FAILED(hr)) {
                error = hrError("CreateRootSignature", hr);
                return false;
            }

            ComPtr<ID3DBlob> preprocessShader;
            ComPtr<ID3DBlob> yuv420pShader;
            ComPtr<ID3DBlob> nv12InputShader;
            ComPtr<ID3DBlob> nv12PitchedInputShader;
            ComPtr<ID3DBlob> packShader;
            ComPtr<ID3DBlob> chromaShader;
            ComPtr<ID3DBlob> nv12Shader;
            ComPtr<ID3DBlob> nv12PitchedOutputShader;
            if (!compileComputeShader(
                    kPreprocessShader, preprocessShader, error)) {
                return false;
            }
            if (!compileComputeShader(
                    kYuv420pToRgbaShader, yuv420pShader, error)) {
                return false;
            }
            if (!compileComputeShader(
                    kNv12ToRgbaShader, nv12InputShader, error)) {
                return false;
            }
            if (!compileComputeShader(
                    kNv12PitchedToRgbaShader, nv12PitchedInputShader, error)) {
                return false;
            }
            if (!compileComputeShader(
                    kAlphaPackedShader, packShader, error)) {
                return false;
            }
            if (!compileComputeShader(
                    kChromaKeyShader, chromaShader, error)) {
                return false;
            }
            if (!compileComputeShader(
                    kRgbaToNv12Shader, nv12Shader, error)) {
                return false;
            }
            if (!compileComputeShader(
                    kRgbaToNv12PitchedShader, nv12PitchedOutputShader, error)) {
                return false;
            }

            // Dedicated root signature for D3D11<->D3D12 NV12 interop.
            D3D12_ROOT_PARAMETER interopParams[3]{};
            interopParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
            interopParams[0].Descriptor.ShaderRegister = 0;
            interopParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            interopParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
            interopParams[1].Descriptor.ShaderRegister = 0;
            interopParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            interopParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            interopParams[2].Constants.ShaderRegister = 0;
            interopParams[2].Constants.Num32BitValues = 6;
            interopParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

            D3D12_ROOT_SIGNATURE_DESC interopRootDesc{};
            interopRootDesc.NumParameters = 3;
            interopRootDesc.pParameters = interopParams;
            interopRootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
            ComPtr<ID3DBlob> interopRootBlob;
            ComPtr<ID3DBlob> interopRootErrors;
            hr = D3D12SerializeRootSignature(
                &interopRootDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                &interopRootBlob, &interopRootErrors);
            if (FAILED(hr)) {
                if (interopRootErrors && interopRootErrors->GetBufferPointer()) {
                    error.assign(
                        static_cast<const char*>(interopRootErrors->GetBufferPointer()),
                        interopRootErrors->GetBufferSize());
                } else {
                    error = hrError("D3D12SerializeRootSignature(interop)", hr);
                }
                return false;
            }
            hr = impl_->d3d12Device->CreateRootSignature(
                0, interopRootBlob->GetBufferPointer(),
                interopRootBlob->GetBufferSize(),
                IID_PPV_ARGS(&impl_->interopRootSignature));
            if (FAILED(hr)) {
                error = hrError("CreateRootSignature(interop)", hr);
                return false;
            }

            D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
            pso.pRootSignature = impl_->rootSignature.Get();
            pso.CS.pShaderBytecode = preprocessShader->GetBufferPointer();
            pso.CS.BytecodeLength = preprocessShader->GetBufferSize();
            hr = impl_->d3d12Device->CreateComputePipelineState(
                &pso,
                IID_PPV_ARGS(&impl_->preprocessPso));
            if (FAILED(hr)) {
                error = hrError("CreateComputePipelineState(preprocess)", hr);
                return false;
            }

            pso.CS.pShaderBytecode = yuv420pShader->GetBufferPointer();
            pso.CS.BytecodeLength = yuv420pShader->GetBufferSize();
            hr = impl_->d3d12Device->CreateComputePipelineState(
                &pso,
                IID_PPV_ARGS(&impl_->yuv420pToRgbaPso));
            if (FAILED(hr)) {
                error = hrError("CreateComputePipelineState(yuv420p-to-rgba)", hr);
                return false;
            }

            pso.CS.pShaderBytecode = nv12InputShader->GetBufferPointer();
            pso.CS.BytecodeLength = nv12InputShader->GetBufferSize();
            hr = impl_->d3d12Device->CreateComputePipelineState(
                &pso,
                IID_PPV_ARGS(&impl_->nv12ToRgbaPso));
            if (FAILED(hr)) {
                error = hrError("CreateComputePipelineState(nv12-to-rgba)", hr);
                return false;
            }

            pso.CS.pShaderBytecode = packShader->GetBufferPointer();
            pso.CS.BytecodeLength = packShader->GetBufferSize();
            hr = impl_->d3d12Device->CreateComputePipelineState(
                &pso,
                IID_PPV_ARGS(&impl_->alphaPackPso));
            if (FAILED(hr)) {
                error = hrError("CreateComputePipelineState(alpha-pack)", hr);
                return false;
            }

            pso.CS.pShaderBytecode = chromaShader->GetBufferPointer();
            pso.CS.BytecodeLength = chromaShader->GetBufferSize();
            hr = impl_->d3d12Device->CreateComputePipelineState(
                &pso,
                IID_PPV_ARGS(&impl_->chromaKeyPso));
            if (FAILED(hr)) {
                error = hrError("CreateComputePipelineState(chroma-key)", hr);
                return false;
            }

            pso.CS.pShaderBytecode = nv12Shader->GetBufferPointer();
            pso.CS.BytecodeLength = nv12Shader->GetBufferSize();
            hr = impl_->d3d12Device->CreateComputePipelineState(
                &pso,
                IID_PPV_ARGS(&impl_->rgbaToNv12Pso));
            if (FAILED(hr)) {
                error = hrError("CreateComputePipelineState(rgba-to-nv12)", hr);
                return false;
            }

            pso.pRootSignature = impl_->interopRootSignature.Get();
            pso.CS.pShaderBytecode = nv12PitchedInputShader->GetBufferPointer();
            pso.CS.BytecodeLength = nv12PitchedInputShader->GetBufferSize();
            hr = impl_->d3d12Device->CreateComputePipelineState(
                &pso, IID_PPV_ARGS(&impl_->nv12PitchedToRgbaPso));
            if (FAILED(hr)) {
                error = hrError("CreateComputePipelineState(nv12-pitched-to-rgba)", hr);
                return false;
            }
            pso.CS.pShaderBytecode = nv12PitchedOutputShader->GetBufferPointer();
            pso.CS.BytecodeLength = nv12PitchedOutputShader->GetBufferSize();
            hr = impl_->d3d12Device->CreateComputePipelineState(
                &pso, IID_PPV_ARGS(&impl_->rgbaToNv12PitchedPso));
            if (FAILED(hr)) {
                error = hrError("CreateComputePipelineState(rgba-to-nv12-pitched)", hr);
                return false;
            }

            impl_->uploadRgba = createBuffer(
                impl_->d3d12Device.Get(),
                rgbaBytes,
                D3D12_HEAP_TYPE_UPLOAD,
                D3D12_RESOURCE_STATE_GENERIC_READ,
                D3D12_RESOURCE_FLAG_NONE,
                error);
            impl_->gpuInputYuv = createBuffer(
                impl_->d3d12Device.Get(),
                yuv420Bytes,
                D3D12_HEAP_TYPE_DEFAULT,
                D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_FLAG_NONE,
                error);
            impl_->gpuRgba = createBuffer(
                impl_->d3d12Device.Get(),
                rgbaBytes,
                D3D12_HEAP_TYPE_DEFAULT,
                D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                error);
            impl_->gpuNchw = createBuffer(
                impl_->d3d12Device.Get(),
                nchwBytes,
                D3D12_HEAP_TYPE_DEFAULT,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                error);
            impl_->gpuAlpha = createBuffer(
                impl_->d3d12Device.Get(),
                alphaBytes,
                D3D12_HEAP_TYPE_DEFAULT,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                error);
            impl_->gpuPackedRgba = createBuffer(
                impl_->d3d12Device.Get(),
                rgbaBytes,
                D3D12_HEAP_TYPE_DEFAULT,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                error);
            impl_->gpuNv12 = createBuffer(
                impl_->d3d12Device.Get(),
                nv12Bytes,
                D3D12_HEAP_TYPE_DEFAULT,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                error);
            impl_->readbackNv12 = createBuffer(
                impl_->d3d12Device.Get(),
                nv12Bytes,
                D3D12_HEAP_TYPE_READBACK,
                D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_FLAG_NONE,
                error);

            if (!impl_->uploadRgba || !impl_->gpuInputYuv || !impl_->gpuRgba || !impl_->gpuNchw ||
                !impl_->gpuAlpha || !impl_->gpuPackedRgba ||
                !impl_->gpuNv12 || !impl_->readbackNv12) {
                if (error.empty()) error = "DirectML GPU buffer creation failed";
                return false;
            }

            impl_->dmlMem = std::make_unique<Ort::MemoryInfo>(
                "DML",
                OrtDeviceAllocator,
                0,
                OrtMemTypeDefault);

            if (OrtStatus* status =
                    impl_->dmlApi->CreateGPUAllocationFromD3DResource(
                        impl_->gpuNchw.Get(),
                        &impl_->dmlNchwWrapper)) {
                error = "CreateGPUAllocationFromD3DResource(src) failed: " +
                        ortStatusError(status);
                return false;
            }
            if (OrtStatus* status =
                    impl_->dmlApi->CreateGPUAllocationFromD3DResource(
                        impl_->gpuAlpha.Get(),
                        &impl_->dmlAlphaWrapper)) {
                error = "CreateGPUAllocationFromD3DResource(alpha) failed: " +
                        ortStatusError(status);
                return false;
            }

            const std::vector<int64_t> srcShape{
                1, 3, impl_->height, impl_->width};
            const std::vector<int64_t> alphaShape{
                1, 1, impl_->height, impl_->width};

            impl_->dmlNchwTensor = std::make_unique<Ort::Value>(
                Ort::Value::CreateTensor(
                    *impl_->dmlMem,
                    impl_->dmlNchwWrapper,
                    nchwBytes,
                    srcShape.data(),
                    srcShape.size(),
                    ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT));
            impl_->dmlAlphaTensor = std::make_unique<Ort::Value>(
                Ort::Value::CreateTensor(
                    *impl_->dmlMem,
                    impl_->dmlAlphaWrapper,
                    alphaBytes,
                    alphaShape.data(),
                    alphaShape.size(),
                    ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT));

            impl_->gpuAlphaPackedReady = true;
        }

        if (d3d11Interop && !impl_->d3d11InteropReady) {
            auto* sourceTexture = static_cast<ID3D11Texture2D*>(d3d11SourceTexture);
            D3D11_TEXTURE2D_DESC srcDesc{};
            sourceTexture->GetDesc(&srcDesc);
            if (srcDesc.Format != DXGI_FORMAT_NV12 ||
                static_cast<int>(srcDesc.Width) != impl_->width ||
                static_cast<int>(srcDesc.Height) != impl_->height) {
                error = "D3D11 zero-copy input requires NV12 texture matching video size";
                return false;
            }
            ID3D11Device* rawD3d11Device = nullptr;
            sourceTexture->GetDevice(&rawD3d11Device);
            impl_->interopD3d11Device.Attach(rawD3d11Device);
            if (!impl_->interopD3d11Device) {
                error = "D3D11 zero-copy input device unavailable";
                return false;
            }
            ID3D11DeviceContext* rawD3d11Context = nullptr;
            impl_->interopD3d11Device->GetImmediateContext(&rawD3d11Context);
            impl_->interopD3d11Context.Attach(rawD3d11Context);
            if (!impl_->interopD3d11Context) {
                error = "D3D11 zero-copy immediate context unavailable";
                return false;
            }

            ComPtr<ID3D11Device1> d3d11Device1;
            HRESULT hr11 = impl_->interopD3d11Device.As(&d3d11Device1);
            if (FAILED(hr11) || !d3d11Device1) {
                error = hrError("QueryInterface(ID3D11Device1)", hr11);
                return false;
            }

            D3D12_HEAP_PROPERTIES sharedHeap{};
            sharedHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
            sharedHeap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
            sharedHeap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
            sharedHeap.CreationNodeMask = 1;
            sharedHeap.VisibleNodeMask = 1;

            D3D12_RESOURCE_DESC sharedDesc12{};
            sharedDesc12.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            sharedDesc12.Width = static_cast<UINT64>(impl_->width);
            sharedDesc12.Height = static_cast<UINT>(impl_->height);
            sharedDesc12.DepthOrArraySize = 1;
            sharedDesc12.MipLevels = 1;
            sharedDesc12.Format = DXGI_FORMAT_NV12;
            sharedDesc12.SampleDesc.Count = 1;
            sharedDesc12.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            sharedDesc12.Flags = D3D12_RESOURCE_FLAG_NONE;

            auto createSharedPair = [&](ComPtr<ID3D12Resource>& resource12,
                                        ComPtr<ID3D11Texture2D>& texture11,
                                        const char* label) -> bool {
                HRESULT localHr = impl_->d3d12Device->CreateCommittedResource(
                    &sharedHeap,
                    D3D12_HEAP_FLAG_SHARED,
                    &sharedDesc12,
                    D3D12_RESOURCE_STATE_COMMON,
                    nullptr,
                    IID_PPV_ARGS(&resource12));
                if (FAILED(localHr)) {
                    error = hrError(label, localHr);
                    return false;
                }
                HANDLE sharedHandle = nullptr;
                localHr = impl_->d3d12Device->CreateSharedHandle(
                    resource12.Get(), nullptr, GENERIC_ALL, nullptr, &sharedHandle);
                if (FAILED(localHr) || !sharedHandle) {
                    error = hrError("CreateSharedHandle(D3D12 texture)", localHr);
                    return false;
                }
                localHr = d3d11Device1->OpenSharedResource1(
                    sharedHandle, IID_PPV_ARGS(&texture11));
                CloseHandle(sharedHandle);
                if (FAILED(localHr)) {
                    error = hrError("OpenSharedResource1(D3D11 texture)", localHr);
                    return false;
                }
                return true;
            };

            if (!createSharedPair(
                    impl_->interopInputD3d12,
                    impl_->interopInputD3d11,
                    "CreateCommittedResource(D3D12 interop input)")) {
                return false;
            }
            for (size_t i = 0; i < Impl::kInteropOutputSurfaceCount; ++i) {
                if (!createSharedPair(
                        impl_->interopOutputD3d12[i],
                        impl_->interopOutputD3d11[i],
                        "CreateCommittedResource(D3D12 interop output)")) {
                    return false;
                }
                impl_->interopOutputD3d12State[i] = D3D12_RESOURCE_STATE_COMMON;
            }
            impl_->interopOutputCursor = 0;

            // Share the DirectML D3D12 fence with D3D11. On current
            // Windows 10/11 drivers this lets both APIs synchronize entirely
            // on the GPU timeline instead of polling/waiting on the CPU.
            HRESULT device5Hr = impl_->interopD3d11Device.As(
                &impl_->interopD3d11Device5);
            HRESULT context4Hr = impl_->interopD3d11Context.As(
                &impl_->interopD3d11Context4);
            if (SUCCEEDED(device5Hr) && SUCCEEDED(context4Hr) &&
                impl_->interopD3d11Device5 && impl_->interopD3d11Context4) {
                HANDLE sharedFenceHandle = nullptr;
                HRESULT sharedHr = impl_->d3d12Device->CreateSharedHandle(
                    impl_->fence.Get(), nullptr, GENERIC_ALL, nullptr,
                    &sharedFenceHandle);
                if (SUCCEEDED(sharedHr) && sharedFenceHandle) {
                    sharedHr = impl_->interopD3d11Device5->OpenSharedFence(
                        sharedFenceHandle, IID_PPV_ARGS(&impl_->interopFence11));
                    CloseHandle(sharedFenceHandle);
                    if (SUCCEEDED(sharedHr) && impl_->interopFence11) {
                        impl_->interopGpuFenceReady = true;
                    }
                }
            }

            if (!impl_->interopGpuFenceReady) {
                // Compatibility fallback only. The normal DirectML realtime
                // path uses the shared fence and never enters this CPU poll.
                D3D11_QUERY_DESC queryDesc{};
                queryDesc.Query = D3D11_QUERY_EVENT;
                hr11 = impl_->interopD3d11Device->CreateQuery(
                    &queryDesc, impl_->interopCopyQuery.ReleaseAndGetAddressOf());
                if (FAILED(hr11)) {
                    error = hrError("CreateQuery(D3D11 interop fallback)", hr11);
                    return false;
                }
            } else {
                // Keep several command allocators in flight. This is required
                // once the per-frame CPU fence wait is removed.
                for (size_t i = 0; i < Impl::kInteropOutputSurfaceCount; ++i) {
                    HRESULT localHr = impl_->d3d12Device->CreateCommandAllocator(
                        D3D12_COMMAND_LIST_TYPE_DIRECT,
                        IID_PPV_ARGS(&impl_->interopPreprocessAllocators[i]));
                    if (FAILED(localHr)) {
                        error = hrError("CreateCommandAllocator(interop preprocess)", localHr);
                        return false;
                    }
                    localHr = impl_->d3d12Device->CreateCommandList(
                        0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                        impl_->interopPreprocessAllocators[i].Get(), nullptr,
                        IID_PPV_ARGS(&impl_->interopPreprocessLists[i]));
                    if (FAILED(localHr)) {
                        error = hrError("CreateCommandList(interop preprocess)", localHr);
                        return false;
                    }
                    impl_->interopPreprocessLists[i]->Close();

                    localHr = impl_->d3d12Device->CreateCommandAllocator(
                        D3D12_COMMAND_LIST_TYPE_DIRECT,
                        IID_PPV_ARGS(&impl_->interopPackAllocators[i]));
                    if (FAILED(localHr)) {
                        error = hrError("CreateCommandAllocator(interop pack)", localHr);
                        return false;
                    }
                    localHr = impl_->d3d12Device->CreateCommandList(
                        0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                        impl_->interopPackAllocators[i].Get(), nullptr,
                        IID_PPV_ARGS(&impl_->interopPackLists[i]));
                    if (FAILED(localHr)) {
                        error = hrError("CreateCommandList(interop pack)", localHr);
                        return false;
                    }
                    impl_->interopPackLists[i]->Close();
                }
            }

            const D3D12_RESOURCE_DESC inputDesc = impl_->interopInputD3d12->GetDesc();
            impl_->d3d12Device->GetCopyableFootprints(
                &inputDesc, 0, 2, 0,
                impl_->interopInputFootprints,
                impl_->interopInputRows,
                impl_->interopInputRowBytes,
                &impl_->interopInputTotalBytes);
            const D3D12_RESOURCE_DESC outputDesc = impl_->interopOutputD3d12[0]->GetDesc();
            impl_->d3d12Device->GetCopyableFootprints(
                &outputDesc, 0, 2, 0,
                impl_->interopOutputFootprints,
                impl_->interopOutputRows,
                impl_->interopOutputRowBytes,
                &impl_->interopOutputTotalBytes);

            impl_->gpuInputPitched = createBuffer(
                impl_->d3d12Device.Get(),
                static_cast<size_t>(impl_->interopInputTotalBytes),
                D3D12_HEAP_TYPE_DEFAULT,
                D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_FLAG_NONE,
                error);
            impl_->gpuOutputPitched = createBuffer(
                impl_->d3d12Device.Get(),
                static_cast<size_t>(impl_->interopOutputTotalBytes),
                D3D12_HEAP_TYPE_DEFAULT,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                error);
            if (!impl_->gpuInputPitched || !impl_->gpuOutputPitched) {
                if (error.empty()) error = "D3D11/D3D12 interop buffer creation failed";
                return false;
            }
            impl_->d3d11InteropReady = true;
        }

        size_t interopSlot = 0;
        ID3D12CommandAllocator* preprocessAllocator = impl_->preprocessAllocator.Get();
        ID3D12GraphicsCommandList* preprocessList = impl_->preprocessList.Get();
        ID3D12CommandAllocator* packAllocator = impl_->packAllocator.Get();
        ID3D12GraphicsCommandList* packList = impl_->packList.Get();

        if (d3d11Interop) {
            interopSlot =
                impl_->interopOutputCursor++ % Impl::kInteropOutputSurfaceCount;
            if (impl_->interopGpuFenceReady) {
                const UINT64 reusable =
                    impl_->interopFrameCompleteValues[interopSlot];
                if (reusable != 0 && impl_->fence->GetCompletedValue() < reusable) {
                    HRESULT waitHr = impl_->fence->SetEventOnCompletion(
                        reusable, impl_->fenceEvent);
                    if (FAILED(waitHr)) {
                        error = hrError("SetEventOnCompletion(interop ring)", waitHr);
                        return false;
                    }
                    // Backpressure only if the GPU is more than the ring depth
                    // behind; there is no normal per-frame CPU wait.
                    WaitForSingleObject(impl_->fenceEvent, INFINITE);
                }
                preprocessAllocator =
                    impl_->interopPreprocessAllocators[interopSlot].Get();
                preprocessList = impl_->interopPreprocessLists[interopSlot].Get();
                packAllocator = impl_->interopPackAllocators[interopSlot].Get();
                packList = impl_->interopPackLists[interopSlot].Get();
            }
        }

        HRESULT hr = S_OK;
        if (!d3d11Interop) {
            void* mapped = nullptr;
            D3D12_RANGE noRead{0, 0};
            hr = impl_->uploadRgba->Map(0, &noRead, &mapped);
            if (FAILED(hr)) {
                error = hrError("uploadRgba->Map", hr);
                return false;
            }
            uint8_t* upload = static_cast<uint8_t*>(mapped);
            if (inputFormat == RvmAlphaInputFormat::Rgba8) {
                if (stride0 < impl_->width * 4) {
                    impl_->uploadRgba->Unmap(0, nullptr);
                    error = "runAlphaPackedNv12: RGBA stride too small";
                    return false;
                }
                for (int y = 0; y < impl_->height; ++y) {
                    std::memcpy(
                        upload + static_cast<size_t>(y) * impl_->width * 4u,
                        plane0 + static_cast<size_t>(y) * stride0,
                        static_cast<size_t>(impl_->width) * 4u);
                }
            } else if (inputFormat == RvmAlphaInputFormat::Yuv420p) {
                if (stride0 < impl_->width || stride1 < impl_->width / 2 ||
                    stride2 < impl_->width / 2) {
                    impl_->uploadRgba->Unmap(0, nullptr);
                    error = "runAlphaPackedNv12: YUV420P stride too small";
                    return false;
                }
                for (int y = 0; y < impl_->height; ++y) {
                    std::memcpy(upload + static_cast<size_t>(y) * impl_->width,
                                plane0 + static_cast<size_t>(y) * stride0,
                                static_cast<size_t>(impl_->width));
                }
                uint8_t* uDst = upload + pixels;
                uint8_t* vDst = uDst + pixels / 4u;
                for (int y = 0; y < impl_->height / 2; ++y) {
                    std::memcpy(uDst + static_cast<size_t>(y) * (impl_->width / 2),
                                plane1 + static_cast<size_t>(y) * stride1,
                                static_cast<size_t>(impl_->width / 2));
                    std::memcpy(vDst + static_cast<size_t>(y) * (impl_->width / 2),
                                plane2 + static_cast<size_t>(y) * stride2,
                                static_cast<size_t>(impl_->width / 2));
                }
            } else {
                if (stride0 < impl_->width || stride1 < impl_->width) {
                    impl_->uploadRgba->Unmap(0, nullptr);
                    error = "runAlphaPackedNv12: NV12 stride too small";
                    return false;
                }
                for (int y = 0; y < impl_->height; ++y) {
                    std::memcpy(upload + static_cast<size_t>(y) * impl_->width,
                                plane0 + static_cast<size_t>(y) * stride0,
                                static_cast<size_t>(impl_->width));
                }
                uint8_t* uvDst = upload + pixels;
                for (int y = 0; y < impl_->height / 2; ++y) {
                    std::memcpy(uvDst + static_cast<size_t>(y) * impl_->width,
                                plane1 + static_cast<size_t>(y) * stride1,
                                static_cast<size_t>(impl_->width));
                }
            }
            impl_->uploadRgba->Unmap(0, nullptr);

        } else {
            auto* sourceTexture = static_cast<ID3D11Texture2D*>(d3d11SourceTexture);
            const UINT sourceSubresource = D3D11CalcSubresource(
                0, static_cast<UINT>(d3d11SourceArraySlice), 1);
            impl_->interopD3d11Context->CopySubresourceRegion(
                impl_->interopInputD3d11.Get(), 0, 0, 0, 0,
                sourceTexture, sourceSubresource, nullptr);
            if (impl_->interopGpuFenceReady) {
                const UINT64 inputReady = ++impl_->fenceValue;
                HRESULT syncHr = impl_->interopD3d11Context4->Signal(
                    impl_->interopFence11.Get(), inputReady);
                if (FAILED(syncHr)) {
                    error = hrError("ID3D11DeviceContext4::Signal(input)", syncHr);
                    return false;
                }
                impl_->interopD3d11Context->Flush();
                syncHr = impl_->dmlQueue->Wait(impl_->fence.Get(), inputReady);
                if (FAILED(syncHr)) {
                    error = hrError("ID3D12CommandQueue::Wait(input)", syncHr);
                    return false;
                }
            } else {
                impl_->interopD3d11Context->End(impl_->interopCopyQuery.Get());
                impl_->interopD3d11Context->Flush();
                HRESULT queryHr = S_FALSE;
                while ((queryHr = impl_->interopD3d11Context->GetData(
                            impl_->interopCopyQuery.Get(), nullptr, 0, 0)) == S_FALSE) {
                    SwitchToThread();
                }
                if (FAILED(queryHr)) {
                    error = hrError("D3D11 interop copy synchronization", queryHr);
                    return false;
                }
            }
        }

        auto waitForGpu = [&]() -> bool {
            const UINT64 value = ++impl_->fenceValue;
            HRESULT localHr = impl_->dmlQueue->Signal(impl_->fence.Get(), value);
            if (FAILED(localHr)) {
                error = hrError("ID3D12CommandQueue::Signal", localHr);
                return false;
            }
            if (impl_->fence->GetCompletedValue() < value) {
                localHr = impl_->fence->SetEventOnCompletion(
                    value, impl_->fenceEvent);
                if (FAILED(localHr)) {
                    error = hrError("ID3D12Fence::SetEventOnCompletion", localHr);
                    return false;
                }
                WaitForSingleObject(impl_->fenceEvent, INFINITE);
            }
            return true;
        };

        hr = preprocessAllocator->Reset();
        if (FAILED(hr)) {
            error = hrError("commandAllocator->Reset(preprocess)", hr);
            return false;
        }
        hr = preprocessList->Reset(
            preprocessAllocator, impl_->preprocessPso.Get());
        if (FAILED(hr)) {
            error = hrError("commandList->Reset(preprocess)", hr);
            return false;
        }

        if (d3d11Interop) {
            if (impl_->interopInputD3d12State != D3D12_RESOURCE_STATE_COPY_SOURCE) {
                D3D12_RESOURCE_BARRIER b{};
                b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b.Transition.pResource = impl_->interopInputD3d12.Get();
                b.Transition.StateBefore = impl_->interopInputD3d12State;
                b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
                b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                preprocessList->ResourceBarrier(1, &b);
                impl_->interopInputD3d12State = D3D12_RESOURCE_STATE_COPY_SOURCE;
            }
            if (impl_->gpuInputPitchedState != D3D12_RESOURCE_STATE_COPY_DEST) {
                D3D12_RESOURCE_BARRIER b{};
                b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b.Transition.pResource = impl_->gpuInputPitched.Get();
                b.Transition.StateBefore = impl_->gpuInputPitchedState;
                b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
                b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                preprocessList->ResourceBarrier(1, &b);
                impl_->gpuInputPitchedState = D3D12_RESOURCE_STATE_COPY_DEST;
            }
            if (impl_->gpuRgbaState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
                D3D12_RESOURCE_BARRIER b{};
                b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b.Transition.pResource = impl_->gpuRgba.Get();
                b.Transition.StateBefore = impl_->gpuRgbaState;
                b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                preprocessList->ResourceBarrier(1, &b);
                impl_->gpuRgbaState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            }

            for (UINT plane = 0; plane < 2; ++plane) {
                D3D12_TEXTURE_COPY_LOCATION dst{};
                dst.pResource = impl_->gpuInputPitched.Get();
                dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                dst.PlacedFootprint = impl_->interopInputFootprints[plane];
                D3D12_TEXTURE_COPY_LOCATION src{};
                src.pResource = impl_->interopInputD3d12.Get();
                src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                src.SubresourceIndex = plane;
                preprocessList->CopyTextureRegion(
                    &dst, 0, 0, 0, &src, nullptr);
            }

            D3D12_RESOURCE_BARRIER inputBarriers[2]{};
            inputBarriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            inputBarriers[0].Transition.pResource = impl_->interopInputD3d12.Get();
            inputBarriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
            inputBarriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
            inputBarriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            inputBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            inputBarriers[1].Transition.pResource = impl_->gpuInputPitched.Get();
            inputBarriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            inputBarriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            inputBarriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            preprocessList->ResourceBarrier(2, inputBarriers);
            impl_->interopInputD3d12State = D3D12_RESOURCE_STATE_COMMON;
            impl_->gpuInputPitchedState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

            preprocessList->SetPipelineState(impl_->nv12PitchedToRgbaPso.Get());
            preprocessList->SetComputeRootSignature(impl_->interopRootSignature.Get());
            preprocessList->SetComputeRootShaderResourceView(
                0, impl_->gpuInputPitched->GetGPUVirtualAddress());
            preprocessList->SetComputeRootUnorderedAccessView(
                1, impl_->gpuRgba->GetGPUVirtualAddress());
            const UINT interopDims[6]{
                static_cast<UINT>(impl_->width),
                static_cast<UINT>(impl_->height),
                static_cast<UINT>(impl_->interopInputFootprints[0].Offset),
                static_cast<UINT>(impl_->interopInputFootprints[1].Offset),
                impl_->interopInputFootprints[0].Footprint.RowPitch,
                impl_->interopInputFootprints[1].Footprint.RowPitch};
            preprocessList->SetComputeRoot32BitConstants(2, 6, interopDims, 0);
            preprocessList->Dispatch(
                static_cast<UINT>((impl_->width + 15) / 16),
                static_cast<UINT>((impl_->height + 15) / 16),
                1);

            D3D12_RESOURCE_BARRIER rgbaBarriers[2]{};
            rgbaBarriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            rgbaBarriers[0].UAV.pResource = impl_->gpuRgba.Get();
            rgbaBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            rgbaBarriers[1].Transition.pResource = impl_->gpuRgba.Get();
            rgbaBarriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            rgbaBarriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            rgbaBarriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            preprocessList->ResourceBarrier(2, rgbaBarriers);
            impl_->gpuRgbaState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        } else if (inputFormat == RvmAlphaInputFormat::Rgba8) {
            if (impl_->gpuRgbaState != D3D12_RESOURCE_STATE_COPY_DEST) {
                D3D12_RESOURCE_BARRIER b{};
                b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b.Transition.pResource = impl_->gpuRgba.Get();
                b.Transition.StateBefore = impl_->gpuRgbaState;
                b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
                b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                preprocessList->ResourceBarrier(1, &b);
                impl_->gpuRgbaState = D3D12_RESOURCE_STATE_COPY_DEST;
            }

            preprocessList->CopyBufferRegion(
                impl_->gpuRgba.Get(), 0,
                impl_->uploadRgba.Get(), 0,
                rgbaBytes);

            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = impl_->gpuRgba.Get();
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            preprocessList->ResourceBarrier(1, &b);
            impl_->gpuRgbaState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        } else {
            if (impl_->gpuInputYuvState != D3D12_RESOURCE_STATE_COPY_DEST) {
                D3D12_RESOURCE_BARRIER b{};
                b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b.Transition.pResource = impl_->gpuInputYuv.Get();
                b.Transition.StateBefore = impl_->gpuInputYuvState;
                b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
                b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                preprocessList->ResourceBarrier(1, &b);
                impl_->gpuInputYuvState = D3D12_RESOURCE_STATE_COPY_DEST;
            }
            if (impl_->gpuRgbaState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
                D3D12_RESOURCE_BARRIER b{};
                b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b.Transition.pResource = impl_->gpuRgba.Get();
                b.Transition.StateBefore = impl_->gpuRgbaState;
                b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                preprocessList->ResourceBarrier(1, &b);
                impl_->gpuRgbaState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            }

            preprocessList->CopyBufferRegion(
                impl_->gpuInputYuv.Get(), 0,
                impl_->uploadRgba.Get(), 0,
                yuv420Bytes);

            {
                D3D12_RESOURCE_BARRIER b{};
                b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b.Transition.pResource = impl_->gpuInputYuv.Get();
                b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                b.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                preprocessList->ResourceBarrier(1, &b);
                impl_->gpuInputYuvState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            }

            preprocessList->SetPipelineState(
                inputFormat == RvmAlphaInputFormat::Yuv420p
                    ? impl_->yuv420pToRgbaPso.Get()
                    : impl_->nv12ToRgbaPso.Get());
            preprocessList->SetComputeRootSignature(impl_->rootSignature.Get());
            preprocessList->SetComputeRootShaderResourceView(
                0, impl_->gpuInputYuv->GetGPUVirtualAddress());
            preprocessList->SetComputeRootShaderResourceView(
                1, impl_->gpuInputYuv->GetGPUVirtualAddress());
            preprocessList->SetComputeRootUnorderedAccessView(
                2, impl_->gpuRgba->GetGPUVirtualAddress());
            const UINT yuvDims[2]{
                static_cast<UINT>(impl_->width),
                static_cast<UINT>(impl_->height)};
            preprocessList->SetComputeRoot32BitConstants(3, 2, yuvDims, 0);
            preprocessList->Dispatch(
                static_cast<UINT>((impl_->width + 15) / 16),
                static_cast<UINT>((impl_->height + 15) / 16),
                1);

            D3D12_RESOURCE_BARRIER barriers[2]{};
            barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            barriers[0].UAV.pResource = impl_->gpuRgba.Get();
            barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barriers[1].Transition.pResource = impl_->gpuRgba.Get();
            barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            preprocessList->ResourceBarrier(2, barriers);
            impl_->gpuRgbaState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        }

        preprocessList->SetPipelineState(impl_->preprocessPso.Get());
        preprocessList->SetComputeRootSignature(impl_->rootSignature.Get());
        preprocessList->SetComputeRootShaderResourceView(
            0, impl_->gpuRgba->GetGPUVirtualAddress());
        preprocessList->SetComputeRootShaderResourceView(
            1, impl_->gpuRgba->GetGPUVirtualAddress());
        preprocessList->SetComputeRootUnorderedAccessView(
            2, impl_->gpuNchw->GetGPUVirtualAddress());
        const UINT dims[2]{
            static_cast<UINT>(impl_->width),
            static_cast<UINT>(impl_->height)};
        preprocessList->SetComputeRoot32BitConstants(3, 2, dims, 0);
        preprocessList->Dispatch(
            static_cast<UINT>((impl_->width + 15) / 16),
            static_cast<UINT>((impl_->height + 15) / 16),
            1);

        {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            b.UAV.pResource = impl_->gpuNchw.Get();
            preprocessList->ResourceBarrier(1, &b);
        }

        hr = preprocessList->Close();
        if (FAILED(hr)) {
            error = hrError("commandList->Close(preprocess)", hr);
            return false;
        }
        ID3D12CommandList* preprocessLists[]{preprocessList};
        impl_->dmlQueue->ExecuteCommandLists(1, preprocessLists);

        const std::vector<int64_t> ratioShape{1};
        auto ratio = makeCpuTensor(
            impl_->cpuMem,
            &impl_->downsample,
            1,
            ratioShape);

        Ort::IoBinding binding(*impl_->session);
        binding.BindInput("src", *impl_->dmlNchwTensor);
        binding.BindInput("r1i", impl_->rec[0]);
        binding.BindInput("r2i", impl_->rec[1]);
        binding.BindInput("r3i", impl_->rec[2]);
        binding.BindInput("r4i", impl_->rec[3]);
        binding.BindInput("downsample_ratio", ratio);

        binding.BindOutput("pha", *impl_->dmlAlphaTensor);
        binding.BindOutput("r1o", *impl_->dmlMem);
        binding.BindOutput("r2o", *impl_->dmlMem);
        binding.BindOutput("r3o", *impl_->dmlMem);
        binding.BindOutput("r4o", *impl_->dmlMem);

        // Preprocess, DirectML inference and alpha packing all use the same
        // DirectML command queue, so GPU queue ordering provides the dependency
        // chain without a CPU fence between the three stages.
        Ort::RunOptions runOptions;
        impl_->session->Run(runOptions, binding);

        auto outputs = binding.GetOutputValues();
        if (outputs.size() != 5) {
            error =
                "RVM returned unexpected output count: " +
                std::to_string(outputs.size());
            return false;
        }
        std::vector<Ort::Value> nextRec;
        nextRec.reserve(4);
        for (size_t i = 1; i < outputs.size(); ++i) {
            nextRec.emplace_back(std::move(outputs[i]));
        }
        impl_->rec = std::move(nextRec);

        hr = packAllocator->Reset();
        if (FAILED(hr)) {
            error = hrError("commandAllocator->Reset(alpha-pack)", hr);
            return false;
        }
        ID3D12PipelineState* compositePso =
            outputMode == RvmGpuCompositeMode::ChromaKey
                ? impl_->chromaKeyPso.Get()
                : impl_->alphaPackPso.Get();
        hr = packList->Reset(
            packAllocator, compositePso);
        if (FAILED(hr)) {
            error = hrError("commandList->Reset(alpha-pack)", hr);
            return false;
        }

        if (impl_->gpuPackedState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = impl_->gpuPackedRgba.Get();
            b.Transition.StateBefore = impl_->gpuPackedState;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            packList->ResourceBarrier(1, &b);
            impl_->gpuPackedState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        }

        {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = impl_->gpuAlpha.Get();
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            packList->ResourceBarrier(1, &b);
        }

        packList->SetComputeRootSignature(impl_->rootSignature.Get());
        packList->SetComputeRootShaderResourceView(
            0, impl_->gpuRgba->GetGPUVirtualAddress());
        packList->SetComputeRootShaderResourceView(
            1, impl_->gpuAlpha->GetGPUVirtualAddress());
        packList->SetComputeRootUnorderedAccessView(
            2, impl_->gpuPackedRgba->GetGPUVirtualAddress());
        packList->SetComputeRoot32BitConstants(3, 2, dims, 0);
        packList->Dispatch(
            static_cast<UINT>((impl_->width + 15) / 16),
            static_cast<UINT>((impl_->height + 15) / 16),
            1);

        {
            D3D12_RESOURCE_BARRIER barriers[3]{};
            barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            barriers[0].UAV.pResource = impl_->gpuPackedRgba.Get();
            barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barriers[1].Transition.pResource = impl_->gpuPackedRgba.Get();
            barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barriers[2].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barriers[2].Transition.pResource = impl_->gpuAlpha.Get();
            barriers[2].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            barriers[2].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            barriers[2].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            packList->ResourceBarrier(3, barriers);
            impl_->gpuPackedState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        }

        if (d3d11Interop) {
            const size_t outputSlot = interopSlot;
            ID3D12Resource* const interopOutput12 =
                impl_->interopOutputD3d12[outputSlot].Get();
            ID3D11Texture2D* const interopOutput11 =
                impl_->interopOutputD3d11[outputSlot].Get();
            D3D12_RESOURCE_STATES& interopOutputState =
                impl_->interopOutputD3d12State[outputSlot];

            if (impl_->gpuOutputPitchedState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
                D3D12_RESOURCE_BARRIER b{};
                b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b.Transition.pResource = impl_->gpuOutputPitched.Get();
                b.Transition.StateBefore = impl_->gpuOutputPitchedState;
                b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                packList->ResourceBarrier(1, &b);
                impl_->gpuOutputPitchedState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            }

            packList->SetPipelineState(impl_->rgbaToNv12PitchedPso.Get());
            packList->SetComputeRootSignature(impl_->interopRootSignature.Get());
            packList->SetComputeRootShaderResourceView(
                0, impl_->gpuPackedRgba->GetGPUVirtualAddress());
            packList->SetComputeRootUnorderedAccessView(
                1, impl_->gpuOutputPitched->GetGPUVirtualAddress());
            const UINT outputDims[6]{
                static_cast<UINT>(impl_->width),
                static_cast<UINT>(impl_->height),
                static_cast<UINT>(impl_->interopOutputFootprints[0].Offset),
                static_cast<UINT>(impl_->interopOutputFootprints[1].Offset),
                impl_->interopOutputFootprints[0].Footprint.RowPitch,
                impl_->interopOutputFootprints[1].Footprint.RowPitch};
            packList->SetComputeRoot32BitConstants(2, 6, outputDims, 0);
            const size_t yWordsPerRow = (static_cast<size_t>(impl_->width) + 3u) / 4u;
            const size_t outputWords =
                yWordsPerRow * static_cast<size_t>(impl_->height) +
                yWordsPerRow * static_cast<size_t>(impl_->height / 2);
            packList->Dispatch(
                static_cast<UINT>((outputWords + 255u) / 256u), 1, 1);

            D3D12_RESOURCE_BARRIER outputBarriers[3]{};
            outputBarriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            outputBarriers[0].UAV.pResource = impl_->gpuOutputPitched.Get();
            outputBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            outputBarriers[1].Transition.pResource = impl_->gpuOutputPitched.Get();
            outputBarriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            outputBarriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            outputBarriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            outputBarriers[2].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            outputBarriers[2].Transition.pResource = interopOutput12;
            outputBarriers[2].Transition.StateBefore = interopOutputState;
            outputBarriers[2].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            outputBarriers[2].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            packList->ResourceBarrier(3, outputBarriers);
            impl_->gpuOutputPitchedState = D3D12_RESOURCE_STATE_COPY_SOURCE;
            interopOutputState = D3D12_RESOURCE_STATE_COPY_DEST;

            for (UINT plane = 0; plane < 2; ++plane) {
                D3D12_TEXTURE_COPY_LOCATION src{};
                src.pResource = impl_->gpuOutputPitched.Get();
                src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                src.PlacedFootprint = impl_->interopOutputFootprints[plane];
                D3D12_TEXTURE_COPY_LOCATION dst{};
                dst.pResource = interopOutput12;
                dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                dst.SubresourceIndex = plane;
                packList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            }

            D3D12_RESOURCE_BARRIER handoffBarriers[3]{};
            handoffBarriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            handoffBarriers[0].Transition.pResource = impl_->gpuOutputPitched.Get();
            handoffBarriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
            handoffBarriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            handoffBarriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            handoffBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            handoffBarriers[1].Transition.pResource = interopOutput12;
            handoffBarriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            handoffBarriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
            handoffBarriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            handoffBarriers[2].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            handoffBarriers[2].Transition.pResource = impl_->gpuPackedRgba.Get();
            handoffBarriers[2].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            handoffBarriers[2].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            handoffBarriers[2].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            packList->ResourceBarrier(3, handoffBarriers);
            impl_->gpuOutputPitchedState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            interopOutputState = D3D12_RESOURCE_STATE_COMMON;
            impl_->gpuPackedState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

            hr = packList->Close();
            if (FAILED(hr)) {
                error = hrError("commandList->Close(alpha-pack-d3d11)", hr);
                return false;
            }
            ID3D12CommandList* packLists[]{packList};
            impl_->dmlQueue->ExecuteCommandLists(1, packLists);

            if (impl_->interopGpuFenceReady) {
                const UINT64 outputReady = ++impl_->fenceValue;
                HRESULT syncHr = impl_->dmlQueue->Signal(
                    impl_->fence.Get(), outputReady);
                if (FAILED(syncHr)) {
                    error = hrError("ID3D12CommandQueue::Signal(output)", syncHr);
                    return false;
                }
                syncHr = impl_->interopD3d11Context4->Wait(
                    impl_->interopFence11.Get(), outputReady);
                if (FAILED(syncHr)) {
                    error = hrError("ID3D11DeviceContext4::Wait(output)", syncHr);
                    return false;
                }

                if (d3d11DestinationTexture) {
                    auto* destinationTexture =
                        static_cast<ID3D11Texture2D*>(d3d11DestinationTexture);
                    impl_->interopD3d11Context->CopySubresourceRegion(
                        destinationTexture,
                        D3D11CalcSubresource(
                            0, static_cast<UINT>(d3d11DestinationArraySlice), 1),
                        0, 0, 0, interopOutput11, 0, nullptr);
                    const UINT64 destinationReady = ++impl_->fenceValue;
                    syncHr = impl_->interopD3d11Context4->Signal(
                        impl_->interopFence11.Get(), destinationReady);
                    if (FAILED(syncHr)) {
                        error = hrError("ID3D11DeviceContext4::Signal(output copy)", syncHr);
                        return false;
                    }
                    impl_->interopD3d11Context->Flush();
                    impl_->interopFrameCompleteValues[outputSlot] = destinationReady;
                } else {
                    // Compatibility callers that only request the shared texture
                    // keep the old completion semantics.
                    if (!waitForGpu()) return false;
                }
            } else {
                if (!waitForGpu()) return false;
                if (d3d11DestinationTexture) {
                    auto* destinationTexture =
                        static_cast<ID3D11Texture2D*>(d3d11DestinationTexture);
                    impl_->interopD3d11Context->CopySubresourceRegion(
                        destinationTexture,
                        D3D11CalcSubresource(
                            0, static_cast<UINT>(d3d11DestinationArraySlice), 1),
                        0, 0, 0, interopOutput11, 0, nullptr);
                    impl_->interopD3d11Context->Flush();
                }
            }

            if (d3d11OutputTexture) *d3d11OutputTexture = interopOutput11;
            return true;
        }

        // Keep the packed frame on GPU and convert it directly to NV12 there.
        if (impl_->gpuNv12State != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = impl_->gpuNv12.Get();
            b.Transition.StateBefore = impl_->gpuNv12State;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            packList->ResourceBarrier(1, &b);
            impl_->gpuNv12State = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        }

        packList->SetPipelineState(impl_->rgbaToNv12Pso.Get());
        packList->SetComputeRootSignature(impl_->rootSignature.Get());
        packList->SetComputeRootShaderResourceView(
            0, impl_->gpuPackedRgba->GetGPUVirtualAddress());
        packList->SetComputeRootShaderResourceView(
            1, impl_->gpuPackedRgba->GetGPUVirtualAddress());
        packList->SetComputeRootUnorderedAccessView(
            2, impl_->gpuNv12->GetGPUVirtualAddress());
        packList->SetComputeRoot32BitConstants(3, 2, dims, 0);

        const size_t yBytes = pixels;
        const size_t uvBytes = pixels / 2u;
        const size_t yWords = (yBytes + 3u) / 4u;
        const size_t uvWords = (uvBytes + 3u) / 4u;
        const size_t totalWords = yWords + uvWords;
        packList->Dispatch(
            static_cast<UINT>((totalWords + 255u) / 256u),
            1,
            1);

        {
            D3D12_RESOURCE_BARRIER barriers[3]{};
            barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            barriers[0].UAV.pResource = impl_->gpuNv12.Get();
            barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barriers[1].Transition.pResource = impl_->gpuNv12.Get();
            barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barriers[2].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barriers[2].Transition.pResource = impl_->gpuPackedRgba.Get();
            barriers[2].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            barriers[2].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            barriers[2].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            packList->ResourceBarrier(3, barriers);
            impl_->gpuNv12State = D3D12_RESOURCE_STATE_COPY_SOURCE;
            impl_->gpuPackedState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        }

        packList->CopyBufferRegion(
            impl_->readbackNv12.Get(), 0,
            impl_->gpuNv12.Get(), 0,
            nv12Bytes);

        hr = packList->Close();
        if (FAILED(hr)) {
            error = hrError("commandList->Close(alpha-pack-nv12)", hr);
            return false;
        }
        ID3D12CommandList* packLists[]{packList};
        impl_->dmlQueue->ExecuteCommandLists(1, packLists);
        if (!waitForGpu()) return false;

        void* readback = nullptr;
        D3D12_RANGE readRange{0, nv12Bytes};
        hr = impl_->readbackNv12->Map(0, &readRange, &readback);
        if (FAILED(hr)) {
            error = hrError("readbackNv12->Map", hr);
            return false;
        }

        const uint8_t* nv12 = static_cast<const uint8_t*>(readback);
        for (int y = 0; y < impl_->height; ++y) {
            std::memcpy(
                dstY + static_cast<size_t>(y) * dstYStride,
                nv12 + static_cast<size_t>(y) * impl_->width,
                static_cast<size_t>(impl_->width));
        }
        const uint8_t* uv = nv12 + pixels;
        for (int y = 0; y < impl_->height / 2; ++y) {
            std::memcpy(
                dstUV + static_cast<size_t>(y) * dstUVStride,
                uv + static_cast<size_t>(y) * impl_->width,
                static_cast<size_t>(impl_->width));
        }

        D3D12_RANGE writtenRange{0, 0};
        impl_->readbackNv12->Unmap(0, &writtenRange);

        return true;
    } catch (const Ort::Exception& e) {
        error = ortError(e);
        return false;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

bool RvmOrt::runAlphaPackedNv12(RvmAlphaInputFormat inputFormat,
                                const uint8_t* plane0, int stride0,
                                const uint8_t* plane1, int stride1,
                                const uint8_t* plane2, int stride2,
                                uint8_t* dstY, int dstYStride,
                                uint8_t* dstUV, int dstUVStride,
                                std::string& error,
                                void* d3d11SourceTexture,
                                int d3d11SourceArraySlice,
                                void** d3d11OutputTexture,
                                void* d3d11DestinationTexture,
                                int d3d11DestinationArraySlice) {
    return runGpuCompositeNv12(
        RvmGpuCompositeMode::AlphaPacked,
        inputFormat,
        plane0, stride0,
        plane1, stride1,
        plane2, stride2,
        dstY, dstYStride,
        dstUV, dstUVStride,
        error,
        d3d11SourceTexture,
        d3d11SourceArraySlice,
        d3d11OutputTexture,
        d3d11DestinationTexture,
        d3d11DestinationArraySlice);
}

