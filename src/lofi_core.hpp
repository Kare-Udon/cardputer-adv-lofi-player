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
    LibraryRoot,
    Songs,
    Albums,
    Artists,
    ArtistAlbums,
    AlbumDetail,
    LibraryAction,
    Folder,
    FolderDetail,
    Playlists,
    PlaylistDetail,
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
    Home,
    Scan,
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

struct Folder {
    std::string id;
    std::string label;
    std::string path;
    std::vector<size_t> track_indices;
};

struct Playlist {
    std::string id;
    std::string title;
    std::vector<size_t> track_indices;
};

struct LibraryIndex {
    std::vector<Track> tracks;
    std::vector<Album> albums;
    std::vector<Artist> artists;
    std::vector<Folder> folders;
    std::vector<Playlist> playlists;
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
    int volume = 50;
    int brightness_percent = 100;
    int screen_off_seconds = 60;
    RepeatMode repeat = RepeatMode::Off;
    bool playing = false;
    Queue queue;
    LofiProfile lofi;
};

struct PlaybackRestoreResult {
    bool settings_restored = false;
    bool queue_restored = false;
};

struct QueueSnapshotRestoreResult {
    bool restored = false;
    bool current_restored = false;
    size_t saved_count = 0;
    size_t restored_count = 0;
    size_t missing_count = 0;
};

struct UiNavEntry {
    Page page = Page::LibraryHome;
    size_t context_index = 0;
    size_t parent_index = 0;
    size_t selected = 0;
    size_t scroll = 0;
};

struct UiState {
    Page page = Page::LibraryHome;
    Page previous_page = Page::LibraryHome;
    Page action_return_page = Page::LibraryRoot;
    size_t context_index = 0;
    size_t parent_index = 0;
    size_t selected = 0;
    size_t scroll = 0;
    std::string toast;
    std::string action_label;
    std::vector<size_t> selected_tracks;
    std::vector<size_t> action_tracks;
    std::vector<UiNavEntry> back_stack;
    bool volume_boost_warning_armed = false;
};

struct ScreenLine {
    std::string left;
    std::string right;
};

struct ScreenModel {
    std::string title;
    std::string subtitle;
    std::string meta;
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
Queue make_folder_queue(const LibraryIndex &index, const std::string &folder_id, bool shuffle, uint32_t seed);
Queue make_playlist_queue(const LibraryIndex &index, const std::string &playlist_id, bool shuffle, uint32_t seed);
int queue_current_track(const Queue &queue);
int queue_next(Queue &queue, RepeatMode repeat);
int queue_previous(Queue &queue);

LofiProfile lofi_preset(LofiPreset preset);
const char *to_string(LofiPreset preset);
const char *to_string(RepeatMode repeat);
const char *to_string(Page page);
LofiPreset preset_from_string(const std::string &value);
RepeatMode repeat_from_string(const std::string &value);
int audio_volume_from_user_percent(int volume);

std::string serialize_playback_state(const PlaybackState &state);
bool parse_playback_state(const std::string &text, PlaybackState &out);
bool restore_playback_queue(const LibraryIndex &index, PlaybackState &state, bool resume_playing);
PlaybackRestoreResult restore_saved_playback_state(const LibraryIndex &index,
                                                   const PlaybackState &saved,
                                                   PlaybackState &state,
                                                   bool resume_playing);
std::string serialize_queue_snapshot(const LibraryIndex &index, const PlaybackState &state);
QueueSnapshotRestoreResult restore_queue_snapshot(const LibraryIndex &index,
                                                  const std::string &text,
                                                  PlaybackState &state,
                                                  bool resume_playing);

ScreenModel render_screen(const LibraryIndex &index, const PlaybackState &playback, const UiState &ui);
void apply_action(const LibraryIndex &index, PlaybackState &playback, UiState &ui, Action action);

std::vector<std::string> screen_to_lines(const ScreenModel &screen);
uint32_t screen_hash(const ScreenModel &screen);
std::string screen_auto_snapshot(const ScreenModel &screen, uint32_t revision);

} // namespace lofi
