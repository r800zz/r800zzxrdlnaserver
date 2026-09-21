#include "rvm_ort.h"

#include <array>
#include <cstring>
#include <sstream>
#include <unordered_map>
#include <vector>

#include <onnxruntime_cxx_api.h>

namespace {

std::string ortError(const Ort::Exception& e) {
    std::ostringstream oss;
    oss << "ONNX Runtime error: " << e.what()
        << " code=" << static_cast<int>(e.GetOrtErrorCode());
    return oss.str();
}

Ort::Value makeCudaTensor(const Ort::MemoryInfo& mem, void* ptr, size_t bytes,
                          const std::vector<int64_t>& shape,
                          ONNXTensorElementDataType type) {
    return Ort::Value::CreateTensor(mem, ptr, bytes, shape.data(), shape.size(), type);
}

Ort::Value makeCpuTensor(const Ort::MemoryInfo& mem, float* ptr, size_t count,
                         const std::vector<int64_t>& shape) {
    return Ort::Value::CreateTensor<float>(
        mem, ptr, count, shape.data(), shape.size());
}

bool hasOfficialRvmInputs(Ort::Session& session, std::string& error) {
    bool hasDownsampleInput = false;
    Ort::AllocatorWithDefaultOptions allocator;
    const size_t inputCount = session.GetInputCount();
    for (size_t i = 0; i < inputCount; ++i) {
        auto name = session.GetInputNameAllocated(i, allocator);
        if (name && std::strcmp(name.get(), "downsample_ratio") == 0) {
            hasDownsampleInput = true;
            break;
        }
    }
    if (!hasDownsampleInput) {
        error = "RVM model mismatch: official ONNX with downsample_ratio input is required";
    }
    return hasDownsampleInput;
}

} // namespace

struct RvmOrtGpu::Impl {
    Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "r800zz-rvm"};
    Ort::SessionOptions options;
    std::unique_ptr<Ort::Session> session;
    Ort::MemoryInfo cudaMem{Ort::MemoryInfo("Cuda", OrtAllocatorType::OrtDeviceAllocator, 0, OrtMemTypeDefault)};

    int width = 0;
    int height = 0;
    int deviceId = 0;
    float downsample = 0.25f;

    void* pha = nullptr;
    size_t phaBytes = 0;

    void* downsampleDevice = nullptr;
    std::vector<Ort::Value> rec;
    std::array<void*, 4> initialRecPtrs{};

    ~Impl() {
        rec.clear();
        for (void* p : initialRecPtrs) {
            if (p) cudaFree(p);
        }
        if (downsampleDevice) cudaFree(downsampleDevice);
        if (pha) cudaFree(pha);
    }
};

RvmOrtGpu::RvmOrtGpu() : impl_(std::make_unique<Impl>()) {}
RvmOrtGpu::~RvmOrtGpu() = default;

