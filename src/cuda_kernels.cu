#include "cuda_kernels.h"

#include <cuda_fp16.h>
#include <math_constants.h>

namespace {

__device__ __forceinline__ float clamp01(float v) {
    return fminf(1.0f, fmaxf(0.0f, v));
}

__device__ __forceinline__ float bilerp(float a, float b, float c, float d, float tx, float ty) {
    const float ab = a + (b - a) * tx;
    const float cd = c + (d - c) * tx;
    return ab + (cd - ab) * ty;
}

__device__ float samplePlane(const uint8_t* p, int pitch, int width, int height, float x, float y) {
    x = fminf(fmaxf(x, 0.0f), static_cast<float>(width - 1));
    y = fminf(fmaxf(y, 0.0f), static_cast<float>(height - 1));
    const int x0 = static_cast<int>(floorf(x));
    const int y0 = static_cast<int>(floorf(y));
    const int x1 = min(x0 + 1, width - 1);
    const int y1 = min(y0 + 1, height - 1);
    const float tx = x - static_cast<float>(x0);
    const float ty = y - static_cast<float>(y0);
    const float a = static_cast<float>(p[y0 * pitch + x0]);
    const float b = static_cast<float>(p[y0 * pitch + x1]);
    const float c = static_cast<float>(p[y1 * pitch + x0]);
    const float d = static_cast<float>(p[y1 * pitch + x1]);
    return bilerp(a, b, c, d, tx, ty);
}

__device__ void sampleUv(const uint8_t* uv, int pitch, int width, int height, float x, float y, float& u, float& v) {
    const int cw = max(1, width / 2);
    const int ch = max(1, height / 2);
    float cx = x * 0.5f;
    float cy = y * 0.5f;
    cx = fminf(fmaxf(cx, 0.0f), static_cast<float>(cw - 1));
    cy = fminf(fmaxf(cy, 0.0f), static_cast<float>(ch - 1));
    const int x0 = static_cast<int>(floorf(cx));
    const int y0 = static_cast<int>(floorf(cy));
    const int x1 = min(x0 + 1, cw - 1);
    const int y1 = min(y0 + 1, ch - 1);
    const float tx = cx - static_cast<float>(x0);
    const float ty = cy - static_cast<float>(y0);

    const float u00 = static_cast<float>(uv[y0 * pitch + x0 * 2 + 0]);
    const float u10 = static_cast<float>(uv[y0 * pitch + x1 * 2 + 0]);
    const float u01 = static_cast<float>(uv[y1 * pitch + x0 * 2 + 0]);
    const float u11 = static_cast<float>(uv[y1 * pitch + x1 * 2 + 0]);
    const float v00 = static_cast<float>(uv[y0 * pitch + x0 * 2 + 1]);
    const float v10 = static_cast<float>(uv[y0 * pitch + x1 * 2 + 1]);
    const float v01 = static_cast<float>(uv[y1 * pitch + x0 * 2 + 1]);
    const float v11 = static_cast<float>(uv[y1 * pitch + x1 * 2 + 1]);

    u = bilerp(u00, u10, u01, u11, tx, ty);
    v = bilerp(v00, v10, v01, v11, tx, ty);
}

__device__ void yuvToRgb(float y8, float u8, float v8, float& r, float& g, float& b) {
    const float y = (y8 - 16.0f) / 219.0f;
    const float u = (u8 - 128.0f) / 224.0f;
    const float v = (v8 - 128.0f) / 224.0f;
    r = clamp01(y + 1.5748f * v);
    g = clamp01(y - 0.187324f * u - 0.468124f * v);
    b = clamp01(y + 1.8556f * u);
}

__device__ void rgbToYuv(float r, float g, float b, float& y8, float& u8, float& v8) {
    r = clamp01(r);
    g = clamp01(g);
    b = clamp01(b);
    const float y = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    const float u = (b - y) / 1.8556f;
    const float v = (r - y) / 1.5748f;
    y8 = 16.0f + 219.0f * y;
    u8 = 128.0f + 224.0f * u;
    v8 = 128.0f + 224.0f * v;
}

__device__ bool mapPackedAlpha(int x, int y, int width, int height, float packScale, int& ax, int& ay) {
    int alphaW = static_cast<int>(floorf(static_cast<float>(width) * packScale + 0.5f));
    int alphaH = static_cast<int>(floorf(static_cast<float>(height) * packScale + 0.5f));
    alphaW = max(4, alphaW & ~3);
    alphaH = max(2, alphaH & ~1);

    const int halfW = alphaW / 2;
    const int halfH = alphaH / 2;
    const int quarterW = alphaW / 4;
    const int right2X = alphaW - quarterW;
    const int xCenter = width / 2 - halfW / 2;
    const int yBottom = height - halfH;
    const int xRight = width - quarterW;

    if (x >= xCenter && x < xCenter + halfW && y >= yBottom && y < yBottom + halfH) {
        ax = x - xCenter; ay = y - yBottom; return true;
    }
    if (x >= xCenter && x < xCenter + halfW && y >= 0 && y < halfH) {
        ax = x - xCenter; ay = halfH + y; return true;
    }
    if (x >= xRight && x < width && y >= yBottom && y < yBottom + halfH) {
        ax = halfW + (x - xRight); ay = y - yBottom; return true;
    }
    if (x >= 0 && x < quarterW && y >= yBottom && y < yBottom + halfH) {
        ax = right2X + x; ay = y - yBottom; return true;
    }
    if (x >= xRight && x < width && y >= 0 && y < halfH) {
        ax = halfW + (x - xRight); ay = halfH + y; return true;
    }
    if (x >= 0 && x < quarterW && y >= 0 && y < halfH) {
        ax = right2X + x; ay = halfH + y; return true;
    }
    return false;
}

__device__ float sampleAlphaPlane(const half* pha, int width, int height, float x, float y) {
    x = fminf(fmaxf(x, 0.0f), static_cast<float>(width - 1));
    y = fminf(fmaxf(y, 0.0f), static_cast<float>(height - 1));
    const int x0 = static_cast<int>(floorf(x));
    const int y0 = static_cast<int>(floorf(y));
    const int x1 = min(x0 + 1, width - 1);
    const int y1 = min(y0 + 1, height - 1);
    const float tx = x - static_cast<float>(x0);
    const float ty = y - static_cast<float>(y0);
    const float a = __half2float(pha[y0 * width + x0]);
    const float b = __half2float(pha[y0 * width + x1]);
    const float c = __half2float(pha[y1 * width + x0]);
    const float d = __half2float(pha[y1 * width + x1]);
    return clamp01(bilerp(a, b, c, d, tx, ty));
}

__device__ float sampleAlphaSmall(const half* pha, const float2* projectionMap,
                                  int width, int height, float packScale,
                                  int ax, int ay) {
    int alphaW = static_cast<int>(floorf(static_cast<float>(width) * packScale + 0.5f));
    int alphaH = static_cast<int>(floorf(static_cast<float>(height) * packScale + 0.5f));
    alphaW = max(4, alphaW & ~3);
    alphaH = max(2, alphaH & ~1);

    // Downscale the RVM alpha into the packed corner blocks while preserving
    // the original source projection.
    const int sourceX = min(width - 1, max(0, (ax * width) / max(1, alphaW)));
    const int sourceY = min(height - 1, max(0, (ay * height) / max(1, alphaH)));
    const float2 src = projectionMap[sourceY * width + sourceX];
    if (src.x < 0.0f || src.y < 0.0f) return 0.0f;
    return sampleAlphaPlane(pha, width, height, src.x, src.y);
}

__device__ void desiredRgb(int x, int y, const R800ZZGpuFrame& src, const half* pha,
                           const float2* projectionMap, float packScale,
                           float& r, float& g, float& b) {
    int ax = 0, ay = 0;
    if (mapPackedAlpha(x, y, src.width, src.height, packScale, ax, ay)) {
        const float a = sampleAlphaSmall(pha, projectionMap, src.width, src.height,
                                         packScale, ax, ay);
        r = a; g = 0.0f; b = 0.0f;
        return;
    }

    const float2 mapped = projectionMap[y * src.width + x];
    if (mapped.x < 0.0f || mapped.y < 0.0f) {
        r = g = b = 0.0f;
        return;
    }
    const float sx = mapped.x;
    const float sy = mapped.y;

    const float yy = samplePlane(src.y, src.pitchY, src.width, src.height, sx, sy);
    float uu = 128.0f, vv = 128.0f;
    sampleUv(src.uv, src.pitchUV, src.width, src.height, sx, sy, uu, vv);
    yuvToRgb(yy, uu, vv, r, g, b);
}

__global__ void nv12ToHalfRgbKernel(R800ZZGpuFrame src, half* dst) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= src.width || y >= src.height) return;

    const float yy = static_cast<float>(src.y[y * src.pitchY + x]);
    const int uvx = (x / 2) * 2;
    const int uvy = y / 2;
    const float uu = static_cast<float>(src.uv[uvy * src.pitchUV + uvx]);
    const float vv = static_cast<float>(src.uv[uvy * src.pitchUV + uvx + 1]);
    float r, g, b;
    yuvToRgb(yy, uu, vv, r, g, b);
    const size_t plane = static_cast<size_t>(src.width) * static_cast<size_t>(src.height);
    const size_t i = static_cast<size_t>(y) * static_cast<size_t>(src.width) + static_cast<size_t>(x);
    dst[i] = __float2half(r);
    dst[plane + i] = __float2half(g);
    dst[plane * 2 + i] = __float2half(b);
}

