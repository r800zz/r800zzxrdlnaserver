#pragma once

#include <filesystem>
#include <string>

bool RunSimpleVideoPlayer(const std::filesystem::path& input,
                          int language,
                          std::string& error);
