#include "lofi_core.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iomanip>
#include <map>
#include <sstream>
#include <utility>

namespace lofi {
namespace {

std::string lower_copy(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string trim_copy(const std::string &value)
{
    size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin]))) {
        ++begin;
    }
    size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }
    return value.substr(begin, end - begin);
}

std::vector<std::string> split_path(const std::string &path)
{
    std::string normalized = path;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');

    std::vector<std::string> parts;
    size_t begin = 0;
    while (begin < normalized.size()) {
        while (begin < normalized.size() && normalized[begin] == '/') {
            ++begin;
        }
        size_t end = begin;
        while (end < normalized.size() && normalized[end] != '/') {
            ++end;
        }
        if (end > begin) {
            parts.push_back(normalized.substr(begin, end - begin));
        }
        begin = end;
    }
    return parts;
}

std::string extension_of(const std::string &path)
{
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= path.size()) {
        return "";
    }
    return lower_copy(path.substr(dot + 1));
}

std::string basename_without_ext(const std::string &path)
{
    const std::vector<std::string> parts = split_path(path);
    if (parts.empty()) {
        return path;
    }
    std::string name = parts.back();
    const size_t dot = name.find_last_of('.');
    if (dot != std::string::npos) {
        name = name.substr(0, dot);
    }
    return name;
}

std::vector<std::string> relative_music_parts(const std::string &path)
{
    std::vector<std::string> parts = split_path(path);
    for (size_t i = 0; i < parts.size(); ++i) {
        if (lower_copy(parts[i]) == "music") {
            return std::vector<std::string>(parts.begin() + static_cast<long>(i) + 1, parts.end());
        }
    }
    return parts;
}

int parse_track_number(const std::string &name)
{
    int value = 0;
    size_t i = 0;
    while (i < name.size() && std::isdigit(static_cast<unsigned char>(name[i]))) {
        value = value * 10 + (name[i] - '0');
        ++i;
    }
    if (i == 0 || value <= 0 || value > 999) {
        return 0;
    }
    if (i < name.size() && (name[i] == ' ' || name[i] == '-' || name[i] == '_' || name[i] == '.')) {
        return value;
    }
    return 0;
}

std::string strip_track_prefix(const std::string &name)
{
    size_t i = 0;
    while (i < name.size() && std::isdigit(static_cast<unsigned char>(name[i]))) {
        ++i;
    }
    if (i == 0) {
        return name;
    }
    while (i < name.size() && (name[i] == ' ' || name[i] == '-' || name[i] == '_' || name[i] == '.')) {
        ++i;
    }
    if (i >= name.size()) {
        return name;
    }
    return trim_copy(name.substr(i));
}

std::string strip_source_id_suffix(const std::string &name)
{
    size_t i = name.size();
    while (i > 0 && std::isdigit(static_cast<unsigned char>(name[i - 1]))) {
        --i;
    }
    const size_t digit_count = name.size() - i;
    if (digit_count >= 5 && i > 0 && name[i - 1] == '_') {
        return trim_copy(name.substr(0, i - 1));
    }
    return name;
}

std::string clean_inferred_title(const std::string &name)
{
    return strip_source_id_suffix(strip_track_prefix(name));
}

std::string make_id(const std::string &prefix, const std::string &a, const std::string &b = "")
{
    std::string raw = prefix + ":" + lower_copy(a) + ":" + lower_copy(b);
    for (char &c : raw) {
        if (c == ' ' || c == '/' || c == '\\') {
            c = '_';
        }
    }
    return raw;
}

int parse_int_or(const std::string &value, int fallback)
{
    char *end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (end == value.c_str()) {
        return fallback;
    }
    return static_cast<int>(parsed);
}

uint32_t parse_u32_or(const std::string &value, uint32_t fallback)
{
    char *end = nullptr;
    const unsigned long parsed = std::strtoul(value.c_str(), &end, 10);
    if (end == value.c_str()) {
        return fallback;
    }
    return static_cast<uint32_t>(parsed);
}

void sort_album_tracks(LibraryIndex &index)
{
    for (Album &album : index.albums) {
        std::sort(album.track_indices.begin(), album.track_indices.end(), [&](size_t lhs, size_t rhs) {
            const Track &a = index.tracks[lhs];
            const Track &b = index.tracks[rhs];
            if (a.track_no != b.track_no) {
                if (a.track_no == 0) {
                    return false;
                }
                if (b.track_no == 0) {
                    return true;
                }
                return a.track_no < b.track_no;
            }
            return lower_copy(a.title) < lower_copy(b.title);
        });
    }
}

uint32_t lcg_next(uint32_t &state)
{
    state = state * 1664525u + 1013904223u;
    return state;
}

void shuffle_indices(std::vector<size_t> &items, uint32_t seed)
{
    if (items.size() < 2) {
        return;
    }
    uint32_t state = seed == 0 ? 0xC0FFEEu : seed;
    for (size_t i = items.size() - 1; i > 0; --i) {
        const size_t j = lcg_next(state) % (i + 1);
        std::swap(items[i], items[j]);
    }
}

std::string folder_id_for_track(const std::string &path)
{
    std::vector<std::string> parts = relative_music_parts(path);
    if (!parts.empty()) {
        parts.pop_back();
    }
    if (parts.empty()) {
        return "/Music";
    }
    std::string id = "/Music";
    for (const std::string &part : parts) {
        id += "/";
        id += part;
    }
    return id;
}

std::string folder_label_from_id(const std::string &folder_id)
{
    const std::vector<std::string> parts = split_path(folder_id);
    if (parts.size() >= 2) {
        return parts.back();
    }
    return "Music";
}

void sort_folder_tracks(LibraryIndex &index)
{
    for (Folder &folder : index.folders) {
        std::sort(folder.track_indices.begin(), folder.track_indices.end(), [&](size_t lhs, size_t rhs) {
            return lower_copy(index.tracks[lhs].path) < lower_copy(index.tracks[rhs].path);
        });
    }
}

std::vector<size_t> recently_added_track_indices(const LibraryIndex &index)
{
    std::vector<size_t> tracks;
    for (size_t i = 0; i < index.tracks.size(); ++i) {
        tracks.push_back(i);
    }
    std::sort(tracks.begin(), tracks.end(), [&](size_t lhs, size_t rhs) {
        const Track &a = index.tracks[lhs];
        const Track &b = index.tracks[rhs];
        if (a.mtime != b.mtime) {
            return a.mtime > b.mtime;
        }
        return lower_copy(a.path) < lower_copy(b.path);
    });
    return tracks;
}

void build_virtual_playlists(LibraryIndex &index)
{
    index.playlists.clear();
    if (index.tracks.empty()) {
        return;
    }

    Playlist all;
    all.id = "virtual:all";
    all.title = "All Songs";
    for (size_t i = 0; i < index.tracks.size(); ++i) {
        all.track_indices.push_back(i);
    }
    index.playlists.push_back(all);

    Playlist recent;
    recent.id = "virtual:recent";
    recent.title = "Recently Added";
    recent.track_indices = recently_added_track_indices(index);
    index.playlists.push_back(recent);

    Playlist loose;
    loose.id = "virtual:loose";
    loose.title = "Loose Tracks";
    for (size_t i = 0; i < index.tracks.size(); ++i) {
        const Track &track = index.tracks[i];
        if (track.album == "Singles / Loose" || track.album == "Inbox") {
            loose.track_indices.push_back(i);
        }
    }
    if (!loose.track_indices.empty()) {
        index.playlists.push_back(loose);
    }

    Playlist compilations;
    compilations.id = "virtual:compilations";
    compilations.title = "Compilations";
    for (size_t i = 0; i < index.tracks.size(); ++i) {
        if (index.tracks[i].album_artist == "Various Artists") {
            compilations.track_indices.push_back(i);
        }
    }
    if (!compilations.track_indices.empty()) {
        index.playlists.push_back(compilations);
    }
}

size_t next_utf8_offset(const std::string &value, size_t offset)
{
    if (offset >= value.size()) {
        return offset;
    }
    const unsigned char c0 = static_cast<unsigned char>(value[offset]);
    if (c0 < 0x80) {
        return offset + 1;
    }
    if ((c0 & 0xE0) == 0xC0 && offset + 1 < value.size()) {
        const unsigned char c1 = static_cast<unsigned char>(value[offset + 1]);
        if ((c1 & 0xC0) == 0x80) {
            return offset + 2;
        }
    } else if ((c0 & 0xF0) == 0xE0 && offset + 2 < value.size()) {
        const unsigned char c1 = static_cast<unsigned char>(value[offset + 1]);
        const unsigned char c2 = static_cast<unsigned char>(value[offset + 2]);
        if ((c1 & 0xC0) == 0x80 && (c2 & 0xC0) == 0x80) {
            return offset + 3;
        }
    } else if ((c0 & 0xF8) == 0xF0 && offset + 3 < value.size()) {
        const unsigned char c1 = static_cast<unsigned char>(value[offset + 1]);
        const unsigned char c2 = static_cast<unsigned char>(value[offset + 2]);
        const unsigned char c3 = static_cast<unsigned char>(value[offset + 3]);
        if ((c1 & 0xC0) == 0x80 && (c2 & 0xC0) == 0x80 && (c3 & 0xC0) == 0x80) {
            return offset + 4;
        }
    }
    return offset + 1;
}

bool decode_utf8_codepoint(const std::string &value, size_t &offset, uint32_t &codepoint)
{
    if (offset >= value.size()) {
        return false;
    }
    const unsigned char c0 = static_cast<unsigned char>(value[offset]);
    if (c0 < 0x80) {
        codepoint = c0;
        ++offset;
        return true;
    }
    if ((c0 & 0xE0) == 0xC0 && offset + 1 < value.size()) {
        const unsigned char c1 = static_cast<unsigned char>(value[offset + 1]);
        if ((c1 & 0xC0) == 0x80) {
            codepoint = ((c0 & 0x1F) << 6) | (c1 & 0x3F);
            offset += 2;
            return true;
        }
    } else if ((c0 & 0xF0) == 0xE0 && offset + 2 < value.size()) {
        const unsigned char c1 = static_cast<unsigned char>(value[offset + 1]);
        const unsigned char c2 = static_cast<unsigned char>(value[offset + 2]);
        if ((c1 & 0xC0) == 0x80 && (c2 & 0xC0) == 0x80) {
            codepoint = ((c0 & 0x0F) << 12) | ((c1 & 0x3F) << 6) | (c2 & 0x3F);
            offset += 3;
            return true;
        }
    }
    codepoint = c0;
    ++offset;
    return true;
}

