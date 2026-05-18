#include "lofi_storage.hpp"

#include "lofi_core.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <cstdio>
#include <string>
#include <sys/stat.h>

namespace lofi {
namespace {

constexpr const char *kPathIndexMagic = "LOFI_INDEX_V1";

bool is_directory(const std::string &path)
{
    struct stat info = {};
    if (stat(path.c_str(), &info) != 0) {
        return false;
    }
    return S_ISDIR(info.st_mode);
}

bool should_skip_dir(const std::string &name)
{
    return name.empty() || name == "." || name == ".." || name == ".lofi" || name == "LOFI" || name == "Playlists";
}

void scan_recursive(const std::string &dir, size_t depth, size_t max_depth, StorageScanResult &result)
{
    if (depth > max_depth) {
        result.warnings.push_back("Skip deep directory: " + dir);
        return;
    }

    DIR *handle = opendir(dir.c_str());
    if (!handle) {
        result.warnings.push_back("Cannot open " + dir + ": " + std::strerror(errno));
        return;
    }

    while (dirent *entry = readdir(handle)) {
        const std::string name = entry->d_name;
        if (should_skip_dir(name)) {
            continue;
        }

        const std::string path = dir + "/" + name;
        if (is_directory(path)) {
            scan_recursive(path, depth + 1, max_depth, result);
        } else if (is_audio_path(path)) {
            result.paths.push_back(path);
        }
    }

    closedir(handle);
}

} // namespace

StorageScanResult collect_audio_paths(const std::string &music_root, size_t max_depth)
{
    StorageScanResult result;
    if (!is_directory(music_root)) {
        result.warnings.push_back("Music root not found: " + music_root);
        return result;
    }

    scan_recursive(music_root, 0, max_depth, result);
    std::sort(result.paths.begin(), result.paths.end());
    return result;
}

std::string serialize_path_index(const std::vector<std::string> &paths)
{
    std::string text;
    text += kPathIndexMagic;
    text += "\n";
    for (const std::string &path : paths) {
        if (!path.empty() && path.find('\n') == std::string::npos && is_audio_path(path)) {
            text += path;
            text += "\n";
        }
    }
    return text;
}

bool parse_path_index(const std::string &text, std::vector<std::string> &paths)
{
    paths.clear();
    size_t begin = 0;
    size_t end = text.find('\n');
    if (end == std::string::npos) {
        return false;
    }
    if (text.substr(begin, end - begin) != kPathIndexMagic) {
        return false;
    }
    begin = end + 1;
    while (begin < text.size()) {
        end = text.find('\n', begin);
        if (end == std::string::npos) {
            end = text.size();
        }
        std::string path = text.substr(begin, end - begin);
        if (!path.empty()) {
            if (path.find('\r') != std::string::npos || !is_audio_path(path)) {
                paths.clear();
                return false;
            }
            paths.push_back(path);
        }
        begin = end + 1;
    }
    std::sort(paths.begin(), paths.end());
    return !paths.empty();
}

bool read_text_file(const std::string &path, std::string &out)
{
    FILE *file = std::fopen(path.c_str(), "rb");
    if (!file) {
        return false;
    }
    out.clear();
    char buffer[256];
    while (true) {
        const size_t got = std::fread(buffer, 1, sizeof(buffer), file);
        if (got > 0) {
            out.append(buffer, got);
        }
        if (got < sizeof(buffer)) {
            break;
        }
    }
    const bool ok = std::ferror(file) == 0;
    std::fclose(file);
    return ok;
}

bool write_text_file(const std::string &path, const std::string &text)
{
    FILE *file = std::fopen(path.c_str(), "wb");
    if (!file) {
        return false;
    }
    const size_t written = std::fwrite(text.data(), 1, text.size(), file);
    const bool ok = written == text.size() && std::fclose(file) == 0;
    return ok;
}

bool ensure_directory(const std::string &path)
{
    if (is_directory(path)) {
        return true;
    }
    return mkdir(path.c_str(), 0775) == 0 || errno == EEXIST;
}

} // namespace lofi