__global__ void projectionMapKernel(float2* map, int width, int height) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) return;
    // Preserve the source projection exactly. The server does not interpret
    // mono/SBS/VR180/VR360 layout; that is a receiver-side concern.
    map[y * width + x] = make_float2(static_cast<float>(x), static_cast<float>(y));
}

__global__ void packKernel(R800ZZGpuFrame src, const half* pha, const float2* projectionMap,
                           R800ZZGpuFrameMutable dst, float packScale) {
    const int bx = blockIdx.x * blockDim.x + threadIdx.x;
    const int by = blockIdx.y * blockDim.y + threadIdx.y;
    const int x0 = bx * 2;
    const int y0 = by * 2;
    if (x0 >= dst.width || y0 >= dst.height) return;

    float sumU = 0.0f, sumV = 0.0f;
    int count = 0;
    for (int dy = 0; dy < 2; ++dy) {
        for (int dx = 0; dx < 2; ++dx) {
            const int x = x0 + dx;
            const int y = y0 + dy;
            if (x >= dst.width || y >= dst.height) continue;
            float r, g, b;
            desiredRgb(x, y, src, pha, projectionMap, packScale, r, g, b);
            float yy, uu, vv;
            rgbToYuv(r, g, b, yy, uu, vv);
            dst.y[y * dst.pitchY + x] = static_cast<uint8_t>(fminf(235.0f, fmaxf(16.0f, yy)) + 0.5f);
            sumU += uu;
            sumV += vv;
            ++count;
        }
    }
    if (count > 0) {
        const int uvx = bx * 2;
        const int uvy = by;
        dst.uv[uvy * dst.pitchUV + uvx + 0] = static_cast<uint8_t>(fminf(240.0f, fmaxf(16.0f, sumU / count)) + 0.5f);
        dst.uv[uvy * dst.pitchUV + uvx + 1] = static_cast<uint8_t>(fminf(240.0f, fmaxf(16.0f, sumV / count)) + 0.5f);
    }
}