void append_utf8(std::string &out, uint32_t codepoint)
{
    if (codepoint < 0x80) {
        out.push_back(static_cast<char>(codepoint));
    } else if (codepoint < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else if (codepoint < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
}

bool compose_japanese_mark(uint32_t base, uint32_t mark, uint32_t &composed)
{
    if (mark == 0x3099) {
        switch (base) {
        case 0x3046: composed = 0x3094; return true;
        case 0x304B: case 0x304D: case 0x304F: case 0x3051: case 0x3053:
        case 0x3055: case 0x3057: case 0x3059: case 0x305B: case 0x305D:
        case 0x305F: case 0x3061: case 0x3064: case 0x3066: case 0x3068:
        case 0x306F: case 0x3072: case 0x3075: case 0x3078: case 0x307B:
        case 0x30AB: case 0x30AD: case 0x30AF: case 0x30B1: case 0x30B3:
        case 0x30B5: case 0x30B7: case 0x30B9: case 0x30BB: case 0x30BD:
        case 0x30BF: case 0x30C1: case 0x30C4: case 0x30C6: case 0x30C8:
        case 0x30CF: case 0x30D2: case 0x30D5: case 0x30D8: case 0x30DB:
            composed = base + 1;
            return true;
        case 0x30A6: composed = 0x30F4; return true;
        default: return false;
        }
    }
    if (mark == 0x309A) {
        switch (base) {
        case 0x306F: case 0x3072: case 0x3075: case 0x3078: case 0x307B:
        case 0x30CF: case 0x30D2: case 0x30D5: case 0x30D8: case 0x30DB:
            composed = base + 2;
            return true;
        default: return false;
        }
    }
    return false;
}

std::string normalize_japanese_voicing_marks(const std::string &value)
{
    std::string out;
    out.reserve(value.size());
    size_t offset = 0;
    while (offset < value.size()) {
        uint32_t base = 0;
        const size_t base_start = offset;
        if (!decode_utf8_codepoint(value, offset, base)) {
            break;
        }
        const size_t next_start = offset;
        uint32_t mark = 0;
        if (decode_utf8_codepoint(value, offset, mark) && (mark == 0x3099 || mark == 0x309A)) {
            uint32_t composed = 0;
            if (compose_japanese_mark(base, mark, composed)) {
                append_utf8(out, composed);
                continue;
            }
        }
        offset = next_start;
        out.append(value, base_start, next_start - base_start);
    }
    return out;
}

std::string first_n(const std::string &value, size_t max_chars)
{
    if (max_chars == 0 || value.empty()) {
        return "";
    }

    size_t offset = 0;
    size_t chars = 0;
    while (offset < value.size() && chars < max_chars) {
        offset = next_utf8_offset(value, offset);
        ++chars;
    }
    if (offset >= value.size()) {
        return value;
    }
    if (max_chars <= 1) {
        return value.substr(0, offset);
    }

    offset = 0;
    chars = 0;
    while (offset < value.size() && chars + 1 < max_chars) {
        offset = next_utf8_offset(value, offset);
        ++chars;
    }
    return value.substr(0, offset) + "~";
}

constexpr LofiPreset kLofiPresetChoices[] = {
    LofiPreset::Off,
    LofiPreset::WarmTape,
    LofiPreset::VinylCafe,
    LofiPreset::RainyWindow,
    LofiPreset::TinyRadio,
    LofiPreset::LateNight,
};

constexpr size_t kLofiPresetChoiceCount = sizeof(kLofiPresetChoices) / sizeof(kLofiPresetChoices[0]);
constexpr int kScreenOffChoices[] = {10, 15, 20, 30, 60, 120, 180, 300, 600, 0};
constexpr size_t kScreenOffChoiceCount = sizeof(kScreenOffChoices) / sizeof(kScreenOffChoices[0]);
constexpr size_t kLibraryRootVisibleRows = 5;
constexpr size_t kLibraryListVisibleRows = 4;
constexpr int kVolumeBoostThreshold = 80;

int normalize_user_volume_percent(int percent)
{
    return std::max(0, std::min(100, percent));
}

bool adjust_user_volume(PlaybackState &playback, UiState &ui, int delta)
{
    const int current = normalize_user_volume_percent(playback.volume);
    if (delta > 0 && current == kVolumeBoostThreshold && !ui.volume_boost_warning_armed) {
        ui.volume_boost_warning_armed = true;
        ui.toast = "Distortion risk. Press again";
        return false;
    }

    const int next = normalize_user_volume_percent(current + delta);
    playback.volume = next;
    if (next <= kVolumeBoostThreshold || delta < 0) {
        ui.volume_boost_warning_armed = false;
    }
    ui.toast = "Volume " + std::to_string(playback.volume);
    return next != current;
}

int normalize_brightness_percent(int percent)
{
    percent = std::max(10, std::min(100, percent));
    return ((percent + 5) / 10) * 10;
}

int step_brightness_percent(int percent, int delta)
{
    percent = normalize_brightness_percent(percent);
    if (delta < 0) {
        return std::max(10, percent - 10);
    }
    if (delta > 0) {
        return std::min(100, percent + 10);
    }
    return percent;
}

int nearest_screen_off_choice_index(int seconds)
{
    if (seconds <= 0) {
        return static_cast<int>(kScreenOffChoiceCount) - 1;
    }
    int best_index = 0;
    int best_delta = std::abs(seconds - kScreenOffChoices[0]);
    for (size_t i = 1; i < kScreenOffChoiceCount; ++i) {
        const int delta = std::abs(seconds - kScreenOffChoices[i]);
        if (delta < best_delta) {
            best_delta = delta;
            best_index = static_cast<int>(i);
        }
    }
    return best_index;
}

int normalize_screen_off_seconds(int seconds)
{
    return kScreenOffChoices[nearest_screen_off_choice_index(seconds)];
}

int step_screen_off_seconds(int seconds, int delta)
{
    int index = nearest_screen_off_choice_index(seconds);
    if (delta < 0) {
        index = index == 0 ? static_cast<int>(kScreenOffChoiceCount) - 1 : index - 1;
    } else if (delta > 0) {
        index = index + 1 >= static_cast<int>(kScreenOffChoiceCount) ? 0 : index + 1;
    }
    return kScreenOffChoices[index];
}

std::string format_screen_off_seconds(int seconds)
{
    seconds = normalize_screen_off_seconds(seconds);
    if (seconds <= 0) {
        return "Forever";
    }
    if (seconds < 60) {
        return std::to_string(seconds) + "s";
    }
    return std::to_string(seconds / 60) + "m";
}

void clamp_selection(UiState &ui, size_t count)
{
    if (count == 0) {
        ui.selected = 0;
        ui.scroll = 0;
        return;
    }
    if (ui.selected >= count) {
        ui.selected = count - 1;
    }
    if (ui.selected < ui.scroll) {
        ui.scroll = ui.selected;
    }
    if (ui.selected >= ui.scroll + 4) {
        ui.scroll = ui.selected - 3;
    }
}

void clamp_selection_window(UiState &ui, size_t count, size_t visible_rows)
{
    if (count == 0) {
        ui.selected = 0;
        ui.scroll = 0;
        return;
    }
    if (ui.selected >= count) {
        ui.selected = count - 1;
    }
    const size_t visible = std::max<size_t>(1, visible_rows);
    if (ui.selected < ui.scroll) {
        ui.scroll = ui.selected;
    }
    if (ui.selected >= ui.scroll + visible) {
        ui.scroll = ui.selected - visible + 1;
    }
    if (count <= visible) {
        ui.scroll = 0;
    } else {
        ui.scroll = std::min(ui.scroll, count - visible);
    }
}

size_t list_window_start(size_t scroll, size_t selected, size_t count, size_t visible_rows)
{
    if (count == 0) {
        return 0;
    }
    const size_t visible = std::max<size_t>(1, visible_rows);
    if (count <= visible) {
        return 0;
    }
    const size_t max_start = count - visible;
    const size_t safe_selected = std::min(selected, count - 1);
    size_t start = std::min(scroll, max_start);
    if (safe_selected < start) {
        start = safe_selected;
    } else if (safe_selected >= start + visible) {
        start = safe_selected - visible + 1;
    }
    return std::min(start, max_start);
}

void move_selection_wrapped(UiState &ui, size_t count, int delta, size_t visible_rows)
{
    if (count == 0) {
        ui.selected = 0;
        ui.scroll = 0;
        return;
    }
    const size_t current = std::min(ui.selected, count - 1);
    if (delta < 0) {
        ui.selected = current == 0 ? count - 1 : current - 1;
    } else if (delta > 0) {
        ui.selected = current + 1 >= count ? 0 : current + 1;
    }
    clamp_selection_window(ui, count, visible_rows);
}

bool has_track(const std::vector<size_t> &tracks, size_t track_index)
{
    return std::find(tracks.begin(), tracks.end(), track_index) != tracks.end();
}

void toggle_track(std::vector<size_t> &tracks, size_t track_index)
{
    const auto it = std::find(tracks.begin(), tracks.end(), track_index);
    if (it == tracks.end()) {
        tracks.push_back(track_index);
    } else {
        tracks.erase(it);
    }
}

std::string count3(size_t value)
{
    std::ostringstream out;
    out << std::setw(3) << std::setfill('0') << value;
    return out.str();
}

std::vector<size_t> artist_track_indices(const LibraryIndex &index, const Artist &artist)
{
    std::vector<size_t> tracks;
    for (size_t album_index : artist.album_indices) {
        if (album_index >= index.albums.size()) {
            continue;
        }
        const Album &album = index.albums[album_index];
        tracks.insert(tracks.end(), album.track_indices.begin(), album.track_indices.end());
    }
    return tracks;
}

Queue make_explicit_queue(std::vector<size_t> tracks, const std::string &source_id, bool shuffle, uint32_t seed)
{
    Queue queue;
    queue.source_type = "selection";
    queue.source_id = source_id;
    queue.track_indices = std::move(tracks);
    queue.current_index = 0;
    queue.shuffle = shuffle;
    queue.shuffle_seed = seed;
    if (queue.shuffle) {
        shuffle_indices(queue.track_indices, queue.shuffle_seed);
    }
    return queue;
}

UiNavEntry capture_nav_entry(const UiState &ui)
{
    UiNavEntry entry;
    entry.page = ui.page;
    entry.context_index = ui.context_index;
    entry.parent_index = ui.parent_index;
    entry.selected = ui.selected;
    entry.scroll = ui.scroll;
    return entry;
}

void restore_nav_entry(UiState &ui, const UiNavEntry &entry)
{
    ui.page = entry.page;
    ui.context_index = entry.context_index;
    ui.parent_index = entry.parent_index;
    ui.selected = entry.selected;
    ui.scroll = entry.scroll;
}

void push_nav_entry(UiState &ui)
{
    constexpr size_t kMaxNavDepth = 16;
    if (!ui.back_stack.empty()) {
        const UiNavEntry &last = ui.back_stack.back();
        if (last.page == ui.page && last.context_index == ui.context_index && last.parent_index == ui.parent_index &&
            last.selected == ui.selected && last.scroll == ui.scroll) {
            return;
        }
    }
    if (ui.back_stack.size() >= kMaxNavDepth) {
        ui.back_stack.erase(ui.back_stack.begin());
    }
    ui.back_stack.push_back(capture_nav_entry(ui));
}

void reset_page_position(UiState &ui)
{
    ui.selected = 0;
    ui.scroll = 0;
    ui.context_index = 0;
    ui.parent_index = 0;
}

void navigate_to(UiState &ui, Page page, bool reset_position = true)
{
    if (ui.page != page) {
        push_nav_entry(ui);
    }
    ui.previous_page = ui.page;
    ui.page = page;
    if (reset_position) {
        reset_page_position(ui);
    }
}

void navigate_back(UiState &ui, Page fallback, size_t fallback_selected = 0)
{
    if (!ui.back_stack.empty()) {
        const UiNavEntry entry = ui.back_stack.back();
        ui.back_stack.pop_back();
        restore_nav_entry(ui, entry);
        return;
    }
    ui.page = fallback;
    ui.selected = fallback_selected;
    ui.scroll = 0;
}

void reset_to_home(UiState &ui)
{
    ui.page = Page::LibraryHome;
    ui.previous_page = Page::LibraryHome;
    ui.action_return_page = Page::LibraryRoot;
    ui.context_index = 0;
    ui.parent_index = 0;
    ui.selected = 0;
    ui.scroll = 0;
    ui.selected_tracks.clear();
    ui.action_tracks.clear();
    ui.action_label.clear();
    ui.back_stack.clear();
}

void begin_library_action(UiState &ui,
                          std::vector<size_t> tracks,
                          const std::string &label,
                          Page return_page)
{
    ui.previous_page = ui.page;
    ui.action_return_page = return_page;
    ui.action_tracks = std::move(tracks);
    ui.action_label = label;
    navigate_to(ui, Page::LibraryAction);
    ui.selected = 0;
    ui.scroll = 0;
}

} // namespace

bool is_audio_path(const std::string &path)
{
    const std::string ext = extension_of(path);
    return ext == "mp3" || ext == "flac" || ext == "wav" || ext == "aac" || ext == "m4a" || ext == "ogg";
}

Track infer_track_from_path(const std::string &path, uint64_t mtime)
{
    Track track;
    track.path = path;
    track.format = extension_of(path);
    track.mtime = mtime;
    track.id = make_id("track", path);

    const std::vector<std::string> parts = relative_music_parts(path);
    const std::string raw_title = basename_without_ext(path);
    track.track_no = parse_track_number(raw_title);
    track.title = clean_inferred_title(raw_title);
    track.artist = "Unknown Artist";
    track.album = "Inbox";
    track.album_artist = track.artist;

    if (parts.size() == 2) {
        track.artist = parts[0];
        track.album = "Singles / Loose";
        track.album_artist = track.artist;
    } else if (parts.size() >= 3) {
        track.artist = parts[parts.size() - 3];
        track.album = parts[parts.size() - 2];
        track.album_artist = track.artist;
    }

    if (!parts.empty() && lower_copy(parts[0]) == "various artists") {
        track.album_artist = "Various Artists";
        if (parts.size() >= 3) {
            track.album = parts[1];
        }
        const std::string title = clean_inferred_title(raw_title);
        const size_t sep = title.find(" - ");
        if (sep != std::string::npos && sep > 0 && sep + 3 < title.size()) {
            track.artist = title.substr(0, sep);
            track.title = title.substr(sep + 3);
        } else {
            track.artist = "Various Artists";
        }
    }

    if (track.title.empty()) {
        track.title = raw_title.empty() ? "Untitled" : raw_title;
    }
    track.title = normalize_japanese_voicing_marks(track.title);
    track.artist = normalize_japanese_voicing_marks(track.artist);
    track.album = normalize_japanese_voicing_marks(track.album);
    track.album_artist = normalize_japanese_voicing_marks(track.album_artist);
    return track;
}

LibraryIndex build_library_index(const std::vector<std::string> &paths)
{
    LibraryIndex index;
    std::map<std::string, size_t> album_by_id;
    std::map<std::string, size_t> artist_by_name;
    std::map<std::string, size_t> folder_by_id;

    for (const std::string &path : paths) {
        const std::vector<std::string> rel = relative_music_parts(path);
        if (!rel.empty() && (rel[0] == ".lofi" || rel[0] == "LOFI" || rel[0] == "Playlists")) {
            continue;
        }
        if (!is_audio_path(path)) {
            continue;
        }
        Track track = infer_track_from_path(path);
        const size_t track_index = index.tracks.size();
        index.tracks.push_back(track);

        const std::string folder_id = folder_id_for_track(path);
        auto folder_it = folder_by_id.find(folder_id);
        if (folder_it == folder_by_id.end()) {
            Folder folder;
            folder.id = folder_id;
            folder.label = folder_label_from_id(folder_id);
            folder.path = folder_id;
            folder.track_indices.push_back(track_index);
            folder_by_id[folder.id] = index.folders.size();
            index.folders.push_back(folder);
        } else {
            index.folders[folder_it->second].track_indices.push_back(track_index);
        }

        const std::string album_id = make_id("album", track.album_artist, track.album);
        auto album_it = album_by_id.find(album_id);
        if (album_it == album_by_id.end()) {
            Album album;
            album.id = album_id;
            album.title = track.album;
            album.album_artist = track.album_artist;
            const std::vector<std::string> parts = split_path(path);
            if (parts.size() >= 2) {
                album.source_folder = parts[parts.size() - 2];
            }
            album.track_indices.push_back(track_index);
            album_by_id[album.id] = index.albums.size();
            index.albums.push_back(album);
        } else {
            index.albums[album_it->second].track_indices.push_back(track_index);
        }

        auto artist_it = artist_by_name.find(track.album_artist);
        if (artist_it == artist_by_name.end()) {
            Artist artist;
            artist.name = track.album_artist;
            artist_by_name[artist.name] = index.artists.size();
            index.artists.push_back(artist);
        }
    }

    sort_album_tracks(index);
    sort_folder_tracks(index);

    std::sort(index.albums.begin(), index.albums.end(), [](const Album &a, const Album &b) {
        if (lower_copy(a.album_artist) != lower_copy(b.album_artist)) {
            return lower_copy(a.album_artist) < lower_copy(b.album_artist);
        }
        return lower_copy(a.title) < lower_copy(b.title);
    });

    index.artists.clear();
    artist_by_name.clear();
    for (size_t album_index = 0; album_index < index.albums.size(); ++album_index) {
        Album &album = index.albums[album_index];
        auto artist_it = artist_by_name.find(album.album_artist);
        if (artist_it == artist_by_name.end()) {
            Artist artist;
            artist.name = album.album_artist;
            artist_by_name[artist.name] = index.artists.size();
            index.artists.push_back(artist);
            artist_it = artist_by_name.find(album.album_artist);
        }
        Artist &artist = index.artists[artist_it->second];
        artist.album_indices.push_back(album_index);
        if (album.title == "Singles / Loose" || album.title == "Inbox") {
            artist.loose_track_count += album.track_indices.size();
        }
    }

    std::sort(index.artists.begin(), index.artists.end(), [](const Artist &a, const Artist &b) {
        return lower_copy(a.name) < lower_copy(b.name);
    });

    std::sort(index.folders.begin(), index.folders.end(), [](const Folder &a, const Folder &b) {
        return lower_copy(a.path) < lower_copy(b.path);
    });

    build_virtual_playlists(index);

    if (index.tracks.empty()) {
        index.warnings.push_back("No playable files found under /Music");
    }
    return index;
}

Queue make_album_queue(const LibraryIndex &index, const std::string &album_id, bool shuffle, uint32_t seed)
{
    Queue queue;
    queue.source_type = "album";
    queue.source_id = album_id;
    queue.shuffle = shuffle;
    queue.shuffle_seed = seed;
    for (const Album &album : index.albums) {
        if (album.id == album_id) {
            queue.track_indices = album.track_indices;
            break;
        }
    }
    if (shuffle) {
        shuffle_indices(queue.track_indices, seed);
    }
    return queue;
}

Queue make_artist_queue(const LibraryIndex &index, const std::string &artist_name, bool shuffle, uint32_t seed)
{
    Queue queue;
    queue.source_type = "artist";
    queue.source_id = artist_name;
    queue.shuffle = shuffle;
    queue.shuffle_seed = seed;
    for (const Artist &artist : index.artists) {
        if (artist.name != artist_name) {
            continue;
        }
        for (size_t album_index : artist.album_indices) {
            if (album_index >= index.albums.size()) {
                continue;
            }
            const Album &album = index.albums[album_index];
            queue.track_indices.insert(queue.track_indices.end(), album.track_indices.begin(), album.track_indices.end());
        }
        break;
    }
    if (shuffle) {
        shuffle_indices(queue.track_indices, seed);
    }
    return queue;
}

Queue make_all_tracks_queue(const LibraryIndex &index, bool shuffle, uint32_t seed)
{
    Queue queue;
    queue.source_type = "library";
    queue.source_id = "all";
    queue.shuffle = shuffle;
    queue.shuffle_seed = seed;
    for (size_t i = 0; i < index.tracks.size(); ++i) {
        queue.track_indices.push_back(i);
    }
    if (shuffle) {
        shuffle_indices(queue.track_indices, seed);
    }
    return queue;
}

Queue make_folder_queue(const LibraryIndex &index, size_t selected_order_index, bool shuffle, uint32_t seed)
{
    if (index.folders.empty()) {
        Queue queue;
        queue.source_type = "folder";
        queue.source_id = "/Music";
        queue.shuffle = shuffle;
        queue.shuffle_seed = seed;
        return queue;
    }
    if (selected_order_index >= index.folders.size()) {
        selected_order_index = index.folders.size() - 1;
    }
    return make_folder_queue(index, index.folders[selected_order_index].id, shuffle, seed);
}

Queue make_folder_queue(const LibraryIndex &index, const std::string &folder_id, bool shuffle, uint32_t seed)
{
    Queue queue;
    queue.source_type = "folder";
    queue.source_id = folder_id.empty() ? "/Music" : folder_id;
    queue.shuffle = shuffle;
    queue.shuffle_seed = seed;
    for (const Folder &folder : index.folders) {
        if (folder.id == queue.source_id) {
            queue.track_indices = folder.track_indices;
            break;
        }
    }
    if (shuffle) {
        shuffle_indices(queue.track_indices, seed);
    }
    return queue;
}

Queue make_playlist_queue(const LibraryIndex &index, const std::string &playlist_id, bool shuffle, uint32_t seed)
{
    Queue queue;
    queue.source_type = "playlist";
    queue.source_id = playlist_id;
    queue.shuffle = shuffle;
    queue.shuffle_seed = seed;
    for (const Playlist &playlist : index.playlists) {
        if (playlist.id == playlist_id) {
            queue.track_indices = playlist.track_indices;
            break;
        }
    }
    if (shuffle) {
        shuffle_indices(queue.track_indices, seed);
    }
    return queue;
}

int queue_current_track(const Queue &queue)
{
    if (queue.track_indices.empty() || queue.current_index >= queue.track_indices.size()) {
        return -1;
    }
    return static_cast<int>(queue.track_indices[queue.current_index]);
}

int queue_next(Queue &queue, RepeatMode repeat)
{
    if (queue.track_indices.empty()) {
        return -1;
    }
    if (repeat == RepeatMode::One) {
        return queue_current_track(queue);
    }
    if (queue.current_index + 1 < queue.track_indices.size()) {
        ++queue.current_index;
        return queue_current_track(queue);
    }
    if (repeat == RepeatMode::Album || repeat == RepeatMode::All) {
        queue.current_index = 0;
        return queue_current_track(queue);
    }
    return -1;
}

int queue_previous(Queue &queue)
{
    if (queue.track_indices.empty()) {
        return -1;
    }
    if (queue.current_index > 0) {
        --queue.current_index;
    }
    return queue_current_track(queue);
}

LofiProfile lofi_preset(LofiPreset preset)
{
    LofiProfile profile;
    profile.preset = preset;
    switch (preset) {
    case LofiPreset::WarmTape:
        profile.intensity = 74;
        profile.warmth = 88;
        profile.noise = 24;
        profile.wobble = 36;
        profile.space = 12;
        profile.softness = 58;
        break;
    case LofiPreset::VinylCafe:
        profile.intensity = 76;
        profile.warmth = 60;
        profile.noise = 72;
        profile.wobble = 24;
        profile.space = 50;
        profile.softness = 48;
        break;
    case LofiPreset::RainyWindow:
        profile.intensity = 78;
        profile.warmth = 46;
        profile.noise = 46;
        profile.wobble = 18;
        profile.space = 82;
        profile.softness = 86;
        break;
    case LofiPreset::TinyRadio:
        profile.intensity = 82;
        profile.warmth = 26;
        profile.noise = 32;
        profile.wobble = 14;
        profile.space = 8;
        profile.softness = 88;
        break;
    case LofiPreset::LateNight:
        profile.intensity = 58;
        profile.warmth = 56;
        profile.noise = 18;
        profile.wobble = 12;
        profile.space = 20;
        profile.softness = 78;
        break;
    case LofiPreset::Custom:
        profile.intensity = 50;
        profile.warmth = 50;
        profile.noise = 20;
        profile.wobble = 10;
        profile.space = 20;
        profile.softness = 50;
        break;
    case LofiPreset::Off:
    default:
        break;
    }
    return profile;
}

const char *to_string(LofiPreset preset)
{
    switch (preset) {
    case LofiPreset::WarmTape:
        return "Warm Tape";
    case LofiPreset::VinylCafe:
        return "Vinyl Cafe";
    case LofiPreset::RainyWindow:
        return "Rainy Window";
    case LofiPreset::TinyRadio:
        return "Tiny Radio";
    case LofiPreset::LateNight:
        return "Late Night";
    case LofiPreset::Custom:
        return "Custom";
    case LofiPreset::Off:
    default:
        return "Off";
    }
}

const char *to_string(RepeatMode repeat)
{
    switch (repeat) {
    case RepeatMode::One:
        return "One";
    case RepeatMode::Album:
        return "Album";
    case RepeatMode::All:
        return "All";
    case RepeatMode::Off:
    default:
        return "Off";
    }
}

const char *to_string(Page page)
{
    switch (page) {
    case Page::NowPlaying:
        return "Now Playing";
    case Page::LibraryHome:
        return "Library";
    case Page::LibraryRoot:
        return "Library Root";
    case Page::Songs:
        return "Songs";
    case Page::Albums:
        return "Albums";
    case Page::Artists:
        return "Artists";
    case Page::ArtistAlbums:
        return "Artist";
    case Page::AlbumDetail:
        return "Album";
    case Page::LibraryAction:
        return "Library Action";
    case Page::Folder:
        return "Folders";
    case Page::FolderDetail:
        return "Folder";
    case Page::Playlists:
        return "Playlists";
    case Page::PlaylistDetail:
        return "Playlist";
    case Page::LofiPresets:
        return "Lo-Fi";
    case Page::LofiEdit:
        return "Lo-Fi Edit";
    case Page::PlaybackMenu:
        return "Playback Menu";
    case Page::Queue:
        return "Queue";
    case Page::Scan:
        return "Scan";
    case Page::Empty:
    default:
        return "Empty";
    }
}

LofiPreset preset_from_string(const std::string &value)
{
    const std::string lower = lower_copy(value);
    if (lower == "warm tape") {
        return LofiPreset::WarmTape;
    }
    if (lower == "vinyl cafe" || lower == "vinyl café") {
        return LofiPreset::VinylCafe;
    }
    if (lower == "rainy window") {
        return LofiPreset::RainyWindow;
    }
    if (lower == "tiny radio") {
        return LofiPreset::TinyRadio;
    }
    if (lower == "late night") {
        return LofiPreset::LateNight;
    }
    if (lower == "custom") {
        return LofiPreset::Custom;
    }
    return LofiPreset::Off;
}

RepeatMode repeat_from_string(const std::string &value)
{
    const std::string lower = lower_copy(value);
    if (lower == "one") {
        return RepeatMode::One;
    }
    if (lower == "album") {
        return RepeatMode::Album;
    }
    if (lower == "all") {
        return RepeatMode::All;
    }
    return RepeatMode::Off;
}

int audio_volume_from_user_percent(int volume)
{
    volume = std::max(0, std::min(100, volume));
    if (volume <= kVolumeBoostThreshold) {
        return 50 + (volume * 20 + 40) / 80;
    }
    return 70 + ((volume - kVolumeBoostThreshold) * 10 + 10) / 20;
}

std::string serialize_playback_state(const PlaybackState &state)
{
    std::ostringstream out;
    out << "current_track=" << state.current_track << "\n";
    out << "position_seconds=" << state.position_seconds << "\n";
    out << "volume=" << normalize_user_volume_percent(state.volume) << "\n";
    out << "brightness_percent=" << normalize_brightness_percent(state.brightness_percent) << "\n";
    out << "screen_off_seconds=" << normalize_screen_off_seconds(state.screen_off_seconds) << "\n";
    out << "repeat=" << to_string(state.repeat) << "\n";
    out << "playing=" << (state.playing ? 1 : 0) << "\n";
    out << "queue_source_type=" << state.queue.source_type << "\n";
    out << "queue_source_id=" << state.queue.source_id << "\n";
    out << "queue_index=" << state.queue.current_index << "\n";
    out << "shuffle=" << (state.queue.shuffle ? 1 : 0) << "\n";
    out << "shuffle_seed=" << state.queue.shuffle_seed << "\n";
    out << "lofi_preset=" << to_string(state.lofi.preset) << "\n";
    out << "lofi_intensity=" << state.lofi.intensity << "\n";
    out << "lofi_warmth=" << state.lofi.warmth << "\n";
    out << "lofi_noise=" << state.lofi.noise << "\n";
    out << "lofi_wobble=" << state.lofi.wobble << "\n";
    out << "lofi_space=" << state.lofi.space << "\n";
    out << "lofi_softness=" << state.lofi.softness << "\n";
    return out.str();
}

bool parse_playback_state(const std::string &text, PlaybackState &out)
{
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const std::string key = line.substr(0, eq);
        const std::string value = line.substr(eq + 1);
        if (key == "current_track") {
            out.current_track = parse_int_or(value, out.current_track);
        } else if (key == "position_seconds") {
            out.position_seconds = parse_int_or(value, out.position_seconds);
        } else if (key == "volume") {
            out.volume = normalize_user_volume_percent(parse_int_or(value, out.volume));
        } else if (key == "brightness_percent") {
            out.brightness_percent = normalize_brightness_percent(parse_int_or(value, out.brightness_percent));
        } else if (key == "screen_off_seconds") {
            out.screen_off_seconds = normalize_screen_off_seconds(parse_int_or(value, out.screen_off_seconds));
        } else if (key == "repeat") {
            out.repeat = repeat_from_string(value);
        } else if (key == "playing") {
            out.playing = value == "1";
        } else if (key == "queue_source_type") {
            out.queue.source_type = value;
        } else if (key == "queue_source_id") {
            out.queue.source_id = value;
        } else if (key == "queue_index") {
            out.queue.current_index = static_cast<size_t>(std::max(0, parse_int_or(value, static_cast<int>(out.queue.current_index))));
        } else if (key == "shuffle") {
            out.queue.shuffle = value == "1";
        } else if (key == "shuffle_seed") {
            out.queue.shuffle_seed = parse_u32_or(value, out.queue.shuffle_seed);
        } else if (key == "lofi_preset") {
            out.lofi.preset = preset_from_string(value);
        } else if (key == "lofi_intensity") {
            out.lofi.intensity = parse_int_or(value, out.lofi.intensity);
        } else if (key == "lofi_warmth") {
            out.lofi.warmth = parse_int_or(value, out.lofi.warmth);
        } else if (key == "lofi_noise") {
            out.lofi.noise = parse_int_or(value, out.lofi.noise);
        } else if (key == "lofi_wobble") {
            out.lofi.wobble = parse_int_or(value, out.lofi.wobble);
        } else if (key == "lofi_space") {
            out.lofi.space = parse_int_or(value, out.lofi.space);
        } else if (key == "lofi_softness") {
            out.lofi.softness = parse_int_or(value, out.lofi.softness);
        }
    }
    if (out.lofi.preset != LofiPreset::Off && out.lofi.preset != LofiPreset::Custom) {
        out.lofi = lofi_preset(out.lofi.preset);
    }
    out.brightness_percent = normalize_brightness_percent(out.brightness_percent);
    out.screen_off_seconds = normalize_screen_off_seconds(out.screen_off_seconds);
    return true;
}

