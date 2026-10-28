#pragma once
#include <cstdlib>
#include <filesystem>
namespace LocalCache {
inline std::filesystem::path PipelineRoot() {
    const char* path = std::getenv("KYTY_PIPELINE_CACHE_DIR");
    return path && *path ? std::filesystem::path(path) : std::filesystem::path("_PipelineCache");
}
}