__global__ void chromaKeyKernel(R800ZZGpuFrame src, const half* pha,
                                R800ZZGpuFrameMutable dst) {
    const int bx = blockIdx.x * blockDim.x + threadIdx.x;
    const int by = blockIdx.y * blockDim.y + threadIdx.y;
    const int x0 = bx * 2;
    const int y0 = by * 2;
    if (x0 >= dst.width || y0 >= dst.height) return;

    float sumU = 0.0f;
    float sumV = 0.0f;
    int count = 0;
    for (int dy = 0; dy < 2; ++dy) {
        for (int dx = 0; dx < 2; ++dx) {
            const int x = x0 + dx;
            const int y = y0 + dy;
            if (x >= dst.width || y >= dst.height) continue;

            const float yy = static_cast<float>(src.y[y * src.pitchY + x]);
            const int uvx = (x / 2) * 2;
            const int uvy = y / 2;
            const float uu = static_cast<float>(src.uv[uvy * src.pitchUV + uvx]);
            const float vv = static_cast<float>(src.uv[uvy * src.pitchUV + uvx + 1]);
            float sourceR = 0.0f;
            float sourceG = 0.0f;
            float sourceB = 0.0f;
            yuvToRgb(yy, uu, vv, sourceR, sourceG, sourceB);

            const float alpha = clamp01(__half2float(
                pha[static_cast<size_t>(y) * static_cast<size_t>(src.width) +
                    static_cast<size_t>(x)]));
            const float r = sourceR * alpha;
            const float g = sourceG * alpha + (1.0f - alpha);
            const float b = sourceB * alpha;

            float outY = 0.0f;
            float outU = 128.0f;
            float outV = 128.0f;
            rgbToYuv(r, g, b, outY, outU, outV);
            dst.y[y * dst.pitchY + x] = static_cast<uint8_t>(
                fminf(235.0f, fmaxf(16.0f, outY)) + 0.5f);
            sumU += outU;
            sumV += outV;
            ++count;
        }
    }

    if (count > 0) {
        const int uvx = bx * 2;
        const int uvy = by;
        dst.uv[uvy * dst.pitchUV + uvx] = static_cast<uint8_t>(
            fminf(240.0f, fmaxf(16.0f, sumU / count)) + 0.5f);
        dst.uv[uvy * dst.pitchUV + uvx + 1] = static_cast<uint8_t>(
            fminf(240.0f, fmaxf(16.0f, sumV / count)) + 0.5f);
    }
}