bool restore_playback_queue(const LibraryIndex &index, PlaybackState &state, bool resume_playing)
{
    Queue restored;
    if (state.queue.source_type == "album") {
        restored = make_album_queue(index, state.queue.source_id, state.queue.shuffle, state.queue.shuffle_seed);
    } else if (state.queue.source_type == "artist") {
        restored = make_artist_queue(index, state.queue.source_id, state.queue.shuffle, state.queue.shuffle_seed);
    } else if (state.queue.source_type == "library") {
        restored = make_all_tracks_queue(index, state.queue.shuffle, state.queue.shuffle_seed);
    } else if (state.queue.source_type == "folder") {
        restored = make_folder_queue(index, state.queue.source_id, state.queue.shuffle, state.queue.shuffle_seed);
    } else if (state.queue.source_type == "playlist") {
        restored = make_playlist_queue(index, state.queue.source_id, state.queue.shuffle, state.queue.shuffle_seed);
    } else {
        state.current_track = -1;
        state.playing = false;
        return false;
    }

    if (restored.track_indices.empty()) {
        state.queue = restored;
        state.current_track = -1;
        state.playing = false;
        return false;
    }

    if (state.queue.current_index >= restored.track_indices.size()) {
        restored.current_index = restored.track_indices.size() - 1;
    } else {
        restored.current_index = state.queue.current_index;
    }
    state.queue = restored;
    state.current_track = queue_current_track(state.queue);
    state.playing = resume_playing && state.current_track >= 0;
    if (state.position_seconds < 0) {
        state.position_seconds = 0;
    }
    return state.current_track >= 0;
}

