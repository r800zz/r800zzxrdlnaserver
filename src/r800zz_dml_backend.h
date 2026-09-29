#pragma once

#include <atomic>
#include <filesystem>
#include <string>

#include <windows.h>

int RunR800zzDmlBackend(int argc, char** argv);

struct R800zzDmlResidentSession;

R800zzDmlResidentSession* R800zzCreateDmlResidentSession(
    const std::filesystem::path& input,
    const std::filesystem::path& model,
    bool directml,
    int device,
    int qp,
    float downsample,
    std::string& error);

void R800zzDestroyDmlResidentSession(R800zzDmlResidentSession* session);

int R800zzRunDmlResidentStream(
    R800zzDmlResidentSession* session,
    const std::filesystem::path& input,
    int64_t start_ms,
    const std::string& output_mode,
    HANDLE output,
    std::atomic<bool>* cancel_flag,
    std::string& error);