__global__ void nv12ToBgraPreviewKernel(R800ZZGpuFrame src, uint8_t* dst, int dstWidth, int dstHeight) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= dstWidth || y >= dstHeight) return;

    const int sx = min(src.width - 1, (x * src.width) / max(1, dstWidth));
    const int sy = min(src.height - 1, (y * src.height) / max(1, dstHeight));
    const float yy = static_cast<float>(src.y[sy * src.pitchY + sx]);
    const int uvx = (sx / 2) * 2;
    const int uvy = sy / 2;
    const float uu = static_cast<float>(src.uv[uvy * src.pitchUV + uvx]);
    const float vv = static_cast<float>(src.uv[uvy * src.pitchUV + uvx + 1]);

    float r, g, b;
    yuvToRgb(yy, uu, vv, r, g, b);
    const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(dstWidth) + static_cast<size_t>(x)) * 4u;
    dst[i + 0] = static_cast<uint8_t>(b * 255.0f + 0.5f);
    dst[i + 1] = static_cast<uint8_t>(g * 255.0f + 0.5f);
    dst[i + 2] = static_cast<uint8_t>(r * 255.0f + 0.5f);
    dst[i + 3] = 255;
}

__global__ void prepareYuva420pKernel(R800ZZGpuFrame src, const half* pha,
                                      uint8_t* dstU, int pitchU,
                                      uint8_t* dstV, int pitchV,
                                      uint8_t* dstAlpha, int pitchAlpha) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= src.width || y >= src.height) return;

    const float alpha = clamp01(__half2float(
        pha[static_cast<size_t>(y) * static_cast<size_t>(src.width) +
            static_cast<size_t>(x)]));
    dstAlpha[y * pitchAlpha + x] =
        static_cast<uint8_t>(alpha * 255.0f + 0.5f);

    if ((x & 1) == 0 && (y & 1) == 0) {
        const int cx = x / 2;
        const int cy = y / 2;
        const int uvx = cx * 2;
        dstU[cy * pitchU + cx] = src.uv[cy * src.pitchUV + uvx];
        dstV[cy * pitchV + cx] = src.uv[cy * src.pitchUV + uvx + 1];
    }
}

} // namespace