PlaybackRestoreResult restore_saved_playback_state(const LibraryIndex &index,
                                                   const PlaybackState &saved,
                                                   PlaybackState &state,
                                                   bool resume_playing)
{
    PlaybackRestoreResult result;

    PlaybackState restored = state;
    restored.volume = normalize_user_volume_percent(saved.volume);
    restored.brightness_percent = normalize_brightness_percent(saved.brightness_percent);
    restored.screen_off_seconds = normalize_screen_off_seconds(saved.screen_off_seconds);
    restored.repeat = saved.repeat;
    restored.lofi = saved.lofi;
    restored.queue.shuffle = saved.queue.shuffle;
    restored.queue.shuffle_seed = saved.queue.shuffle_seed;
    result.settings_restored = true;

    PlaybackState queue_candidate = saved;
    if (restore_playback_queue(index, queue_candidate, resume_playing)) {
        queue_candidate.volume = restored.volume;
        queue_candidate.brightness_percent = restored.brightness_percent;
        queue_candidate.screen_off_seconds = restored.screen_off_seconds;
        queue_candidate.repeat = restored.repeat;
        queue_candidate.lofi = restored.lofi;
        restored = queue_candidate;
        result.queue_restored = true;
    }

    state = restored;
    return result;
}

std::string serialize_queue_snapshot(const LibraryIndex &index, const PlaybackState &state)
{
    std::vector<std::string> paths;
    paths.reserve(state.queue.track_indices.size());
    for (size_t track_index : state.queue.track_indices) {
        if (track_index < index.tracks.size()) {
            paths.push_back(index.tracks[track_index].path);
        }
    }

    std::ostringstream out;
    out << "version=1\n";
    out << "source_type=" << state.queue.source_type << "\n";
    out << "source_id=" << state.queue.source_id << "\n";
    out << "queue_index=" << state.queue.current_index << "\n";
    out << "shuffle=" << (state.queue.shuffle ? 1 : 0) << "\n";
    out << "shuffle_seed=" << state.queue.shuffle_seed << "\n";
    if (state.queue.current_index < state.queue.track_indices.size()) {
        const size_t current_track = state.queue.track_indices[state.queue.current_index];
        if (current_track < index.tracks.size()) {
            out << "current_path=" << index.tracks[current_track].path << "\n";
        }
    }
    out << "track_count=" << paths.size() << "\n";
    for (const std::string &path : paths) {
        out << "track=" << path << "\n";
    }
    return out.str();
}

QueueSnapshotRestoreResult restore_queue_snapshot(const LibraryIndex &index,
                                                  const std::string &text,
                                                  PlaybackState &state,
                                                  bool resume_playing)
{
    Queue restored;
    restored.source_type = "selection";
    restored.source_id = "QUEUE";
    std::string current_path;
    std::vector<std::string> paths;

    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const std::string key = line.substr(0, eq);
        const std::string value = line.substr(eq + 1);
        if (key == "source_type") {
            restored.source_type = value.empty() ? "selection" : value;
        } else if (key == "source_id") {
            restored.source_id = value.empty() ? "QUEUE" : value;
        } else if (key == "queue_index") {
            restored.current_index = static_cast<size_t>(std::max(0, parse_int_or(value, static_cast<int>(restored.current_index))));
        } else if (key == "shuffle") {
            restored.shuffle = value == "1";
        } else if (key == "shuffle_seed") {
            restored.shuffle_seed = parse_u32_or(value, restored.shuffle_seed);
        } else if (key == "current_path") {
            current_path = value;
        } else if (key == "track") {
            paths.push_back(value);
        }
    }

    QueueSnapshotRestoreResult result;
    result.saved_count = paths.size();
    if (paths.empty()) {
        state.queue = restored;
        state.current_track = -1;
        state.playing = false;
        return result;
    }

    std::map<std::string, size_t> path_to_track;
    for (size_t i = 0; i < index.tracks.size(); ++i) {
        path_to_track[index.tracks[i].path] = i;
    }

    size_t restored_current_index = restored.current_index;
    bool found_current_path = false;
    for (const std::string &path : paths) {
        const auto it = path_to_track.find(path);
        if (it == path_to_track.end()) {
            ++result.missing_count;
            continue;
        }
        if (!current_path.empty() && path == current_path) {
            restored_current_index = restored.track_indices.size();
            found_current_path = true;
        }
        restored.track_indices.push_back(it->second);
    }

    result.restored_count = restored.track_indices.size();
    if (restored.track_indices.empty()) {
        state.queue = restored;
        state.current_track = -1;
        state.playing = false;
        return result;
    }

    if (found_current_path) {
        restored.current_index = restored_current_index;
        result.current_restored = true;
    } else if (restored.current_index >= restored.track_indices.size()) {
        restored.current_index = restored.track_indices.size() - 1;
    }

    state.queue = restored;
    state.current_track = queue_current_track(state.queue);
    state.playing = resume_playing && state.current_track >= 0;
    if (!result.current_restored && !current_path.empty()) {
        state.position_seconds = 0;
    }
    result.restored = state.current_track >= 0;
    return result;
}

