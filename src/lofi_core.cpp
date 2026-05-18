#include "lofi_core.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iomanip>
#include <map>
#include <sstream>

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

std::vector<size_t> path_sorted_track_indices(const LibraryIndex &index)
{
    std::vector<size_t> items;
    for (size_t i = 0; i < index.tracks.size(); ++i) {
        items.push_back(i);
    }
    std::sort(items.begin(), items.end(), [&](size_t lhs, size_t rhs) {
        return lower_copy(index.tracks[lhs].path) < lower_copy(index.tracks[rhs].path);
    });
    return items;
}

std::string parent_folder_label(const std::string &path)
{
    const std::vector<std::string> parts = relative_music_parts(path);
    if (parts.size() >= 2) {
        return parts[parts.size() - 2];
    }
    return "Music";
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
    Queue queue;
    queue.source_type = "folder";
    queue.source_id = "/Music";
    queue.shuffle = shuffle;
    queue.shuffle_seed = seed;
    queue.track_indices = path_sorted_track_indices(index);
    if (queue.track_indices.empty()) {
        queue.current_index = 0;
        return queue;
    }
    if (selected_order_index >= queue.track_indices.size()) {
        selected_order_index = queue.track_indices.size() - 1;
    }
    const size_t selected_track = queue.track_indices[selected_order_index];
    if (shuffle) {
        shuffle_indices(queue.track_indices, seed);
        const auto selected_it = std::find(queue.track_indices.begin(), queue.track_indices.end(), selected_track);
        queue.current_index = selected_it == queue.track_indices.end()
                                  ? 0
                                  : static_cast<size_t>(selected_it - queue.track_indices.begin());
    } else {
        queue.current_index = selected_order_index;
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
    case Page::Albums:
        return "Albums";
    case Page::Artists:
        return "Artists";
    case Page::ArtistAlbums:
        return "Artist";
    case Page::AlbumDetail:
        return "Album";
    case Page::Folder:
        return "Folder";
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

std::string serialize_playback_state(const PlaybackState &state)
{
    std::ostringstream out;
    out << "current_track=" << state.current_track << "\n";
    out << "position_seconds=" << state.position_seconds << "\n";
    out << "volume=" << state.volume << "\n";
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
            out.volume = std::max(0, std::min(100, parse_int_or(value, out.volume)));
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
        restored = make_folder_queue(index, 0, state.queue.shuffle, state.queue.shuffle_seed);
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

ScreenModel render_screen(const LibraryIndex &index, const PlaybackState &playback, const UiState &ui)
{
    ScreenModel screen;
    screen.title = to_string(ui.page);
    screen.status = std::string("SD ") + (index.tracks.empty() ? "EMPTY" : "OK") + " | Vol " +
                    std::to_string(playback.volume) + " | LF " + to_string(playback.lofi.preset);
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
            screen.rows.push_back({track.title, playback.playing ? ">" : "||"});
            screen.rows.push_back({track.artist + " / " + track.album, ""});
            screen.rows.push_back({"Pos " + std::to_string(playback.position_seconds) + "s", std::string("Repeat ") + to_string(playback.repeat)});
            screen.rows.push_back({"Queue " + std::to_string(playback.queue.current_index + 1) + "/" + std::to_string(playback.queue.track_indices.size()), playback.queue.shuffle ? "Shuffle" : ""});
        } else {
            screen.rows.push_back({"No track selected", ""});
            screen.rows.push_back({"Open Library to play", ""});
        }
    } else if (ui.page == Page::LibraryHome) {
        screen.soft_center = ui.selected == 3 ? "Shuffle" : (ui.selected == 6 ? "Scan" : "Open");
        screen.soft_right = "Scan";
        static const char *items[] = {"Now Playing", "Albums", "Artists", "Shuffle All", "Folder", "Lo-Fi", "Scan"};
        for (size_t i = ui.scroll; i < 7 && screen.rows.size() < 4; ++i) {
            std::string right;
            if (i == 0 && playback.current_track >= 0) {
                right = "NOW";
            } else if (i == 1) {
                right = std::to_string(index.albums.size());
            } else if (i == 2) {
                right = std::to_string(index.artists.size());
            } else if (i == 3) {
                right = "ALL";
            } else if (i == 4) {
                right = "SD";
            } else if (i == 5) {
                right = first_n(to_string(playback.lofi.preset), 6);
            }
            screen.rows.push_back({std::string(i == ui.selected ? "> " : "  ") + items[i], right});
        }
    } else if (ui.page == Page::Albums || ui.page == Page::AlbumDetail) {
        if (index.albums.empty()) {
            screen.rows.push_back({"No albums", "Scan"});
        } else if (ui.page == Page::Albums) {
            for (size_t i = ui.scroll; i < index.albums.size() && screen.rows.size() < 4; ++i) {
                const Album &album = index.albums[i];
                screen.rows.push_back({std::string(i == ui.selected ? "> " : "  ") + first_n(album.title, 18),
                                       std::to_string(album.track_indices.size())});
            }
        } else {
            const Album &album = index.albums[std::min(ui.context_index, index.albums.size() - 1)];
            screen.soft_left = "Back";
            screen.soft_center = "Play";
            screen.soft_right = "Album";
            screen.rows.push_back({first_n(album.title, 22), std::to_string(album.track_indices.size())});
            for (size_t i = ui.scroll; i < album.track_indices.size() && screen.rows.size() < 4; ++i) {
                const Track &track = index.tracks[album.track_indices[i]];
                screen.rows.push_back({std::string(i == ui.selected ? "> " : "  ") + std::to_string(i + 1) + ". " +
                                           first_n(track.title, 16),
                                       ""});
            }
        }
    } else if (ui.page == Page::Artists) {
        screen.soft_center = "Open";
        screen.soft_right = "Play";
        if (index.artists.empty()) {
            screen.rows.push_back({"No artists", "Scan"});
        } else {
            for (size_t i = ui.scroll; i < index.artists.size() && screen.rows.size() < 4; ++i) {
                const Artist &artist = index.artists[i];
                screen.rows.push_back({std::string(i == ui.selected ? "> " : "  ") + first_n(artist.name, 19),
                                       std::to_string(artist.album_indices.size()) + " alb"});
            }
        }
    } else if (ui.page == Page::ArtistAlbums) {
        screen.soft_center = "Open";
        screen.soft_right = "Play";
        if (ui.context_index >= index.artists.size()) {
            screen.rows.push_back({"No artist", "Back"});
        } else {
            const Artist &artist = index.artists[ui.context_index];
            screen.rows.push_back({first_n(artist.name, 22), std::to_string(artist.album_indices.size())});
            for (size_t i = ui.scroll; i < artist.album_indices.size() && screen.rows.size() < 4; ++i) {
                const size_t album_index = artist.album_indices[i];
                if (album_index >= index.albums.size()) {
                    continue;
                }
                const Album &album = index.albums[album_index];
                screen.rows.push_back({std::string(i == ui.selected ? "> " : "  ") + first_n(album.title, 18),
                                       std::to_string(album.track_indices.size())});
            }
        }
    } else if (ui.page == Page::Folder) {
        screen.soft_left = "Back";
        screen.soft_center = "Play";
        screen.soft_right = "Scan";
        const std::vector<size_t> order = path_sorted_track_indices(index);
        if (order.empty()) {
            screen.rows.push_back({"No files", "Scan"});
            screen.rows.push_back({"Put music in /Music", ""});
        } else {
            for (size_t i = ui.scroll; i < order.size() && screen.rows.size() < 4; ++i) {
                const Track &track = index.tracks[order[i]];
                screen.rows.push_back({std::string(i == ui.selected ? "> " : "  ") + first_n(track.title, 18),
                                       first_n(parent_folder_label(track.path), 8)});
            }
        }
    } else if (ui.page == Page::LofiPresets) {
        screen.soft_left = "Off";
        screen.soft_center = "Edit";
        screen.soft_right = "Apply";
        const size_t max_rows = 3;
        const size_t max_start = kLofiPresetChoiceCount > max_rows ? kLofiPresetChoiceCount - max_rows : 0;
        const size_t start = std::min(ui.selected, max_start);
        for (size_t i = start; i < kLofiPresetChoiceCount && screen.rows.size() < max_rows; ++i) {
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
        for (size_t i = ui.scroll; i < 6 && screen.rows.size() < 4; ++i) {
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
        screen.rows.push_back({std::string(ui.selected == 1 ? "> " : "  ") + "Repeat", to_string(playback.repeat)});
        screen.rows.push_back({std::string(ui.selected == 2 ? "> " : "  ") + "Shuffle", playback.queue.shuffle ? "On" : "Off"});
        screen.rows.push_back({std::string(ui.selected == 3 ? "> " : "  ") + "Queue", queue_pos});
    } else if (ui.page == Page::Queue) {
        screen.soft_left = "Back";
        screen.soft_center = "Play";
        screen.soft_right = "Now";
        if (playback.queue.track_indices.empty()) {
            screen.rows.push_back({"Queue is empty", ""});
        } else {
            size_t start = std::min(ui.selected, playback.queue.track_indices.size() - 1);
            for (size_t i = start; i < playback.queue.track_indices.size() && screen.rows.size() < 3; ++i) {
                const size_t track_index = playback.queue.track_indices[i];
                if (track_index >= index.tracks.size()) {
                    continue;
                }
                const Track &track = index.tracks[track_index];
                const bool is_current = i == playback.queue.current_index;
                const std::string marker = is_current ? "*" : " ";
                screen.rows.push_back({std::string(i == ui.selected ? ">" : " ") + marker + " " + first_n(track.title, 18),
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

    ui.toast.clear();
    if (action == Action::Lofi) {
        ui.previous_page = ui.page;
        ui.page = Page::LofiPresets;
        ui.selected = 0;
        ui.scroll = 0;
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
                playback.queue = make_folder_queue(index, playback.queue.current_index, playback.queue.shuffle,
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
        if (action == Action::Up) {
            move_selection(7, -1);
        } else if (action == Action::Down) {
            move_selection(7, 1);
        } else if (action == Action::Menu) {
            ui.page = Page::Scan;
        } else if (action == Action::Ok || action == Action::Right) {
            if (ui.selected == 0) {
                ui.page = Page::NowPlaying;
            } else if (ui.selected == 1) {
                ui.page = Page::Albums;
                ui.selected = 0;
                ui.scroll = 0;
            } else if (ui.selected == 2) {
                ui.page = Page::Artists;
                ui.selected = 0;
                ui.scroll = 0;
            } else if (ui.selected == 3) {
                playback.queue = make_all_tracks_queue(index, true, 42);
                playback.current_track = queue_current_track(playback.queue);
                playback.playing = playback.current_track >= 0;
                ui.page = Page::NowPlaying;
            } else if (ui.selected == 4) {
                ui.page = Page::Folder;
                ui.selected = 0;
                ui.scroll = 0;
            } else if (ui.selected == 5) {
                ui.page = Page::LofiPresets;
                ui.selected = 0;
                ui.scroll = 0;
            } else {
                ui.page = Page::Scan;
            }
        }
        break;
    case Page::Albums:
        if (action == Action::Up) {
            move_selection(index.albums.size(), -1);
        } else if (action == Action::Down) {
            move_selection(index.albums.size(), 1);
        } else if ((action == Action::Ok || action == Action::PlayPause) && ui.selected < index.albums.size()) {
            playback.queue = make_album_queue(index, index.albums[ui.selected].id, false, 0);
            playback.current_track = queue_current_track(playback.queue);
            playback.playing = playback.current_track >= 0;
            ui.page = Page::NowPlaying;
        } else if (action == Action::Right && ui.selected < index.albums.size()) {
            ui.previous_page = Page::Albums;
            ui.context_index = ui.selected;
            ui.parent_index = 0;
            ui.page = Page::AlbumDetail;
            ui.selected = 0;
            ui.scroll = 0;
        } else if (action == Action::Back || action == Action::Left) {
            ui.page = Page::LibraryHome;
            ui.selected = 1;
            ui.scroll = 0;
        }
        break;
    case Page::AlbumDetail:
        if (action == Action::Back || action == Action::Left || ui.context_index >= index.albums.size()) {
            const size_t album_index = ui.context_index;
            if (ui.previous_page == Page::ArtistAlbums && ui.parent_index < index.artists.size()) {
                const Artist &artist = index.artists[ui.parent_index];
                ui.page = Page::ArtistAlbums;
                ui.context_index = ui.parent_index;
                ui.selected = 0;
                for (size_t i = 0; i < artist.album_indices.size(); ++i) {
                    if (artist.album_indices[i] == album_index) {
                        ui.selected = i;
                        break;
                    }
                }
                clamp_selection(ui, artist.album_indices.size());
            } else {
                ui.page = Page::Albums;
                ui.selected = std::min(album_index, index.albums.empty() ? 0 : index.albums.size() - 1);
                clamp_selection(ui, index.albums.size());
            }
        } else {
            const Album &album = index.albums[ui.context_index];
            if (action == Action::Up) {
                move_selection(album.track_indices.size(), -1);
            } else if (action == Action::Down) {
                move_selection(album.track_indices.size(), 1);
            } else if ((action == Action::Ok || action == Action::PlayPause) &&
                       ui.selected < album.track_indices.size()) {
                playback.queue = make_album_queue(index, album.id, false, 0);
                if (!playback.queue.track_indices.empty()) {
                    playback.queue.current_index = std::min(ui.selected, playback.queue.track_indices.size() - 1);
                }
                playback.current_track = queue_current_track(playback.queue);
                playback.position_seconds = 0;
                playback.playing = playback.current_track >= 0;
                ui.page = Page::NowPlaying;
            } else if (action == Action::Menu || action == Action::Right) {
                playback.queue = make_album_queue(index, album.id, false, 0);
                playback.current_track = queue_current_track(playback.queue);
                playback.position_seconds = 0;
                playback.playing = playback.current_track >= 0;
                ui.page = Page::NowPlaying;
            }
        }
        break;
    case Page::Artists:
        if (action == Action::Up) {
            move_selection(index.artists.size(), -1);
        } else if (action == Action::Down) {
            move_selection(index.artists.size(), 1);
        } else if ((action == Action::Ok || action == Action::Right) && ui.selected < index.artists.size()) {
            ui.page = Page::ArtistAlbums;
            ui.context_index = ui.selected;
            ui.selected = 0;
            ui.scroll = 0;
        } else if ((action == Action::PlayPause || action == Action::Menu) && ui.selected < index.artists.size()) {
            playback.queue = make_artist_queue(index, index.artists[ui.selected].name, false, 0);
            playback.current_track = queue_current_track(playback.queue);
            playback.position_seconds = 0;
            playback.playing = playback.current_track >= 0;
            ui.page = Page::NowPlaying;
        } else if (action == Action::Back || action == Action::Left) {
            ui.page = Page::LibraryHome;
            ui.selected = 2;
            ui.scroll = 0;
        }
        break;
    case Page::ArtistAlbums:
        if (ui.context_index >= index.artists.size()) {
            ui.page = Page::Artists;
            ui.selected = 0;
            ui.scroll = 0;
        } else {
            const Artist &artist = index.artists[ui.context_index];
            if (action == Action::Up) {
                move_selection(artist.album_indices.size(), -1);
            } else if (action == Action::Down) {
                move_selection(artist.album_indices.size(), 1);
            } else if ((action == Action::Ok || action == Action::Right) && ui.selected < artist.album_indices.size()) {
                const size_t album_index = artist.album_indices[ui.selected];
                if (album_index < index.albums.size()) {
                    ui.previous_page = Page::ArtistAlbums;
                    ui.parent_index = ui.context_index;
                    ui.context_index = album_index;
                    ui.page = Page::AlbumDetail;
                    ui.selected = 0;
                    ui.scroll = 0;
                }
            } else if ((action == Action::PlayPause || action == Action::Menu) && ui.context_index < index.artists.size()) {
                playback.queue = make_artist_queue(index, artist.name, false, 0);
                playback.current_track = queue_current_track(playback.queue);
                playback.position_seconds = 0;
                playback.playing = playback.current_track >= 0;
                ui.page = Page::NowPlaying;
            } else if (action == Action::Back || action == Action::Left) {
                ui.page = Page::Artists;
                ui.selected = ui.context_index;
                clamp_selection(ui, index.artists.size());
            }
        }
        break;
    case Page::Folder:
        if (action == Action::Up) {
            move_selection(index.tracks.size(), -1);
        } else if (action == Action::Down) {
            move_selection(index.tracks.size(), 1);
        } else if ((action == Action::Ok || action == Action::PlayPause) && ui.selected < index.tracks.size()) {
            playback.queue = make_folder_queue(index, ui.selected, false, 0);
            playback.current_track = queue_current_track(playback.queue);
            playback.playing = playback.current_track >= 0;
            ui.page = Page::NowPlaying;
        } else if (action == Action::Back || action == Action::Left) {
            ui.page = Page::LibraryHome;
            ui.selected = 4;
            ui.scroll = 1;
        }
        break;
    case Page::LofiPresets:
        if (action == Action::Up) {
            move_selection(kLofiPresetChoiceCount, -1);
        } else if (action == Action::Down) {
            move_selection(kLofiPresetChoiceCount, 1);
        } else if (action == Action::Left) {
            playback.lofi = lofi_preset(LofiPreset::Off);
            ui.toast = "Lo-Fi Off";
        } else if (action == Action::Right || action == Action::Menu) {
            ui.page = Page::LofiEdit;
            ui.selected = 0;
            ui.scroll = 0;
        } else if (action == Action::Ok) {
            playback.lofi = lofi_preset(kLofiPresetChoices[std::min(ui.selected, kLofiPresetChoiceCount - 1)]);
            ui.toast = std::string("Lo-Fi ") + to_string(playback.lofi.preset);
        } else if (action == Action::Back) {
            ui.page = ui.previous_page;
        }
        break;
    case Page::LofiEdit:
        if (action == Action::Up) {
            move_selection(6, -1);
        } else if (action == Action::Down) {
            move_selection(6, 1);
        } else if (action == Action::Left || action == Action::Right) {
            int *values[] = {&playback.lofi.intensity, &playback.lofi.warmth, &playback.lofi.noise,
                             &playback.lofi.wobble, &playback.lofi.space, &playback.lofi.softness};
            int &value = *values[std::min<size_t>(ui.selected, 5)];
            value = std::max(0, std::min(100, value + (action == Action::Right ? 5 : -5)));
            playback.lofi.preset = LofiPreset::Custom;
        } else if (action == Action::Ok) {
            ui.toast = "Lo-Fi Saved";
        } else if (action == Action::Back) {
            ui.page = Page::LofiPresets;
            ui.selected = 0;
            ui.scroll = 0;
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
            playback.volume = std::min(100, playback.volume + 5);
            ui.toast = "Volume " + std::to_string(playback.volume);
        } else if (action == Action::Down) {
            playback.volume = std::max(0, playback.volume - 5);
            ui.toast = "Volume " + std::to_string(playback.volume);
        } else if (action == Action::Menu) {
            ui.page = Page::PlaybackMenu;
            ui.selected = 0;
            ui.scroll = 0;
        } else if (action == Action::Back) {
            ui.page = Page::LibraryHome;
        }
        break;
    case Page::PlaybackMenu:
        if (action == Action::Up) {
            move_selection(4, -1);
        } else if (action == Action::Down) {
            move_selection(4, 1);
        } else if (ui.selected == 0 && action == Action::Left) {
            playback.volume = std::max(0, playback.volume - 5);
            ui.toast = "Volume " + std::to_string(playback.volume);
        } else if (ui.selected == 0 && (action == Action::Right || action == Action::Ok)) {
            playback.volume = std::min(100, playback.volume + 5);
            ui.toast = "Volume " + std::to_string(playback.volume);
        } else if (ui.selected == 1 && (action == Action::Left || action == Action::Right || action == Action::Ok)) {
            playback.repeat = playback.repeat == RepeatMode::Off   ? RepeatMode::One
                              : playback.repeat == RepeatMode::One ? RepeatMode::Album
                              : playback.repeat == RepeatMode::Album ? RepeatMode::All
                                                                     : RepeatMode::Off;
            ui.toast = std::string("Repeat ") + to_string(playback.repeat);
        } else if (ui.selected == 2 && (action == Action::Left || action == Action::Right || action == Action::Ok)) {
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
                    playback.queue = make_folder_queue(index, playback.queue.current_index, playback.queue.shuffle,
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
        } else if (ui.selected == 3 && (action == Action::Right || action == Action::Ok)) {
            ui.page = Page::Queue;
            ui.selected = playback.queue.current_index;
            clamp_selection(ui, playback.queue.track_indices.size());
        } else if (action == Action::Menu || action == Action::Back || action == Action::Left) {
            ui.page = Page::NowPlaying;
            ui.selected = 0;
            ui.scroll = 0;
        }
        break;
    case Page::Queue:
        if (action == Action::Up) {
            move_selection(playback.queue.track_indices.size(), -1);
        } else if (action == Action::Down) {
            move_selection(playback.queue.track_indices.size(), 1);
        } else if ((action == Action::Ok || action == Action::PlayPause || action == Action::Right) &&
                   ui.selected < playback.queue.track_indices.size()) {
            playback.queue.current_index = ui.selected;
            playback.current_track = queue_current_track(playback.queue);
            playback.position_seconds = 0;
            playback.playing = playback.current_track >= 0;
            ui.page = Page::NowPlaying;
        } else if (action == Action::Back || action == Action::Left) {
            ui.selected = playback.queue.current_index;
            clamp_selection(ui, playback.queue.track_indices.size());
            ui.page = Page::NowPlaying;
        }
        break;
    case Page::Scan:
        if (action == Action::Back || action == Action::Left || action == Action::Ok || action == Action::Right) {
            ui.page = Page::LibraryHome;
            ui.selected = 6;
            ui.scroll = 3;
        }
        break;
    case Page::Empty:
        if (action == Action::Ok || action == Action::PlayPause || action == Action::Menu) {
            ui.page = Page::Scan;
            ui.selected = 0;
            ui.scroll = 0;
        } else if (action == Action::Right) {
            ui.page = Page::Folder;
            ui.selected = 0;
            ui.scroll = 0;
        } else if (action == Action::Back || action == Action::Left) {
            ui.page = Page::LibraryHome;
            ui.selected = 0;
            ui.scroll = 0;
        }
        break;
    default:
        if (action == Action::Back || action == Action::Left) {
            ui.page = Page::LibraryHome;
        }
        break;
    }
}

std::vector<std::string> screen_to_lines(const ScreenModel &screen)
{
    std::vector<std::string> lines;
    lines.push_back("[" + screen.title + "] " + screen.status);
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
        << " soft=\"" << screen.soft_left << "|" << screen.soft_center << "|" << screen.soft_right << "\"";
    return out.str();
}

} // namespace lofi