cudaError_t r800zzNv12ToRgbHalfNchw(const R800ZZGpuFrame& src, void* dstHalf, cudaStream_t stream) {
    dim3 block(16, 16);
    dim3 grid((src.width + block.x - 1) / block.x, (src.height + block.y - 1) / block.y);
    nv12ToHalfRgbKernel<<<grid, block, 0, stream>>>(src, static_cast<half*>(dstHalf));
    return cudaGetLastError();
}

cudaError_t r800zzBuildProjectionMap(void* mapFloat2, int width, int height,
                                      cudaStream_t stream) {
    dim3 block(16, 16);
    dim3 grid((width + block.x - 1) / block.x, (height + block.y - 1) / block.y);
    projectionMapKernel<<<grid, block, 0, stream>>>(static_cast<float2*>(mapFloat2), width, height);
    return cudaGetLastError();
}

cudaError_t r800zzPackAlphaNv12(const R800ZZGpuFrame& src, const void* alphaHalf,
                                 const void* projectionMapFloat2,
                                 const R800ZZGpuFrameMutable& dst,
                                 float packScale, cudaStream_t stream) {
    dim3 block(16, 16);
    const int chromaW = (dst.width + 1) / 2;
    const int chromaH = (dst.height + 1) / 2;
    dim3 grid((chromaW + block.x - 1) / block.x, (chromaH + block.y - 1) / block.y);
    packKernel<<<grid, block, 0, stream>>>(src, static_cast<const half*>(alphaHalf),
                                           static_cast<const float2*>(projectionMapFloat2),
                                           dst, packScale);
    return cudaGetLastError();
}

cudaError_t r800zzCompositeChromaKeyNv12(
    const R800ZZGpuFrame& src,
    const void* alphaHalf,
    const R800ZZGpuFrameMutable& dst,
    cudaStream_t stream) {
    dim3 block(16, 16);
    const int chromaW = (dst.width + 1) / 2;
    const int chromaH = (dst.height + 1) / 2;
    dim3 grid((chromaW + block.x - 1) / block.x,
              (chromaH + block.y - 1) / block.y);
    chromaKeyKernel<<<grid, block, 0, stream>>>(
        src, static_cast<const half*>(alphaHalf), dst);
    return cudaGetLastError();
}

cudaError_t r800zzPrepareYuva420p(const R800ZZGpuFrame& src,
                                  const void* alphaHalf,
                                  uint8_t* dstU, int pitchU,
                                  uint8_t* dstV, int pitchV,
                                  uint8_t* dstAlpha, int pitchAlpha,
                                  cudaStream_t stream) {
    dim3 block(16, 16);
    dim3 grid((src.width + block.x - 1) / block.x,
              (src.height + block.y - 1) / block.y);
    prepareYuva420pKernel<<<grid, block, 0, stream>>>(
        src, static_cast<const half*>(alphaHalf),
        dstU, pitchU, dstV, pitchV, dstAlpha, pitchAlpha);
    return cudaGetLastError();
}


cudaError_t r800zzNv12ToBgraPreview(const R800ZZGpuFrame& src, uint8_t* dstBgra,
                                      int dstWidth, int dstHeight, cudaStream_t stream) {
    dim3 block(16, 16);
    dim3 grid((dstWidth + block.x - 1) / block.x,
              (dstHeight + block.y - 1) / block.y);
    nv12ToBgraPreviewKernel<<<grid, block, 0, stream>>>(src, dstBgra, dstWidth, dstHeight);
    return cudaGetLastError();
}