ScreenModel render_screen(const LibraryIndex &index, const PlaybackState &playback, const UiState &ui)
{
    ScreenModel screen;
    screen.title = to_string(ui.page);
    screen.status = std::string("SD ") + (index.tracks.empty() ? "EMPTY" : "OK") + " | Vol " +
                    std::to_string(playback.volume) + " | LF " + to_string(playback.lofi.preset);
    screen.volume_percent = playback.volume;
    if (ui.page == Page::LofiPresets || ui.page == Page::LofiEdit) {
        screen.meta = "Intensity " + std::to_string((std::max(0, std::min(100, playback.lofi.intensity)) + 5) / 10) +
                      "/10";
    }
    screen.soft_left = "Back";
    screen.soft_center = "OK";
    screen.soft_right = "Menu";

    const int current = queue_current_track(playback.queue);
    if (ui.page == Page::NowPlaying) {
        screen.soft_left = "Menu";
        screen.soft_center = playback.playing ? "Pause" : "Play";
        screen.soft_right = "LoFi";
        if (current >= 0 && static_cast<size_t>(current) < index.tracks.size()) {
            const Track &track = index.tracks[static_cast<size_t>(current)];
            screen.position_seconds = playback.position_seconds;
            screen.duration_seconds = track.duration_seconds;
            if (!playback.playing || playback.position_seconds > 0) {
                screen.album_art_cache_path = track.album_art_cache_path;
            }
            screen.rows.push_back({track.title, playback.playing ? ">" : "||"});
            screen.rows.push_back({track.artist + " / " + track.album, ""});
            screen.rows.push_back({"Pos " + std::to_string(playback.position_seconds) + "s", std::string("Repeat ") + to_string(playback.repeat)});
            screen.rows.push_back({"Queue " + std::to_string(playback.queue.current_index + 1) + "/" + std::to_string(playback.queue.track_indices.size()), playback.queue.shuffle ? "Shuffle" : ""});
        } else {
            screen.rows.push_back({"No track selected", ""});
            screen.rows.push_back({"Open Library to play", ""});
        }
    } else if (ui.page == Page::LibraryHome) {
        screen.soft_center = "Open";
        screen.soft_right = "Menu";
        static const char *items[] = {"Library", "Queue", "Lo-Fi", "Settings", "Now Playing"};
        const size_t selected = ui.selected < 5 ? ui.selected : 0;
        for (size_t i = 0; i < kLibraryRootVisibleRows; ++i) {
            std::string right;
            if (i == 0) {
                right = std::to_string(index.albums.size());
            } else if (i == 1) {
                right = playback.queue.track_indices.empty() ? "0" : std::to_string(playback.queue.track_indices.size());
            } else if (i == 2) {
                right = first_n(to_string(playback.lofi.preset), 6);
            } else if (i == 3) {
                right = "MENU";
            } else if (i == 4 && playback.current_track >= 0) {
                right = "NOW";
            }
            screen.rows.push_back({std::string(i == selected ? "> " : "  ") + items[i], right});
        }
    } else if (ui.page == Page::LibraryRoot) {
        screen.title = "Library Root";
        screen.subtitle = "ALL LIBRARY";
        screen.meta = std::to_string(index.tracks.size()) + " SONGS";
        screen.soft_left = "Back";
        screen.soft_center = "Open";
        screen.soft_right = "Menu";
        struct LibraryRootItem {
            const char *label;
            std::string right;
        };
        const LibraryRootItem items[] = {
            {"SONGS", count3(index.tracks.size())},
            {"ARTISTS", count3(index.artists.size())},
            {"ALBUMS", count3(index.albums.size())},
            {"FOLDERS", count3(index.folders.size())},
            {"PLAYLISTS", count3(index.playlists.size())},
        };
        const size_t selected = std::min<size_t>(ui.selected, 4);
        for (size_t i = 0; i < kLibraryRootVisibleRows; ++i) {
            screen.rows.push_back({std::string(i == selected ? "> " : "  ") + items[i].label, items[i].right});
        }
        screen.status = "range=001-005/005";
    } else if (ui.page == Page::Songs) {
        screen.title = "Songs";
        screen.subtitle = "ALL LIBRARY";
        screen.meta = std::to_string(index.tracks.size()) + " SONGS";
        screen.soft_left = "Back";
        screen.soft_center = "Open";
        screen.soft_right = ui.selected == 0 ? "Menu" : "Select";
        const size_t total_rows = index.tracks.size() + 1;
        const size_t selected = total_rows == 0 ? 0 : std::min(ui.selected, total_rows - 1);
        const size_t visible = kLibraryListVisibleRows;
        const size_t start = list_window_start(ui.scroll, selected, total_rows, visible);
        const size_t end = std::min(total_rows, start + visible);
        for (size_t row_index = start; row_index < end; ++row_index) {
            if (row_index == 0) {
                screen.rows.push_back({std::string(row_index == selected ? "> " : "  ") + "PLAY ALL",
                                       count3(index.tracks.size())});
                continue;
            }
            const size_t track_index = row_index - 1;
            if (track_index >= index.tracks.size()) {
                continue;
            }
            const Track &track = index.tracks[track_index];
            const bool checked = has_track(ui.selected_tracks, track_index);
            screen.rows.push_back({std::string(row_index == selected ? "> " : "  ") + (checked ? "[x] " : "[ ] ") +
                                       first_n(track.title, 28),
                                   track.duration_seconds > 0 ? std::to_string(track.duration_seconds) : ""});
        }
        screen.status = "range=" + count3(start + 1) + "-" + count3(end) + "/" + count3(total_rows) +
                        " selected=" + std::to_string(ui.selected_tracks.size());
    } else if (ui.page == Page::Albums || ui.page == Page::AlbumDetail) {
        screen.soft_center = "Open";
        screen.soft_right = "Menu";
        if (index.albums.empty()) {
            screen.subtitle = "ALBUMS";
            screen.meta = "000 ALBUMS";
            screen.rows.push_back({"No albums", "Scan"});
        } else if (ui.page == Page::Albums) {
            screen.subtitle = "ALBUMS";
            screen.meta = std::to_string(index.albums.size()) + " ALBUMS";
            const size_t visible = kLibraryListVisibleRows;
            const size_t selected = std::min(ui.selected, index.albums.size() - 1);
            const size_t start = list_window_start(ui.scroll, selected, index.albums.size(), visible);
            const size_t end = std::min(index.albums.size(), start + visible);
            for (size_t i = start; i < end; ++i) {
                const Album &album = index.albums[i];
                screen.rows.push_back({std::string(i == selected ? "> " : "  ") + first_n(album.title, 24),
                                       count3(album.track_indices.size())});
            }
            screen.status = "range=" + count3(start + 1) + "-" + count3(end) + "/" + count3(index.albums.size());
        } else {
            const Album &album = index.albums[std::min(ui.context_index, index.albums.size() - 1)];
            screen.soft_left = "Back";
            screen.soft_center = "Open";
            screen.soft_right = ui.selected == 0 ? "Menu" : "Select";
            screen.subtitle = first_n(album.title, 24);
            screen.meta = (album.album_artist.empty() ? "UNKNOWN" : first_n(album.album_artist, 16)) + " / " +
                          std::to_string(album.track_indices.size()) + " SONGS";
            const size_t total_rows = album.track_indices.size() + 1;
            const size_t selected = total_rows == 0 ? 0 : std::min(ui.selected, total_rows - 1);
            const size_t visible = kLibraryListVisibleRows;
            const size_t start = list_window_start(ui.scroll, selected, total_rows, visible);
            const size_t end = std::min(total_rows, start + visible);
            for (size_t row_index = start; row_index < end; ++row_index) {
                if (row_index == 0) {
                    screen.rows.push_back({std::string(row_index == selected ? "> " : "  ") + "ALL TRACKS",
                                           count3(album.track_indices.size())});
                    continue;
                }
                const size_t album_row = row_index - 1;
                if (album_row >= album.track_indices.size()) {
                    continue;
                }
                const size_t track_index = album.track_indices[album_row];
                if (track_index >= index.tracks.size()) {
                    continue;
                }
                const Track &track = index.tracks[track_index];
                const bool checked = has_track(ui.selected_tracks, track_index);
                screen.rows.push_back({std::string(row_index == selected ? "> " : "  ") + (checked ? "[x] " : "[ ] ") +
                                           count3(album_row + 1) + " " + first_n(track.title, 22),
                                       track.duration_seconds > 0 ? std::to_string(track.duration_seconds) : ""});
            }
            screen.status = "range=" + count3(start + 1) + "-" + count3(end) + "/" + count3(total_rows) +
                            " selected=" + std::to_string(ui.selected_tracks.size());
        }
    } else if (ui.page == Page::Artists) {
        screen.soft_center = "Open";
        screen.soft_right = "Play";
        if (index.artists.empty()) {
            screen.rows.push_back({"No artists", "Scan"});
        } else {
            screen.subtitle = "ALL ARTISTS";
            screen.meta = std::to_string(index.artists.size()) + " ARTISTS";
            const size_t visible = kLibraryListVisibleRows;
            const size_t selected = std::min(ui.selected, index.artists.size() - 1);
            const size_t start = list_window_start(ui.scroll, selected, index.artists.size(), visible);
            const size_t end = std::min(index.artists.size(), start + visible);
            for (size_t i = start; i < end; ++i) {
                const Artist &artist = index.artists[i];
                screen.rows.push_back({std::string(i == selected ? "> " : "  ") + first_n(artist.name, 24),
                                       count3(artist_track_indices(index, artist).size())});
            }
            screen.status = "range=" + count3(start + 1) + "-" + count3(end) + "/" + count3(index.artists.size());
        }
    } else if (ui.page == Page::ArtistAlbums) {
        screen.soft_center = "Open";
        screen.soft_right = "Menu";
        if (ui.context_index >= index.artists.size()) {
            screen.rows.push_back({"No artist", "Back"});
        } else {
            const Artist &artist = index.artists[ui.context_index];
            const std::vector<size_t> artist_tracks = artist_track_indices(index, artist);
            screen.subtitle = first_n(artist.name, 24);
            screen.meta = std::to_string(artist_tracks.size()) + " SONGS / " +
                          std::to_string(artist.album_indices.size()) + " ALBUMS";
            const size_t total_rows = artist.album_indices.size() + 1;
            const size_t selected = total_rows == 0 ? 0 : std::min(ui.selected, total_rows - 1);
            const size_t visible = kLibraryListVisibleRows;
            const size_t start = list_window_start(ui.scroll, selected, total_rows, visible);
            const size_t end = std::min(total_rows, start + visible);
            for (size_t row_index = start; row_index < end; ++row_index) {
                if (row_index == 0) {
                    screen.rows.push_back({std::string(row_index == selected ? "> " : "  ") + "ALL SONGS",
                                           count3(artist_tracks.size())});
                    continue;
                }
                const size_t album_row = row_index - 1;
                const size_t album_index = artist.album_indices[album_row];
                if (album_index >= index.albums.size()) {
                    continue;
                }
                const Album &album = index.albums[album_index];
                screen.rows.push_back({std::string(row_index == selected ? "> " : "  ") + first_n(album.title, 24),
                                       count3(album.track_indices.size()) + " >"});
            }
            screen.status = "range=" + count3(start + 1) + "-" + count3(end) + "/" + count3(total_rows);
        }
    } else if (ui.page == Page::LibraryAction) {
        screen.title = "Library Action";
        screen.subtitle = std::to_string(ui.action_tracks.size()) + " TRACKS SELECTED";
        screen.meta = ui.action_label.empty() ? "LIBRARY" : first_n(ui.action_label, 24);
        screen.soft_left = "Back";
        screen.soft_center = "OK";
        screen.soft_right = "OK";
        static const char *actions[] = {"REPLACE QUEUE", "PLAY NEXT", "ADD TO END", "ADD TO FRONT", "CANCEL"};
        const size_t selected = std::min<size_t>(ui.selected, 4);
        const size_t action_count = 5;
        const size_t start = list_window_start(ui.scroll, selected, action_count, kLibraryListVisibleRows);
        const size_t end = std::min(action_count, start + kLibraryListVisibleRows);
        for (size_t i = start; i < end; ++i) {
            screen.rows.push_back({std::string(i == selected ? "> " : "  ") + actions[i], ""});
        }
    } else if (ui.page == Page::Folder) {
        screen.soft_left = "Back";
        screen.soft_center = "Open";
        screen.soft_right = "Menu";
        screen.subtitle = "FOLDERS";
        screen.meta = std::to_string(index.folders.size()) + " FOLDERS";
        if (index.folders.empty()) {
            screen.rows.push_back({"No folders", "Scan"});
            screen.rows.push_back({"Put music in /Music", ""});
        } else {
            const size_t visible = kLibraryListVisibleRows;
            const size_t selected = std::min(ui.selected, index.folders.size() - 1);
            const size_t start = list_window_start(ui.scroll, selected, index.folders.size(), visible);
            const size_t end = std::min(index.folders.size(), start + visible);
            for (size_t i = start; i < end; ++i) {
                const Folder &folder = index.folders[i];
                screen.rows.push_back({std::string(i == selected ? "> " : "  ") + first_n(folder.label, 24),
                                       count3(folder.track_indices.size())});
            }
            screen.status = "range=" + count3(start + 1) + "-" + count3(end) + "/" + count3(index.folders.size());
        }
    } else if (ui.page == Page::FolderDetail) {
        screen.soft_left = "Back";
        screen.soft_center = "Open";
        screen.soft_right = ui.selected == 0 ? "Menu" : "Select";
        if (ui.context_index >= index.folders.size()) {
            screen.rows.push_back({"No folder", "Back"});
        } else {
            const Folder &folder = index.folders[ui.context_index];
            screen.subtitle = first_n(folder.label, 24);
            screen.meta = std::to_string(folder.track_indices.size()) + " SONGS";
            const size_t total_rows = folder.track_indices.size() + 1;
            const size_t selected = total_rows == 0 ? 0 : std::min(ui.selected, total_rows - 1);
            const size_t visible = kLibraryListVisibleRows;
            const size_t start = list_window_start(ui.scroll, selected, total_rows, visible);
            const size_t end = std::min(total_rows, start + visible);
            for (size_t row_index = start; row_index < end; ++row_index) {
                if (row_index == 0) {
                    screen.rows.push_back({std::string(row_index == selected ? "> " : "  ") + "ALL TRACKS",
                                           count3(folder.track_indices.size())});
                    continue;
                }
                const size_t folder_row = row_index - 1;
                if (folder_row >= folder.track_indices.size()) {
                    continue;
                }
                const size_t track_index = folder.track_indices[folder_row];
                if (track_index >= index.tracks.size()) {
                    continue;
                }
                const Track &track = index.tracks[track_index];
                const bool checked = has_track(ui.selected_tracks, track_index);
                screen.rows.push_back({std::string(row_index == selected ? "> " : "  ") + (checked ? "[x] " : "[ ] ") +
                                           first_n(track.title, 26),
                                       track.duration_seconds > 0 ? std::to_string(track.duration_seconds) : ""});
            }
            screen.status = "range=" + count3(start + 1) + "-" + count3(end) + "/" + count3(total_rows) +
                            " selected=" + std::to_string(ui.selected_tracks.size());
        }
    } else if (ui.page == Page::Playlists) {
        screen.soft_left = "Back";
        screen.soft_center = "Open";
        screen.soft_right = "Menu";
        screen.subtitle = "PLAYLISTS";
        screen.meta = std::to_string(index.playlists.size()) + " LISTS";
        if (index.playlists.empty()) {
            screen.rows.push_back({"No playlists", "Scan"});
        } else {
            const size_t visible = kLibraryListVisibleRows;
            const size_t selected = std::min(ui.selected, index.playlists.size() - 1);
            const size_t start = list_window_start(ui.scroll, selected, index.playlists.size(), visible);
            const size_t end = std::min(index.playlists.size(), start + visible);
            for (size_t i = start; i < end; ++i) {
                const Playlist &playlist = index.playlists[i];
                screen.rows.push_back({std::string(i == selected ? "> " : "  ") + first_n(playlist.title, 24),
                                       count3(playlist.track_indices.size())});
            }
            screen.status = "range=" + count3(start + 1) + "-" + count3(end) + "/" + count3(index.playlists.size());
        }
    } else if (ui.page == Page::PlaylistDetail) {
        screen.soft_left = "Back";
        screen.soft_center = "Open";
        screen.soft_right = ui.selected == 0 ? "Menu" : "Select";
        if (ui.context_index >= index.playlists.size()) {
            screen.rows.push_back({"No playlist", "Back"});
        } else {
            const Playlist &playlist = index.playlists[ui.context_index];
            screen.subtitle = first_n(playlist.title, 24);
            screen.meta = std::to_string(playlist.track_indices.size()) + " SONGS";
            const size_t total_rows = playlist.track_indices.size() + 1;
            const size_t selected = total_rows == 0 ? 0 : std::min(ui.selected, total_rows - 1);
            const size_t visible = kLibraryListVisibleRows;
            const size_t start = list_window_start(ui.scroll, selected, total_rows, visible);
            const size_t end = std::min(total_rows, start + visible);
            for (size_t row_index = start; row_index < end; ++row_index) {
                if (row_index == 0) {
                    screen.rows.push_back({std::string(row_index == selected ? "> " : "  ") + "ALL TRACKS",
                                           count3(playlist.track_indices.size())});
                    continue;
                }
                const size_t playlist_row = row_index - 1;
                if (playlist_row >= playlist.track_indices.size()) {
                    continue;
                }
                const size_t track_index = playlist.track_indices[playlist_row];
                if (track_index >= index.tracks.size()) {
                    continue;
                }
                const Track &track = index.tracks[track_index];
                const bool checked = has_track(ui.selected_tracks, track_index);
                screen.rows.push_back({std::string(row_index == selected ? "> " : "  ") + (checked ? "[x] " : "[ ] ") +
                                           first_n(track.title, 26),
                                       track.duration_seconds > 0 ? std::to_string(track.duration_seconds) : ""});
            }
            screen.status = "range=" + count3(start + 1) + "-" + count3(end) + "/" + count3(total_rows) +
                            " selected=" + std::to_string(ui.selected_tracks.size());
        }
    } else if (ui.page == Page::LofiPresets) {
        screen.soft_left = "Off";
        screen.soft_center = "Edit";
        screen.soft_right = "Apply";
        constexpr size_t kVisibleRows = 4;
        const size_t start = list_window_start(ui.scroll, std::min(ui.selected, kLofiPresetChoiceCount - 1),
                                               kLofiPresetChoiceCount, kVisibleRows);
        for (size_t i = start; i < kLofiPresetChoiceCount && screen.rows.size() < kVisibleRows; ++i) {
            screen.rows.push_back({std::string(i == ui.selected ? "> " : "  ") + to_string(kLofiPresetChoices[i]),
                                   kLofiPresetChoices[i] == playback.lofi.preset ? "ON" : ""});
        }
    } else if (ui.page == Page::LofiEdit) {
        screen.soft_left = "Back";
        screen.soft_center = "Save";
        screen.soft_right = "Adjust";
        const char *names[] = {"Intensity", "Warmth", "Noise", "Wobble", "Space", "Softness"};
        const int values[] = {playback.lofi.intensity, playback.lofi.warmth, playback.lofi.noise,
                              playback.lofi.wobble, playback.lofi.space, playback.lofi.softness};
        constexpr size_t kVisibleRows = 5;
        const size_t start = list_window_start(ui.scroll, std::min<size_t>(ui.selected, 5), 6, kVisibleRows);
        for (size_t i = start; i < 6 && screen.rows.size() < kVisibleRows; ++i) {
            screen.rows.push_back({std::string(i == ui.selected ? "> " : "  ") + names[i], std::to_string(values[i])});
        }
    } else if (ui.page == Page::PlaybackMenu) {
        screen.soft_left = "Back";
        screen.soft_center = "Toggle";
        screen.soft_right = "LoFi";
        const std::string queue_pos = playback.queue.track_indices.empty()
                                          ? "0/0"
                                          : std::to_string(playback.queue.current_index + 1) + "/" +
                                                std::to_string(playback.queue.track_indices.size());
        screen.rows.push_back({std::string(ui.selected == 0 ? "> " : "  ") + "Volume", std::to_string(playback.volume) + "%"});
        screen.rows.push_back({std::string(ui.selected == 1 ? "> " : "  ") + "Brightness",
                               std::to_string(normalize_brightness_percent(playback.brightness_percent)) + "%"});
        screen.rows.push_back({std::string(ui.selected == 2 ? "> " : "  ") + "Repeat", to_string(playback.repeat)});
        screen.rows.push_back({std::string(ui.selected == 3 ? "> " : "  ") + "Shuffle", playback.queue.shuffle ? "On" : "Off"});
        screen.rows.push_back({std::string(ui.selected == 4 ? "> " : "  ") + "Queue", queue_pos});
        screen.rows.push_back({std::string(ui.selected == 5 ? "> " : "  ") + "Screen Off",
                               format_screen_off_seconds(playback.screen_off_seconds)});
    } else if (ui.page == Page::Queue) {
        screen.soft_left = "Back";
        screen.soft_center = "Play";
        screen.soft_right = "+6";
        if (playback.queue.track_indices.empty()) {
            screen.status = "range=0-0/0 current=0";
            screen.rows.push_back({"Queue is empty", ""});
        } else {
            constexpr size_t kQueuePageSize = 6;
            const size_t total = playback.queue.track_indices.size();
            const size_t selected = std::min(ui.selected, total - 1);
            const size_t start = selected / kQueuePageSize * kQueuePageSize;
            const size_t end = std::min(total, start + kQueuePageSize);
            screen.status = "range=" + std::to_string(start + 1) + "-" + std::to_string(end) + "/" +
                            std::to_string(total) + " current=" + std::to_string(playback.queue.current_index + 1);
            for (size_t i = start; i < end; ++i) {
                const size_t track_index = playback.queue.track_indices[i];
                if (track_index >= index.tracks.size()) {
                    continue;
                }
                const Track &track = index.tracks[track_index];
                const bool is_current = i == playback.queue.current_index;
                const std::string marker = is_current ? "*" : " ";
                screen.rows.push_back({std::string(i == selected ? ">" : " ") + marker + " " + first_n(track.title, 28),
                                       is_current ? "NOW" : std::to_string(i + 1)});
            }
        }
    } else if (ui.page == Page::Scan) {
        screen.soft_left = "Cancel";
        screen.soft_center = "Hide";
        screen.soft_right = "Help";
        screen.rows.push_back({"Indexed tracks", std::to_string(index.tracks.size())});
        screen.rows.push_back({"Albums", std::to_string(index.albums.size())});
        screen.rows.push_back({"Artists", std::to_string(index.artists.size())});
    } else {
        screen.soft_left = "Back";
        screen.soft_center = "Scan";
        screen.soft_right = "Folder";
        screen.rows.push_back({"Open Library", ""});
        screen.rows.push_back({"Use /Music on SD", ""});
    }

    if (!ui.toast.empty()) {
        screen.status = ui.toast;
    }
    return screen;
}

