#pragma once

#include <filesystem>
#include <string>

enum class CpuConversionMode {
    AlphaPacked,
    WebmVp9Alpha,
    ChromaKey,
};

struct CpuConversionOptions {
    std::filesystem::path input;
    std::filesystem::path output;
    std::filesystem::path model;
    CpuConversionMode mode{CpuConversionMode::AlphaPacked};
    int quality{20};
    float downsample{0.25f};
};

bool RunCpuConversion(const CpuConversionOptions& options, std::string& error);
