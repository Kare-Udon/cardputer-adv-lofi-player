#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace lofi {

enum class RepeatMode {
    Off,
    One,
    Album,
    All,
};

enum class LofiPreset {
    Off,
    WarmTape,
    VinylCafe,
    RainyWindow,
    TinyRadio,
    LateNight,
    Custom,
};

enum class Page {
    NowPlaying,
    LibraryHome,
    Albums,
    Artists,
    ArtistAlbums,
    AlbumDetail,
    Folder,
    LofiPresets,
    LofiEdit,
    PlaybackMenu,
    Queue,
    Scan,
    Empty,
};

enum class Action {
    None,
    Up,
    Down,
    Left,
    Right,
    Ok,
    Back,
    PlayPause,
    Next,
    Previous,
    SeekForward,
    SeekBackward,
    Shuffle,
    Repeat,
    Lofi,
    Menu,
};

struct Track {
    std::string id;
    std::string path;
    std::string title;
    std::string artist;
    std::string album;
    std::string album_artist;
    int track_no = 0;
    int duration_seconds = 0;
    std::string format;
    uint64_t mtime = 0;
};

struct Album {
    std::string id;
    std::string title;
    std::string album_artist;
    std::string source_folder;
    std::vector<size_t> track_indices;
};

struct Artist {
    std::string name;
    std::vector<size_t> album_indices;
    size_t loose_track_count = 0;
};

struct LibraryIndex {
    std::vector<Track> tracks;
    std::vector<Album> albums;
    std::vector<Artist> artists;
    std::vector<std::string> warnings;
};

struct Queue {
    std::string source_type = "none";
    std::string source_id;
    std::vector<size_t> track_indices;
    size_t current_index = 0;
    bool shuffle = false;
    uint32_t shuffle_seed = 0;
};

struct LofiProfile {
    LofiPreset preset = LofiPreset::Off;
    int intensity = 0;
    int warmth = 0;
    int noise = 0;
    int wobble = 0;
    int space = 0;
    int softness = 0;
};

struct PlaybackState {
    int current_track = -1;
    int position_seconds = 0;
    int volume = 70;
    RepeatMode repeat = RepeatMode::Off;
    bool playing = false;
    Queue queue;
    LofiProfile lofi;
};

struct UiState {
    Page page = Page::LibraryHome;
    Page previous_page = Page::LibraryHome;
    size_t context_index = 0;
    size_t parent_index = 0;
    size_t selected = 0;
    size_t scroll = 0;
    std::string toast;
};

struct ScreenLine {
    std::string left;
    std::string right;
};

struct ScreenModel {
    std::string title;
    std::vector<ScreenLine> rows;
    std::string soft_left;
    std::string soft_center;
    std::string soft_right;
    std::string status;
    int position_seconds = 0;
    int duration_seconds = 0;
};

bool is_audio_path(const std::string &path);
Track infer_track_from_path(const std::string &path, uint64_t mtime = 0);
LibraryIndex build_library_index(const std::vector<std::string> &paths);

Queue make_album_queue(const LibraryIndex &index, const std::string &album_id, bool shuffle, uint32_t seed);
Queue make_artist_queue(const LibraryIndex &index, const std::string &artist_name, bool shuffle, uint32_t seed);
Queue make_all_tracks_queue(const LibraryIndex &index, bool shuffle, uint32_t seed);
Queue make_folder_queue(const LibraryIndex &index, size_t selected_order_index, bool shuffle, uint32_t seed);
int queue_current_track(const Queue &queue);
int queue_next(Queue &queue, RepeatMode repeat);
int queue_previous(Queue &queue);

LofiProfile lofi_preset(LofiPreset preset);
const char *to_string(LofiPreset preset);
const char *to_string(RepeatMode repeat);
const char *to_string(Page page);
LofiPreset preset_from_string(const std::string &value);
RepeatMode repeat_from_string(const std::string &value);

std::string serialize_playback_state(const PlaybackState &state);
bool parse_playback_state(const std::string &text, PlaybackState &out);
bool restore_playback_queue(const LibraryIndex &index, PlaybackState &state, bool resume_playing);

ScreenModel render_screen(const LibraryIndex &index, const PlaybackState &playback, const UiState &ui);
void apply_action(const LibraryIndex &index, PlaybackState &playback, UiState &ui, Action action);

std::vector<std::string> screen_to_lines(const ScreenModel &screen);
uint32_t screen_hash(const ScreenModel &screen);
std::string screen_auto_snapshot(const ScreenModel &screen, uint32_t revision);

} // namespace lofi