bool RvmOrtGpu::initialize(const std::wstring& modelPath, int width, int height,
                           float downsampleRatio, int deviceId, cudaStream_t stream,
                           std::string& error) {
    try {
        impl_->width = width;
        impl_->height = height;
        impl_->downsample = downsampleRatio;
        impl_->deviceId = deviceId;
        impl_->cudaMem = Ort::MemoryInfo("Cuda", OrtAllocatorType::OrtDeviceAllocator,
                                         deviceId, OrtMemTypeDefault);

        // Use ONNX Runtime CUDA EP directly. The official RVM ONNX model is
        // tested with ORT CUDA, and I/O binding keeps src, alpha and recurrent
        // states on the GPU instead of copying recurrent tensors via the CPU.
        Ort::CUDAProviderOptions cudaOptions;
        cudaOptions.Update({
            {"device_id", std::to_string(deviceId)},
            {"do_copy_in_default_stream", "1"},
            {"cudnn_conv_algo_search", "HEURISTIC"},
            {"cudnn_conv_use_max_workspace", "1"},
            {"use_tf32", "1"}
        });
        if (stream != nullptr) {
            cudaOptions.UpdateWithValue("user_compute_stream", stream);
        }
        impl_->options.AppendExecutionProvider_CUDA_V2(*cudaOptions);
        impl_->options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        impl_->options.DisableMemPattern();
        impl_->options.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);

        impl_->session = std::make_unique<Ort::Session>(impl_->env, modelPath.c_str(), impl_->options);

        // Reject an old TensorRT-patched model immediately. The official RVM
        // ONNX must expose downsample_ratio as a runtime FP32 input.
        if (!hasOfficialRvmInputs(*impl_->session, error)) return false;

        const size_t pixels = static_cast<size_t>(width) * static_cast<size_t>(height);
        impl_->phaBytes = pixels * sizeof(uint16_t);
        if (cudaMalloc(&impl_->pha, impl_->phaBytes) != cudaSuccess) {
            error = "cudaMalloc failed for RVM alpha output buffer";
            return false;
        }

        if (cudaMalloc(&impl_->downsampleDevice, sizeof(float)) != cudaSuccess) {
            error = "cudaMalloc failed for RVM downsample_ratio";
            return false;
        }
        const cudaError_t copyRatio = cudaMemcpyAsync(
            impl_->downsampleDevice, &impl_->downsample, sizeof(float),
            cudaMemcpyHostToDevice, stream);
        if (copyRatio != cudaSuccess) {
            error = std::string("cudaMemcpyAsync downsample_ratio failed: ") + cudaGetErrorString(copyRatio);
            return false;
        }

        impl_->rec.clear();
        impl_->rec.reserve(4);
        const std::vector<int64_t> initShape{1, 1, 1, 1};
        for (int i = 0; i < 4; ++i) {
            void* p = nullptr;
            if (cudaMalloc(&p, sizeof(uint16_t)) != cudaSuccess) {
                error = "cudaMalloc failed for RVM recurrent state";
                return false;
            }
            impl_->initialRecPtrs[static_cast<size_t>(i)] = p;
            const cudaError_t zero = cudaMemsetAsync(p, 0, sizeof(uint16_t), stream);
            if (zero != cudaSuccess) {
                error = std::string("cudaMemsetAsync recurrent state failed: ") + cudaGetErrorString(zero);
                return false;
            }
            impl_->rec.emplace_back(makeCudaTensor(
                impl_->cudaMem, p, sizeof(uint16_t), initShape,
                ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16));
        }

        const cudaError_t initSync = cudaStreamSynchronize(stream);
        if (initSync != cudaSuccess) {
            error = std::string("RVM CUDA initialization sync failed: ") + cudaGetErrorString(initSync);
            return false;
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

struct RvmOrtCpu::Impl {
    Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "r800zz-rvm-cpu"};
    Ort::SessionOptions options;
    std::unique_ptr<Ort::Session> session;
    Ort::MemoryInfo cpuMem{
        Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault)};

    int width = 0;
    int height = 0;
    float downsample = 0.25f;
    std::vector<float> alpha;
    std::array<float, 4> initialRec{};
    std::vector<Ort::Value> rec;
};

RvmOrtCpu::RvmOrtCpu() : impl_(std::make_unique<Impl>()) {}
RvmOrtCpu::~RvmOrtCpu() = default;