void apply_action(const LibraryIndex &index, PlaybackState &playback, UiState &ui, Action action)
{
    if (action == Action::None) {
        return;
    }
    const bool volume_increase_action =
        (ui.page == Page::NowPlaying && action == Action::Up) ||
        (ui.page == Page::PlaybackMenu && ui.selected == 0 && (action == Action::Right || action == Action::Ok));
    if (!volume_increase_action) {
        ui.volume_boost_warning_armed = false;
    }

    auto move_selection = [&](size_t count, int delta) {
        if (count == 0) {
            ui.selected = 0;
            ui.scroll = 0;
            return;
        }
        if (delta < 0 && ui.selected > 0) {
            --ui.selected;
        } else if (delta > 0 && ui.selected + 1 < count) {
            ++ui.selected;
        }
        clamp_selection(ui, count);
    };
    auto open_library_action = [&](std::vector<size_t> tracks, const std::string &label) {
        if (tracks.empty()) {
            ui.toast = "No tracks";
            return;
        }
        begin_library_action(ui, std::move(tracks), label, ui.page);
    };
    auto replace_queue_and_play = [&](std::vector<size_t> tracks, const std::string &label) {
        if (tracks.empty()) {
            ui.toast = "No tracks";
            return;
        }
        const bool shuffle = playback.queue.shuffle;
        const uint32_t seed = shuffle ? playback.queue.shuffle_seed + 1 : playback.queue.shuffle_seed;
        playback.queue = make_explicit_queue(std::move(tracks), label, shuffle, seed);
        playback.current_track = queue_current_track(playback.queue);
        playback.position_seconds = 0;
        playback.playing = playback.current_track >= 0;
        navigate_to(ui, Page::NowPlaying);
    };
    auto preferred_shuffle = [&]() {
        return playback.queue.shuffle;
    };
    auto preferred_shuffle_seed = [&]() {
        return playback.queue.shuffle ? playback.queue.shuffle_seed + 1 : playback.queue.shuffle_seed;
    };

    ui.toast.clear();
    if (action == Action::Home) {
        reset_to_home(ui);
        return;
    }
    if (action == Action::Scan) {
        navigate_to(ui, Page::Scan);
        return;
    }
    if (action == Action::Lofi) {
        navigate_to(ui, Page::LofiPresets);
        return;
    }
    if (action == Action::Repeat) {
        playback.repeat = playback.repeat == RepeatMode::Off   ? RepeatMode::One
                          : playback.repeat == RepeatMode::One ? RepeatMode::Album
                          : playback.repeat == RepeatMode::Album ? RepeatMode::All
                                                                 : RepeatMode::Off;
        ui.toast = std::string("Repeat ") + to_string(playback.repeat);
        return;
    }
    if (action == Action::Shuffle) {
        const int previous_track = queue_current_track(playback.queue);
        playback.queue.shuffle = !playback.queue.shuffle;
        if (!playback.queue.track_indices.empty()) {
            if (playback.queue.source_type == "album") {
                playback.queue = make_album_queue(index, playback.queue.source_id, playback.queue.shuffle,
                                                  playback.queue.shuffle_seed + 1);
            } else if (playback.queue.source_type == "artist") {
                playback.queue = make_artist_queue(index, playback.queue.source_id, playback.queue.shuffle,
                                                   playback.queue.shuffle_seed + 1);
            } else if (playback.queue.source_type == "folder") {
                playback.queue = make_folder_queue(index, playback.queue.source_id, playback.queue.shuffle,
                                                   playback.queue.shuffle_seed + 1);
            } else if (playback.queue.source_type == "playlist") {
                playback.queue = make_playlist_queue(index, playback.queue.source_id, playback.queue.shuffle,
                                                     playback.queue.shuffle_seed + 1);
            } else {
                playback.queue = make_all_tracks_queue(index, playback.queue.shuffle, playback.queue.shuffle_seed + 1);
            }
            if (previous_track >= 0) {
                const size_t previous = static_cast<size_t>(previous_track);
                const auto it = std::find(playback.queue.track_indices.begin(), playback.queue.track_indices.end(), previous);
                if (it != playback.queue.track_indices.end()) {
                    playback.queue.current_index = static_cast<size_t>(it - playback.queue.track_indices.begin());
                }
            }
            playback.current_track = queue_current_track(playback.queue);
        }
        ui.toast = playback.queue.shuffle ? "Shuffle On" : "Shuffle Off";
        return;
    }

    switch (ui.page) {
    case Page::LibraryHome:
        if (ui.selected >= 5) {
            ui.selected = 0;
        }
        if (action == Action::Up || action == Action::Left) {
            ui.selected = ui.selected == 0 ? 4 : ui.selected - 1;
        } else if (action == Action::Down || action == Action::Right) {
            ui.selected = (ui.selected + 1) % 5;
        } else if (action == Action::Menu) {
            navigate_to(ui, Page::PlaybackMenu);
        } else if (action == Action::Ok) {
            if (ui.selected == 0) {
                navigate_to(ui, Page::LibraryRoot);
            } else if (ui.selected == 1) {
                navigate_to(ui, Page::Queue, false);
                ui.selected = playback.queue.current_index;
                ui.scroll = 0;
                clamp_selection(ui, playback.queue.track_indices.size());
            } else if (ui.selected == 2) {
                navigate_to(ui, Page::LofiPresets);
            } else if (ui.selected == 3) {
                navigate_to(ui, Page::PlaybackMenu);
            } else if (ui.selected == 4) {
                navigate_to(ui, Page::NowPlaying);
            }
        }
        break;
    case Page::LibraryRoot:
        if (action == Action::Up) {
            move_selection_wrapped(ui, 5, -1, kLibraryRootVisibleRows);
        } else if (action == Action::Down) {
            move_selection_wrapped(ui, 5, 1, kLibraryRootVisibleRows);
        } else if (action == Action::Left || action == Action::Back) {
            navigate_back(ui, Page::LibraryHome, 0);
        } else if (action == Action::Ok || action == Action::Right) {
            if (ui.selected == 0) {
                navigate_to(ui, Page::Songs);
                ui.selected_tracks.clear();
            } else if (ui.selected == 1) {
                navigate_to(ui, Page::Artists);
            } else if (ui.selected == 2) {
                navigate_to(ui, Page::Albums);
            } else if (ui.selected == 3) {
                navigate_to(ui, Page::Folder);
            } else {
                navigate_to(ui, Page::Playlists);
            }
        } else if (action == Action::Menu) {
            open_library_action(make_all_tracks_queue(index, false, 0).track_indices, "ALL LIBRARY");
        }
        break;
    case Page::Songs: {
        const size_t total_rows = index.tracks.size() + 1;
        if (action == Action::Up) {
            move_selection_wrapped(ui, total_rows, -1, kLibraryListVisibleRows);
        } else if (action == Action::Down) {
            move_selection_wrapped(ui, total_rows, 1, kLibraryListVisibleRows);
        } else if (action == Action::Left || action == Action::Back) {
            navigate_back(ui, Page::LibraryRoot, 0);
            ui.selected_tracks.clear();
        } else if (action == Action::Shuffle && ui.selected > 0 && ui.selected - 1 < index.tracks.size()) {
            toggle_track(ui.selected_tracks, ui.selected - 1);
        } else if (action == Action::Menu) {
            std::vector<size_t> tracks = ui.selected_tracks.empty()
                                             ? make_all_tracks_queue(index, false, 0).track_indices
                                             : ui.selected_tracks;
            open_library_action(std::move(tracks), ui.selected_tracks.empty() ? "ALL LIBRARY" : "SELECTED SONGS");
        } else if ((action == Action::Ok || action == Action::Right || action == Action::PlayPause)) {
            if (ui.selected == 0) {
                replace_queue_and_play(make_all_tracks_queue(index, false, 0).track_indices, "ALL LIBRARY");
            } else if (ui.selected - 1 < index.tracks.size()) {
                replace_queue_and_play(std::vector<size_t>{ui.selected - 1}, index.tracks[ui.selected - 1].title);
            }
        }
        break;
    }
    case Page::Albums:
        if (action == Action::Up) {
            move_selection_wrapped(ui, index.albums.size(), -1, 4);
        } else if (action == Action::Down) {
            move_selection_wrapped(ui, index.albums.size(), 1, 4);
        } else if ((action == Action::Ok || action == Action::Right) && ui.selected < index.albums.size()) {
            ui.previous_page = Page::Albums;
            push_nav_entry(ui);
            ui.context_index = ui.selected;
            ui.parent_index = 0;
            ui.page = Page::AlbumDetail;
            ui.selected = 0;
            ui.scroll = 0;
            ui.selected_tracks.clear();
        } else if (action == Action::PlayPause && ui.selected < index.albums.size()) {
            const Album &album = index.albums[ui.selected];
            playback.queue = make_album_queue(index, album.id, preferred_shuffle(), preferred_shuffle_seed());
            playback.current_track = queue_current_track(playback.queue);
            playback.position_seconds = 0;
            playback.playing = playback.current_track >= 0;
            navigate_to(ui, Page::NowPlaying);
        } else if (action == Action::Menu && ui.selected < index.albums.size()) {
            const Album &album = index.albums[ui.selected];
            open_library_action(album.track_indices, album.title);
        } else if (action == Action::Back || action == Action::Left) {
            navigate_back(ui, Page::LibraryRoot, 2);
        }
        break;
    case Page::AlbumDetail:
        if (action == Action::Back || action == Action::Left || ui.context_index >= index.albums.size()) {
            const size_t album_index = ui.context_index;
            if (!ui.back_stack.empty()) {
                navigate_back(ui, Page::Albums, std::min(album_index, index.albums.empty() ? 0 : index.albums.size() - 1));
            } else if (ui.previous_page == Page::ArtistAlbums && ui.parent_index < index.artists.size()) {
                const Artist &artist = index.artists[ui.parent_index];
                ui.page = Page::ArtistAlbums;
                ui.context_index = ui.parent_index;
                ui.selected = 0;
                for (size_t i = 0; i < artist.album_indices.size(); ++i) {
                    if (artist.album_indices[i] == album_index) {
                        ui.selected = i + 1;
                        break;
                    }
                }
                clamp_selection_window(ui, artist.album_indices.size() + 1, kLibraryListVisibleRows);
            } else {
                ui.page = Page::Albums;
                ui.selected = std::min(album_index, index.albums.empty() ? 0 : index.albums.size() - 1);
                clamp_selection_window(ui, index.albums.size(), kLibraryListVisibleRows);
            }
            ui.selected_tracks.clear();
        } else {
            const Album &album = index.albums[ui.context_index];
            if (action == Action::Up) {
                move_selection_wrapped(ui, album.track_indices.size() + 1, -1, kLibraryListVisibleRows);
            } else if (action == Action::Down) {
                move_selection_wrapped(ui, album.track_indices.size() + 1, 1, kLibraryListVisibleRows);
            } else if (action == Action::Shuffle && ui.selected > 0 && ui.selected - 1 < album.track_indices.size()) {
                toggle_track(ui.selected_tracks, album.track_indices[ui.selected - 1]);
            } else if (action == Action::Menu || action == Action::Right) {
                std::vector<size_t> tracks = ui.selected_tracks.empty() ? album.track_indices : ui.selected_tracks;
                open_library_action(std::move(tracks), album.title);
            } else if (action == Action::Ok || action == Action::PlayPause) {
                if (ui.selected == 0) {
                    replace_queue_and_play(album.track_indices, album.title);
                } else if (ui.selected - 1 < album.track_indices.size()) {
                    const size_t track_index = album.track_indices[ui.selected - 1];
                    const std::string label = track_index < index.tracks.size() ? index.tracks[track_index].title : album.title;
                    replace_queue_and_play(std::vector<size_t>{track_index}, label);
                }
            }
        }
        break;
    case Page::Artists:
        if (action == Action::Up) {
            move_selection_wrapped(ui, index.artists.size(), -1, kLibraryListVisibleRows);
        } else if (action == Action::Down) {
            move_selection_wrapped(ui, index.artists.size(), 1, kLibraryListVisibleRows);
        } else if ((action == Action::Ok || action == Action::Right) && ui.selected < index.artists.size()) {
            push_nav_entry(ui);
            ui.page = Page::ArtistAlbums;
            ui.context_index = ui.selected;
            ui.selected = 0;
            ui.scroll = 0;
        } else if ((action == Action::PlayPause || action == Action::Menu) && ui.selected < index.artists.size()) {
            playback.queue = make_artist_queue(index,
                                               index.artists[ui.selected].name,
                                               preferred_shuffle(),
                                               preferred_shuffle_seed());
            playback.current_track = queue_current_track(playback.queue);
            playback.position_seconds = 0;
            playback.playing = playback.current_track >= 0;
            navigate_to(ui, Page::NowPlaying);
        } else if (action == Action::Back || action == Action::Left) {
            navigate_back(ui, Page::LibraryRoot, 1);
        }
        break;
    case Page::ArtistAlbums:
        if (ui.context_index >= index.artists.size()) {
            navigate_back(ui, Page::Artists, 0);
        } else {
            const Artist &artist = index.artists[ui.context_index];
            const std::vector<size_t> artist_tracks = artist_track_indices(index, artist);
            if (action == Action::Up) {
                move_selection_wrapped(ui, artist.album_indices.size() + 1, -1, kLibraryListVisibleRows);
            } else if (action == Action::Down) {
                move_selection_wrapped(ui, artist.album_indices.size() + 1, 1, kLibraryListVisibleRows);
            } else if ((action == Action::Ok || action == Action::Right)) {
                if (ui.selected == 0) {
                    open_library_action(artist_tracks, artist.name);
                } else if (ui.selected - 1 < artist.album_indices.size()) {
                    const size_t album_index = artist.album_indices[ui.selected - 1];
                    if (album_index < index.albums.size()) {
                        ui.previous_page = Page::ArtistAlbums;
                        ui.parent_index = ui.context_index;
                        push_nav_entry(ui);
                        ui.context_index = album_index;
                        ui.page = Page::AlbumDetail;
                        ui.selected = 0;
                        ui.scroll = 0;
                        ui.selected_tracks.clear();
                    }
                }
            } else if ((action == Action::PlayPause || action == Action::Menu) && ui.context_index < index.artists.size()) {
                open_library_action(artist_tracks, artist.name);
            } else if (action == Action::Back || action == Action::Left) {
                navigate_back(ui, Page::Artists, ui.context_index);
                if (ui.page == Page::Artists) {
                    clamp_selection_window(ui, index.artists.size(), kLibraryListVisibleRows);
                }
            }
        }
        break;
    case Page::LibraryAction:
        if (action == Action::Up) {
            move_selection_wrapped(ui, 5, -1, kLibraryListVisibleRows);
        } else if (action == Action::Down) {
            move_selection_wrapped(ui, 5, 1, kLibraryListVisibleRows);
        } else if (action == Action::Back || action == Action::Left) {
            navigate_back(ui, ui.action_return_page, 0);
        } else if (action == Action::Ok || action == Action::Right || action == Action::PlayPause) {
            if (ui.selected == 4) {
                navigate_back(ui, ui.action_return_page, 0);
                break;
            }
            if (ui.action_tracks.empty()) {
                ui.toast = "No tracks";
                break;
            }
            if (ui.selected == 0) {
                replace_queue_and_play(ui.action_tracks, ui.action_label.empty() ? "SELECTION" : ui.action_label);
                ui.selected_tracks.clear();
                ui.action_tracks.clear();
                break;
            }
            if (playback.queue.track_indices.empty()) {
                const bool shuffle = playback.queue.shuffle;
                const uint32_t seed = shuffle ? playback.queue.shuffle_seed + 1 : playback.queue.shuffle_seed;
                playback.queue = make_explicit_queue(ui.action_tracks,
                                                     ui.action_label.empty() ? "SELECTION" : ui.action_label,
                                                     shuffle,
                                                     seed);
                playback.current_track = queue_current_track(playback.queue);
                playback.position_seconds = 0;
                playback.playing = playback.current_track >= 0;
                navigate_to(ui, Page::NowPlaying);
                break;
            }
            size_t insert_at = playback.queue.track_indices.size();
            if (ui.selected == 1) {
                insert_at = std::min(playback.queue.current_index + 1, playback.queue.track_indices.size());
            } else if (ui.selected == 3) {
                insert_at = 0;
            }
            playback.queue.track_indices.insert(playback.queue.track_indices.begin() + static_cast<long>(insert_at),
                                                ui.action_tracks.begin(),
                                                ui.action_tracks.end());
            if (ui.selected == 3) {
                playback.queue.current_index += ui.action_tracks.size();
            }
            playback.current_track = queue_current_track(playback.queue);
            ui.toast = ui.selected == 1 ? "Queued next" : (ui.selected == 2 ? "Added to end" : "Added to front");
            ui.selected_tracks.clear();
            ui.action_tracks.clear();
            navigate_to(ui, Page::Queue, false);
            ui.selected = insert_at;
            clamp_selection_window(ui, playback.queue.track_indices.size(), 6);
        }
        break;
    case Page::Folder:
        if (action == Action::Up) {
            move_selection_wrapped(ui, index.folders.size(), -1, 4);
        } else if (action == Action::Down) {
            move_selection_wrapped(ui, index.folders.size(), 1, 4);
        } else if ((action == Action::Ok || action == Action::Right) && ui.selected < index.folders.size()) {
            push_nav_entry(ui);
            ui.context_index = ui.selected;
            ui.page = Page::FolderDetail;
            ui.selected = 0;
            ui.scroll = 0;
            ui.selected_tracks.clear();
        } else if (action == Action::PlayPause && ui.selected < index.folders.size()) {
            const Folder &folder = index.folders[ui.selected];
            playback.queue = make_folder_queue(index, folder.id, preferred_shuffle(), preferred_shuffle_seed());
            playback.current_track = queue_current_track(playback.queue);
            playback.position_seconds = 0;
            playback.playing = playback.current_track >= 0;
            navigate_to(ui, Page::NowPlaying);
        } else if (action == Action::Menu && ui.selected < index.folders.size()) {
            const Folder &folder = index.folders[ui.selected];
            open_library_action(folder.track_indices, folder.label);
        } else if (action == Action::Back || action == Action::Left) {
            navigate_back(ui, Page::LibraryRoot, 3);
        }
        break;
    case Page::FolderDetail:
        if (action == Action::Back || action == Action::Left || ui.context_index >= index.folders.size()) {
            const size_t folder_index = ui.context_index;
            navigate_back(ui, Page::Folder, std::min(folder_index, index.folders.empty() ? 0 : index.folders.size() - 1));
            if (ui.page == Page::Folder) {
                clamp_selection_window(ui, index.folders.size(), 4);
            }
            ui.selected_tracks.clear();
        } else {
            const Folder &folder = index.folders[ui.context_index];
            if (action == Action::Up) {
                move_selection_wrapped(ui, folder.track_indices.size() + 1, -1, 4);
            } else if (action == Action::Down) {
                move_selection_wrapped(ui, folder.track_indices.size() + 1, 1, 4);
            } else if (action == Action::Shuffle && ui.selected > 0 && ui.selected - 1 < folder.track_indices.size()) {
                toggle_track(ui.selected_tracks, folder.track_indices[ui.selected - 1]);
            } else if (action == Action::Menu || action == Action::Right) {
                std::vector<size_t> tracks = ui.selected_tracks.empty() ? folder.track_indices : ui.selected_tracks;
                open_library_action(std::move(tracks), folder.label);
            } else if (action == Action::Ok || action == Action::PlayPause) {
                if (ui.selected == 0) {
                    playback.queue = make_folder_queue(index, folder.id, preferred_shuffle(), preferred_shuffle_seed());
                    playback.current_track = queue_current_track(playback.queue);
                    playback.position_seconds = 0;
                    playback.playing = playback.current_track >= 0;
                    navigate_to(ui, Page::NowPlaying);
                } else if (ui.selected - 1 < folder.track_indices.size()) {
                    const size_t track_index = folder.track_indices[ui.selected - 1];
                    const std::string label = track_index < index.tracks.size() ? index.tracks[track_index].title : folder.label;
                    replace_queue_and_play(std::vector<size_t>{track_index}, label);
                }
            }
        }
        break;
    case Page::Playlists:
        if (action == Action::Up) {
            move_selection_wrapped(ui, index.playlists.size(), -1, 4);
        } else if (action == Action::Down) {
            move_selection_wrapped(ui, index.playlists.size(), 1, 4);
        } else if ((action == Action::Ok || action == Action::Right) && ui.selected < index.playlists.size()) {
            push_nav_entry(ui);
            ui.context_index = ui.selected;
            ui.page = Page::PlaylistDetail;
            ui.selected = 0;
            ui.scroll = 0;
            ui.selected_tracks.clear();
        } else if (action == Action::PlayPause && ui.selected < index.playlists.size()) {
            const Playlist &playlist = index.playlists[ui.selected];
            playback.queue = make_playlist_queue(index, playlist.id, preferred_shuffle(), preferred_shuffle_seed());
            playback.current_track = queue_current_track(playback.queue);
            playback.position_seconds = 0;
            playback.playing = playback.current_track >= 0;
            navigate_to(ui, Page::NowPlaying);
        } else if (action == Action::Menu && ui.selected < index.playlists.size()) {
            const Playlist &playlist = index.playlists[ui.selected];
            open_library_action(playlist.track_indices, playlist.title);
        } else if (action == Action::Back || action == Action::Left) {
            navigate_back(ui, Page::LibraryRoot, 4);
        }
        break;
    case Page::PlaylistDetail:
        if (action == Action::Back || action == Action::Left || ui.context_index >= index.playlists.size()) {
            const size_t playlist_index = ui.context_index;
            navigate_back(ui, Page::Playlists, std::min(playlist_index, index.playlists.empty() ? 0 : index.playlists.size() - 1));
            if (ui.page == Page::Playlists) {
                clamp_selection_window(ui, index.playlists.size(), 4);
            }
            ui.selected_tracks.clear();
        } else {
            const Playlist &playlist = index.playlists[ui.context_index];
            if (action == Action::Up) {
                move_selection_wrapped(ui, playlist.track_indices.size() + 1, -1, 4);
            } else if (action == Action::Down) {
                move_selection_wrapped(ui, playlist.track_indices.size() + 1, 1, 4);
            } else if (action == Action::Shuffle && ui.selected > 0 && ui.selected - 1 < playlist.track_indices.size()) {
                toggle_track(ui.selected_tracks, playlist.track_indices[ui.selected - 1]);
            } else if (action == Action::Menu || action == Action::Right) {
                std::vector<size_t> tracks = ui.selected_tracks.empty() ? playlist.track_indices : ui.selected_tracks;
                open_library_action(std::move(tracks), playlist.title);
            } else if (action == Action::Ok || action == Action::PlayPause) {
                if (ui.selected == 0) {
                    playback.queue = make_playlist_queue(index, playlist.id, preferred_shuffle(), preferred_shuffle_seed());
                    playback.current_track = queue_current_track(playback.queue);
                    playback.position_seconds = 0;
                    playback.playing = playback.current_track >= 0;
                    navigate_to(ui, Page::NowPlaying);
                } else if (ui.selected - 1 < playlist.track_indices.size()) {
                    const size_t track_index = playlist.track_indices[ui.selected - 1];
                    const std::string label = track_index < index.tracks.size() ? index.tracks[track_index].title : playlist.title;
                    replace_queue_and_play(std::vector<size_t>{track_index}, label);
                }
            }
        }
        break;
    case Page::LofiPresets:
        if (action == Action::Up) {
            move_selection_wrapped(ui, kLofiPresetChoiceCount, -1, 4);
        } else if (action == Action::Down) {
            move_selection_wrapped(ui, kLofiPresetChoiceCount, 1, 4);
        } else if (action == Action::Left) {
            playback.lofi = lofi_preset(LofiPreset::Off);
            ui.toast = "Lo-Fi Off";
        } else if (action == Action::Right || action == Action::Menu) {
            navigate_to(ui, Page::LofiEdit);
        } else if (action == Action::Ok) {
            playback.lofi = lofi_preset(kLofiPresetChoices[std::min(ui.selected, kLofiPresetChoiceCount - 1)]);
            ui.toast = std::string("Lo-Fi ") + to_string(playback.lofi.preset);
        } else if (action == Action::Back) {
            navigate_back(ui, ui.previous_page, 0);
        }
        break;
    case Page::LofiEdit:
        if (action == Action::Up) {
            move_selection_wrapped(ui, 6, -1, 5);
        } else if (action == Action::Down) {
            move_selection_wrapped(ui, 6, 1, 5);
        } else if (action == Action::Left || action == Action::Right) {
            int *values[] = {&playback.lofi.intensity, &playback.lofi.warmth, &playback.lofi.noise,
                             &playback.lofi.wobble, &playback.lofi.space, &playback.lofi.softness};
            int &value = *values[std::min<size_t>(ui.selected, 5)];
            value = std::max(0, std::min(100, value + (action == Action::Right ? 5 : -5)));
            playback.lofi.preset = LofiPreset::Custom;
        } else if (action == Action::Ok) {
            ui.toast = "Lo-Fi Saved";
        } else if (action == Action::Back) {
            navigate_back(ui, Page::LofiPresets, 0);
        }
        break;
    case Page::NowPlaying:
        if (action == Action::PlayPause || action == Action::Ok) {
            playback.playing = !playback.playing;
        } else if (action == Action::Next || action == Action::Right) {
            playback.current_track = queue_next(playback.queue, playback.repeat);
            playback.position_seconds = 0;
            playback.playing = playback.current_track >= 0;
        } else if (action == Action::Previous || action == Action::Left) {
            playback.current_track = queue_previous(playback.queue);
            playback.position_seconds = 0;
            playback.playing = playback.current_track >= 0;
        } else if (action == Action::SeekForward) {
            playback.position_seconds += 10;
            ui.toast = "Seek " + std::to_string(playback.position_seconds) + "s";
        } else if (action == Action::SeekBackward) {
            playback.position_seconds = std::max(0, playback.position_seconds - 10);
            ui.toast = "Seek " + std::to_string(playback.position_seconds) + "s";
        } else if (action == Action::Up) {
            adjust_user_volume(playback, ui, 5);
        } else if (action == Action::Down) {
            adjust_user_volume(playback, ui, -5);
        } else if (action == Action::Menu) {
            navigate_to(ui, Page::PlaybackMenu);
        } else if (action == Action::Back) {
            navigate_back(ui, Page::LibraryHome, 4);
        }
        break;
    case Page::PlaybackMenu:
        if (action == Action::Up) {
            ui.selected = ui.selected == 0 ? 5 : ui.selected - 1;
            ui.scroll = 0;
        } else if (action == Action::Down) {
            ui.selected = (ui.selected + 1) % 6;
            ui.scroll = 0;
        } else if (ui.selected == 0 && action == Action::Left) {
            adjust_user_volume(playback, ui, -5);
        } else if (ui.selected == 0 && (action == Action::Right || action == Action::Ok)) {
            adjust_user_volume(playback, ui, 5);
        } else if (ui.selected == 1 && (action == Action::Left || action == Action::Right || action == Action::Ok)) {
            playback.brightness_percent =
                step_brightness_percent(playback.brightness_percent, action == Action::Left ? -1 : 1);
            ui.toast = "Brightness " + std::to_string(playback.brightness_percent) + "%";
        } else if (ui.selected == 2 && (action == Action::Left || action == Action::Right || action == Action::Ok)) {
            if (action == Action::Left) {
                playback.repeat = playback.repeat == RepeatMode::Off     ? RepeatMode::All
                                  : playback.repeat == RepeatMode::All   ? RepeatMode::Album
                                  : playback.repeat == RepeatMode::Album ? RepeatMode::One
                                                                         : RepeatMode::Off;
            } else {
                playback.repeat = playback.repeat == RepeatMode::Off   ? RepeatMode::One
                                  : playback.repeat == RepeatMode::One ? RepeatMode::Album
                                  : playback.repeat == RepeatMode::Album ? RepeatMode::All
                                                                         : RepeatMode::Off;
            }
            ui.toast = std::string("Repeat ") + to_string(playback.repeat);
        } else if (ui.selected == 3 && (action == Action::Left || action == Action::Right || action == Action::Ok)) {
            const int previous_track = queue_current_track(playback.queue);
            playback.queue.shuffle = !playback.queue.shuffle;
            if (!playback.queue.track_indices.empty()) {
                if (playback.queue.source_type == "album") {
                    playback.queue = make_album_queue(index, playback.queue.source_id, playback.queue.shuffle,
                                                      playback.queue.shuffle_seed + 1);
                } else if (playback.queue.source_type == "artist") {
                    playback.queue = make_artist_queue(index, playback.queue.source_id, playback.queue.shuffle,
                                                       playback.queue.shuffle_seed + 1);
                } else if (playback.queue.source_type == "folder") {
                    playback.queue = make_folder_queue(index, playback.queue.source_id, playback.queue.shuffle,
                                                       playback.queue.shuffle_seed + 1);
                } else if (playback.queue.source_type == "playlist") {
                    playback.queue = make_playlist_queue(index, playback.queue.source_id, playback.queue.shuffle,
                                                        playback.queue.shuffle_seed + 1);
                } else {
                    playback.queue = make_all_tracks_queue(index, playback.queue.shuffle, playback.queue.shuffle_seed + 1);
                }
                if (previous_track >= 0) {
                    const auto it = std::find(playback.queue.track_indices.begin(),
                                              playback.queue.track_indices.end(),
                                              static_cast<size_t>(previous_track));
                    if (it != playback.queue.track_indices.end()) {
                        playback.queue.current_index = static_cast<size_t>(it - playback.queue.track_indices.begin());
                    }
                }
                playback.current_track = queue_current_track(playback.queue);
            }
            ui.toast = playback.queue.shuffle ? "Shuffle On" : "Shuffle Off";
        } else if (ui.selected == 4 && (action == Action::Right || action == Action::Ok)) {
            navigate_to(ui, Page::Queue, false);
            ui.selected = playback.queue.current_index;
            clamp_selection(ui, playback.queue.track_indices.size());
        } else if (ui.selected == 5 && (action == Action::Left || action == Action::Right || action == Action::Ok)) {
            playback.screen_off_seconds =
                step_screen_off_seconds(playback.screen_off_seconds, action == Action::Left ? -1 : 1);
            ui.toast = "Screen Off " + format_screen_off_seconds(playback.screen_off_seconds);
        } else if (action == Action::Menu || action == Action::Back || action == Action::Left) {
            navigate_back(ui, ui.previous_page, 0);
        }
        break;
    case Page::Queue:
        if (action == Action::Up) {
            move_selection(playback.queue.track_indices.size(), -1);
        } else if (action == Action::Down) {
            move_selection(playback.queue.track_indices.size(), 1);
        } else if (action == Action::Left && !playback.queue.track_indices.empty()) {
            ui.selected = ui.selected < 6 ? 0 : ui.selected - 6;
        } else if (action == Action::Right && !playback.queue.track_indices.empty()) {
            ui.selected = std::min(playback.queue.track_indices.size() - 1, ui.selected + 6);
        } else if ((action == Action::Ok || action == Action::PlayPause) &&
                   ui.selected < playback.queue.track_indices.size()) {
            playback.queue.current_index = ui.selected;
            playback.current_track = queue_current_track(playback.queue);
            playback.position_seconds = 0;
            playback.playing = playback.current_track >= 0;
            navigate_to(ui, Page::NowPlaying);
        } else if (action == Action::Back) {
            ui.selected = playback.queue.current_index;
            clamp_selection(ui, playback.queue.track_indices.size());
            navigate_back(ui, Page::NowPlaying, 0);
        }
        break;
    case Page::Scan:
        if (action == Action::Back || action == Action::Left) {
            navigate_back(ui, Page::LibraryHome, 0);
        } else if (action == Action::Ok || action == Action::Right) {
            reset_to_home(ui);
        }
        break;
    case Page::Empty:
        if (action == Action::Ok || action == Action::PlayPause || action == Action::Menu) {
            navigate_to(ui, Page::Scan);
        } else if (action == Action::Right) {
            navigate_to(ui, Page::Folder);
        } else if (action == Action::Back || action == Action::Left) {
            navigate_back(ui, Page::LibraryHome, 0);
        }
        break;
    default:
        if (action == Action::Back || action == Action::Left) {
            navigate_back(ui, Page::LibraryHome, 0);
        }
        break;
    }
}

