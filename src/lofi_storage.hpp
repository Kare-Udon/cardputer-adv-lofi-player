#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace lofi {

struct StorageScanResult {
    std::vector<std::string> paths;
    std::vector<std::string> warnings;
};

StorageScanResult collect_audio_paths(const std::string &music_root, size_t max_depth);
std::string serialize_path_index(const std::vector<std::string> &paths);
bool parse_path_index(const std::string &text, std::vector<std::string> &paths);
bool read_text_file(const std::string &path, std::string &out);
bool write_text_file(const std::string &path, const std::string &text);
bool ensure_directory(const std::string &path);

} // namespace lofi