bool RvmOrtCpu::initialize(const std::wstring& modelPath, int width, int height,
                           float downsampleRatio, std::string& error) {
    try {
        impl_->width = width;
        impl_->height = height;
        impl_->downsample = downsampleRatio;
        impl_->options.SetGraphOptimizationLevel(
            GraphOptimizationLevel::ORT_ENABLE_ALL);
        impl_->options.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
        impl_->session = std::make_unique<Ort::Session>(
            impl_->env, modelPath.c_str(), impl_->options);
        if (!hasOfficialRvmInputs(*impl_->session, error)) return false;

        const size_t pixels = static_cast<size_t>(width) *
                              static_cast<size_t>(height);
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

bool RvmOrtCpu::resetState(std::string& error) {
    try {
        impl_->rec.clear();
        impl_->rec.reserve(4);
        impl_->initialRec.fill(0.0f);
        const std::vector<int64_t> shape{1, 1, 1, 1};
        for (size_t i = 0; i < impl_->initialRec.size(); ++i) {
            impl_->rec.emplace_back(makeCpuTensor(
                impl_->cpuMem, &impl_->initialRec[i], 1, shape));
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

bool RvmOrtCpu::run(const float* srcFp32Nchw, RvmCpuOutput& output,
                    std::string& error) {
    try {
        const size_t pixels = static_cast<size_t>(impl_->width) *
                              static_cast<size_t>(impl_->height);
        const std::vector<int64_t> srcShape{
            1, 3, impl_->height, impl_->width};
        auto src = makeCpuTensor(
            impl_->cpuMem, const_cast<float*>(srcFp32Nchw), pixels * 3,
            srcShape);
        const std::vector<int64_t> alphaShape{
            1, 1, impl_->height, impl_->width};
        auto alpha = makeCpuTensor(
            impl_->cpuMem, impl_->alpha.data(), pixels, alphaShape);
        const std::vector<int64_t> ratioShape{1};
        auto ratio = makeCpuTensor(
            impl_->cpuMem, &impl_->downsample, 1, ratioShape);

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
            error = "RVM CPU returned unexpected output count: " +
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

bool RvmOrtGpu::resetState(cudaStream_t stream, std::string& error) {
    try {
        impl_->rec.clear();
        const std::vector<int64_t> initShape{1, 1, 1, 1};
        for (size_t i = 0; i < impl_->initialRecPtrs.size(); ++i) {
            void* p = impl_->initialRecPtrs[i];
            if (!p) {
                error = "RVM recurrent state buffer is not initialized";
                return false;
            }
            const cudaError_t zero = cudaMemsetAsync(p, 0, sizeof(uint16_t), stream);
            if (zero != cudaSuccess) {
                error = std::string("cudaMemsetAsync recurrent state failed: ") +
                        cudaGetErrorString(zero);
                return false;
            }
            impl_->rec.emplace_back(makeCudaTensor(
                impl_->cudaMem, p, sizeof(uint16_t), initShape,
                ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16));
        }
        const cudaError_t sync = cudaStreamSynchronize(stream);
        if (sync != cudaSuccess) {
            error = std::string("cudaStreamSynchronize recurrent reset failed: ") +
                    cudaGetErrorString(sync);
            return false;
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

bool RvmOrtGpu::run(const void* srcFp16Nchw, cudaStream_t,
                    RvmGpuOutput& output, std::string& error) {
    try {
        const size_t pixels = static_cast<size_t>(impl_->width) * static_cast<size_t>(impl_->height);
        const size_t srcBytes = pixels * 3 * sizeof(uint16_t);
        const std::vector<int64_t> srcShape{1, 3, impl_->height, impl_->width};
        auto src = makeCudaTensor(impl_->cudaMem, const_cast<void*>(srcFp16Nchw), srcBytes, srcShape,
                                  ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16);

        const std::vector<int64_t> phaShape{1, 1, impl_->height, impl_->width};
        auto pha = makeCudaTensor(impl_->cudaMem, impl_->pha, impl_->phaBytes, phaShape,
                                  ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16);

        const std::vector<int64_t> ratioShape{1};
        auto ratio = makeCudaTensor(impl_->cudaMem, impl_->downsampleDevice, sizeof(float), ratioShape,
                                    ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT);

        Ort::IoBinding binding(*impl_->session);
        binding.BindInput("src", src);
        binding.BindInput("r1i", impl_->rec[0]);
        binding.BindInput("r2i", impl_->rec[1]);
        binding.BindInput("r3i", impl_->rec[2]);
        binding.BindInput("r4i", impl_->rec[3]);
        binding.BindInput("downsample_ratio", ratio);

        // Only alpha and recurrent states are needed. Foreground RGB is not
        // requested, avoiding a full-resolution output allocation/copy.
        binding.BindOutput("pha", pha);
        binding.BindOutput("r1o", impl_->cudaMem);
        binding.BindOutput("r2o", impl_->cudaMem);
        binding.BindOutput("r3o", impl_->cudaMem);
        binding.BindOutput("r4o", impl_->cudaMem);

        Ort::RunOptions runOptions;
        impl_->session->Run(runOptions, binding);
        binding.SynchronizeOutputs();

        auto outputs = binding.GetOutputValues();
        if (outputs.size() != 5) {
            error = "RVM returned unexpected output count: " + std::to_string(outputs.size());
            return false;
        }

        std::vector<Ort::Value> nextRec;
        nextRec.reserve(4);
        for (size_t i = 1; i < 5; ++i) {
            nextRec.emplace_back(std::move(outputs[i]));
        }
        impl_->rec = std::move(nextRec);
        output.alphaFp16 = impl_->pha;
        return true;
    } catch (const Ort::Exception& e) {
        error = ortError(e);
        return false;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}