std::vector<std::string> screen_to_lines(const ScreenModel &screen)
{
    std::vector<std::string> lines;
    lines.push_back("[" + screen.title + "] " + screen.status);
    if (!screen.subtitle.empty() || !screen.meta.empty()) {
        lines.push_back(screen.subtitle + " | " + screen.meta);
    }
    if (!screen.album_art_cache_path.empty()) {
        lines.push_back("cover:" + screen.album_art_cache_path);
    }
    if (screen.background_task_active) {
        lines.push_back("background:" + std::to_string(screen.background_task_frame % 4));
    }
    if (screen.volume_overlay_active) {
        lines.push_back("volume_overlay:" + std::to_string(screen.volume_overlay_percent));
    }
    for (const ScreenLine &row : screen.rows) {
        lines.push_back(row.right.empty() ? row.left : row.left + " | " + row.right);
    }
    lines.push_back(screen.soft_left + " / " + screen.soft_center + " / " + screen.soft_right);
    return lines;
}

uint32_t screen_hash(const ScreenModel &screen)
{
    uint32_t hash = 2166136261u;
    const std::vector<std::string> lines = screen_to_lines(screen);
    for (const std::string &line : lines) {
        for (unsigned char ch : line) {
            hash ^= static_cast<uint32_t>(ch);
            hash *= 16777619u;
        }
        hash ^= static_cast<uint32_t>('\n');
        hash *= 16777619u;
    }
    return hash;
}

std::string screen_auto_snapshot(const ScreenModel &screen, uint32_t revision)
{
    std::ostringstream out;
    out << "AUTO SNAP rev=" << revision
        << " hash=0x" << std::hex << std::setw(8) << std::setfill('0') << screen_hash(screen) << std::dec
        << " page=\"" << screen.title << "\""
        << " status=\"" << screen.status << "\""
        << " rows=" << screen.rows.size()
        << " cover=" << (screen.album_art_cache_path.empty() ? 0 : 1)
        << " bg=" << (screen.background_task_active ? 1 : 0)
        << " bg_frame=" << static_cast<unsigned>(screen.background_task_frame % 4)
        << " volume_overlay=" << (screen.volume_overlay_active ? screen.volume_overlay_percent : -1)
        << " soft=\"" << screen.soft_left << "|" << screen.soft_center << "|" << screen.soft_right << "\"";
    return out.str();
}

} // namespace lofi
