#pragma once

#include <cuda_runtime.h>
#include <memory>
#include <string>

struct RvmGpuOutput {
    const void* alphaFp16 = nullptr;
};

struct RvmCpuOutput {
    const float* alphaFp32 = nullptr;
};

class RvmOrtGpu {
public:
    RvmOrtGpu();
    ~RvmOrtGpu();

    RvmOrtGpu(const RvmOrtGpu&) = delete;
    RvmOrtGpu& operator=(const RvmOrtGpu&) = delete;

    bool initialize(const std::wstring& modelPath, int width, int height,
                    float downsampleRatio, int deviceId, cudaStream_t stream,
                    std::string& error);
    bool run(const void* srcFp16Nchw, cudaStream_t stream, RvmGpuOutput& output, std::string& error);
    bool resetState(cudaStream_t stream, std::string& error);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class RvmOrtCpu {
public:
    RvmOrtCpu();
    ~RvmOrtCpu();

    RvmOrtCpu(const RvmOrtCpu&) = delete;
    RvmOrtCpu& operator=(const RvmOrtCpu&) = delete;

    bool initialize(const std::wstring& modelPath, int width, int height,
                    float downsampleRatio, std::string& error);
    bool run(const float* srcFp32Nchw, RvmCpuOutput& output,
             std::string& error);
    bool resetState(std::string& error);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
