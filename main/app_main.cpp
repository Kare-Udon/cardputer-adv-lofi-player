#include "lofi_core.hpp"
#include "lofi_aac_cache.hpp"
#include "lofi_audio.hpp"
#include "lofi_board.hpp"
#include "lofi_selftest_mp3.hpp"
#include "lofi_storage.hpp"
#include "lofi_wav.hpp"

#include "board_pins.h"
#include "driver/sdspi_host.h"
#include "driver/spi_master.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp32s3/rom/tjpgd.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdmmc_cmd.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <cstdint>
#include <fcntl.h>
#include <new>
#include <strings.h>
#include <sys/stat.h>
#include <utility>
#include <unistd.h>

namespace {

const char *TAG = "lofi";

const std::vector<std::string> kBootSamplePaths = {
    "/Music/Nujabes/Modal Soul/01 - Feather.mp3",
    "/Music/Nujabes/Modal Soul/02 - Ordinary Joe.flac",
    "/Music/Various Artists/Late Night/01 - Uyama Hiroto - Waltz.wav",
    "/Music/Inbox/tape memo.m4a",
};

constexpr const char *kStateDir = "/sdcard/Music/LOFI";
constexpr const char *kStateFile = "/sdcard/Music/LOFI/STATE.TXT";
constexpr const char *kQueueFile = "/sdcard/Music/LOFI/QUEUE.TXT";
constexpr const char *kIndexFile = "/sdcard/Music/LOFI/INDEX.TXT";
constexpr const char *kSelfTestWavFile = "/sdcard/Music/LOFI/SELFTEST.WAV";
constexpr const char *kSelfTestMp3File = "/sdcard/Music/LOFI/SELFTEST.MP3";
constexpr const char *kCoverCacheDir = "/sdcard/Music/LOFI/COVERS";
constexpr const char *kAudioCacheDir = "/sdcard/Music/LOFI/AUDIO_CACHE";
constexpr int kCoverThumbWidth = 72;
constexpr int kCoverThumbHeight = 72;
constexpr TickType_t kLibrarySyncIdleDelay = pdMS_TO_TICKS(60000);
constexpr TickType_t kAudioCacheIdleDelay = pdMS_TO_TICKS(3000);
constexpr TickType_t kAudioCacheAttemptInterval = pdMS_TO_TICKS(1500);
constexpr uint64_t kSlowStartAudioOffsetThreshold = 512 * 1024;
constexpr size_t kAudioCacheMinLargestHeapBlock = 24 * 1024;

volatile bool g_audio_cache_cancel_requested = false;

bool audio_cache_cancel_requested(void *)
{
    return g_audio_cache_cancel_requested;
}

void set_background_task_indicator(lofi::ScreenModel &screen, bool active, TickType_t now_ticks)
{
    screen.background_task_active = active;
    screen.background_task_frame = active ? static_cast<uint8_t>((now_ticks / pdMS_TO_TICKS(180)) % 4) : 0;
}

void set_volume_overlay(lofi::ScreenModel &screen, bool active, int volume_percent)
{
    screen.volume_overlay_active = active;
    screen.volume_overlay_percent = std::max(0, std::min(100, volume_percent));
}

void set_mode_overlay(lofi::ScreenModel &screen,
                      bool active,
                      const std::string &kind,
                      const std::string &title,
                      const std::string &value)
{
    screen.mode_overlay_active = active;
    screen.mode_overlay_kind = active ? kind : "";
    screen.mode_overlay_title = active ? title : "";
    screen.mode_overlay_value = active ? value : "";
}

struct MediaFormatCounts {
    size_t mp3 = 0;
    size_t wav = 0;
    size_t m4a_aac = 0;
    size_t other = 0;
};

struct LibraryScanResult {
    lofi::LibraryIndex library;
    bool using_samples = false;
    bool from_cache = false;
};

struct LibraryScanTaskContext {
    bool sd_mounted = false;
    QueueHandle_t result_queue = nullptr;
};

struct AudioCacheResult {
    bool attempted = false;
    bool built = false;
};

struct AudioCacheTaskContext {
    lofi::Track track;
    QueueHandle_t result_queue = nullptr;
};

void log_library_summary(const lofi::LibraryIndex &library);
void populate_track_album_art_sources(lofi::LibraryIndex &library);

uint32_t read_be32(FILE *file, bool &ok)
{
    uint8_t bytes[4] = {};
    ok = std::fread(bytes, 1, sizeof(bytes), file) == sizeof(bytes);
    if (!ok) {
        return 0;
    }
    return (static_cast<uint32_t>(bytes[0]) << 24) |
           (static_cast<uint32_t>(bytes[1]) << 16) |
           (static_cast<uint32_t>(bytes[2]) << 8) |
           static_cast<uint32_t>(bytes[3]);
}

uint64_t read_be64(FILE *file, bool &ok)
{
    bool hi_ok = false;
    const uint32_t hi = read_be32(file, hi_ok);
    bool lo_ok = false;
    const uint32_t lo = read_be32(file, lo_ok);
    ok = hi_ok && lo_ok;
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

uint32_t read_syncsafe32_bytes(const uint8_t *bytes)
{
    return (static_cast<uint32_t>(bytes[0] & 0x7f) << 21) |
           (static_cast<uint32_t>(bytes[1] & 0x7f) << 14) |
           (static_cast<uint32_t>(bytes[2] & 0x7f) << 7) |
           static_cast<uint32_t>(bytes[3] & 0x7f);
}

uint32_t read_be32_bytes(const uint8_t *bytes)
{
    return (static_cast<uint32_t>(bytes[0]) << 24) |
           (static_cast<uint32_t>(bytes[1]) << 16) |
           (static_cast<uint32_t>(bytes[2]) << 8) |
           static_cast<uint32_t>(bytes[3]);
}

bool is_mp4_container_atom(uint32_t type)
{
    return type == 0x6d6f6f76 || // moov
           type == 0x7472616b || // trak
           type == 0x6d646961;   // mdia
}

int parse_m4a_mdhd_duration(FILE *file)
{
    const long payload_start = std::ftell(file);
    if (payload_start < 0) {
        return 0;
    }
    uint8_t version = 0;
    if (std::fread(&version, 1, 1, file) != 1 || std::fseek(file, 3, SEEK_CUR) != 0) {
        return 0;
    }

    bool ok = false;
    uint32_t timescale = 0;
    uint64_t duration = 0;
    if (version == 1) {
        if (std::fseek(file, 16, SEEK_CUR) != 0) {
            return 0;
        }
        timescale = read_be32(file, ok);
        if (!ok) {
            return 0;
        }
        duration = read_be64(file, ok);
    } else {
        if (std::fseek(file, 8, SEEK_CUR) != 0) {
            return 0;
        }
        timescale = read_be32(file, ok);
        if (!ok) {
            return 0;
        }
        duration = read_be32(file, ok);
    }
    if (!ok || timescale == 0 || duration == 0) {
        return 0;
    }
    return static_cast<int>((duration + timescale / 2) / timescale);
}

int scan_m4a_atoms_for_duration(FILE *file, long end_pos, int depth)
{
    if (depth > 6) {
        return 0;
    }
    while (true) {
        const long atom_start = std::ftell(file);
        if (atom_start < 0 || atom_start + 8 > end_pos) {
            return 0;
        }
        bool ok = false;
        uint64_t atom_size = read_be32(file, ok);
        if (!ok) {
            return 0;
        }
        const uint32_t type = read_be32(file, ok);
        if (!ok) {
            return 0;
        }
        uint64_t header_size = 8;
        if (atom_size == 1) {
            atom_size = read_be64(file, ok);
            if (!ok) {
                return 0;
            }
            header_size = 16;
        } else if (atom_size == 0) {
            atom_size = static_cast<uint64_t>(end_pos - atom_start);
        }
        if (atom_size < header_size) {
            return 0;
        }
        const uint64_t atom_end64 = static_cast<uint64_t>(atom_start) + atom_size;
        if (atom_end64 > static_cast<uint64_t>(end_pos)) {
            return 0;
        }
        const long atom_end = static_cast<long>(atom_end64);
        if (type == 0x6d646864) { // mdhd
            const int duration = parse_m4a_mdhd_duration(file);
            if (duration > 0) {
                return duration;
            }
        } else if (is_mp4_container_atom(type)) {
            const int duration = scan_m4a_atoms_for_duration(file, atom_end, depth + 1);
            if (duration > 0) {
                return duration;
            }
        }
        if (std::fseek(file, atom_end, SEEK_SET) != 0) {
            return 0;
        }
    }
}

int estimate_m4a_duration_seconds(const std::string &path)
{
    FILE *file = std::fopen(path.c_str(), "rb");
    if (!file) {
        return 0;
    }
    int duration = 0;
    if (std::fseek(file, 0, SEEK_END) == 0) {
        const long end_pos = std::ftell(file);
        if (end_pos > 0 && std::fseek(file, 0, SEEK_SET) == 0) {
            duration = scan_m4a_atoms_for_duration(file, end_pos, 0);
        }
    }
    std::fclose(file);
    return duration;
}

int estimate_wav_duration_seconds(const std::string &path)
{
    FILE *file = std::fopen(path.c_str(), "rb");
    if (!file) {
        return 0;
    }
    lofi::WavInfo info;
    const lofi::WavParseResult result = lofi::parse_wav_file(file, info);
    std::fclose(file);
    if (result != lofi::WavParseResult::Ok || info.sample_rate == 0 || info.channels == 0 || info.bits_per_sample == 0) {
        return 0;
    }
    const uint32_t bytes_per_second = info.sample_rate * info.channels * (info.bits_per_sample / 8);
    if (bytes_per_second == 0) {
        return 0;
    }
    return static_cast<int>((info.data_size + bytes_per_second / 2) / bytes_per_second);
}

void populate_track_durations(lofi::LibraryIndex &library)
{
    size_t known = 0;
    for (lofi::Track &track : library.tracks) {
        if (track.duration_seconds <= 0) {
            if (track.format == "m4a" || track.format == "aac") {
                track.duration_seconds = estimate_m4a_duration_seconds(track.path);
            } else if (track.format == "wav") {
                track.duration_seconds = estimate_wav_duration_seconds(track.path);
            }
        }
        if (track.duration_seconds > 0) {
            ++known;
        }
    }
    ESP_LOGI(TAG, "DURATION_INDEX known=%u total=%u",
             static_cast<unsigned>(known),
             static_cast<unsigned>(library.tracks.size()));
}

bool ensure_track_duration(lofi::Track &track)
{
    if (track.duration_seconds > 0) {
        return false;
    }
    if (track.format == "m4a" || track.format == "aac") {
        track.duration_seconds = estimate_m4a_duration_seconds(track.path);
    } else if (track.format == "wav") {
        track.duration_seconds = estimate_wav_duration_seconds(track.path);
    }
    if (track.duration_seconds <= 0) {
        return false;
    }
    ESP_LOGI(TAG,
             "DURATION_TRACK seconds=%d path=%s",
             track.duration_seconds,
             track.path.c_str());
    return true;
}

std::vector<std::string> load_cached_music_paths(bool sd_mounted, bool *cache_loaded)
{
    if (cache_loaded) {
        *cache_loaded = false;
    }
    if (!sd_mounted) {
        return {};
    }

    std::string text;
    std::vector<std::string> paths;
    if (!lofi::read_text_file(kIndexFile, text) || !lofi::parse_path_index(text, paths)) {
        return {};
    }
    if (cache_loaded) {
        *cache_loaded = true;
    }
    ESP_LOGI(TAG, "INDEX_LOAD ok path=%s tracks=%u", kIndexFile, static_cast<unsigned>(paths.size()));
    return paths;
}

LibraryScanResult build_library_from_paths(const std::vector<std::string> &music_paths,
                                           bool using_samples,
                                           bool from_cache,
                                           bool load_durations)
{
    LibraryScanResult result;
    result.using_samples = using_samples;
    result.from_cache = from_cache;
    result.library = lofi::build_library_index(music_paths);
    populate_track_album_art_sources(result.library);
    if (load_durations) {
        populate_track_durations(result.library);
    }
    log_library_summary(result.library);
    return result;
}

std::vector<std::string> scan_music_paths(bool sd_mounted, bool *using_samples)
{
    if (using_samples) {
        *using_samples = false;
    }

    std::vector<std::string> music_paths;
    if (sd_mounted) {
        lofi::StorageScanResult scan = lofi::collect_audio_paths("/sdcard/Music", 8);
        music_paths = scan.paths;
        for (const std::string &warning : scan.warnings) {
            ESP_LOGW(TAG, "Scan warning: %s", warning.c_str());
        }
        if (!music_paths.empty()) {
            if (lofi::ensure_directory(kStateDir) &&
                lofi::write_text_file(kIndexFile, lofi::serialize_path_index(music_paths))) {
                ESP_LOGI(TAG, "INDEX_SAVE ok path=%s tracks=%u", kIndexFile, static_cast<unsigned>(music_paths.size()));
            } else {
                ESP_LOGW(TAG, "INDEX_SAVE failed path=%s", kIndexFile);
            }
        }
    }

    if (music_paths.empty()) {
        ESP_LOGW(TAG, "No SD music found; using boot sample paths for UI self-check");
        music_paths = kBootSamplePaths;
        if (using_samples) {
            *using_samples = true;
        }
    }
    return music_paths;
}

LibraryScanResult scan_library(bool sd_mounted)
{
    bool using_samples = false;
    const std::vector<std::string> music_paths = scan_music_paths(sd_mounted, &using_samples);
    return build_library_from_paths(music_paths, using_samples, false, true);
}

bool mount_sdcard(sdmmc_card_t **out_card)
{
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;

    spi_bus_config_t bus_cfg = {};
    bus_cfg.mosi_io_num = PIN_SPI_MOSI;
    bus_cfg.miso_io_num = PIN_SPI_MISO;
    bus_cfg.sclk_io_num = PIN_SPI_SCLK;
    bus_cfg.quadwp_io_num = -1;
    bus_cfg.quadhd_io_num = -1;
    bus_cfg.max_transfer_sz = 4000;

    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "SD SPI bus init failed: %s", esp_err_to_name(err));
        return false;
    }

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = static_cast<gpio_num_t>(PIN_SD_CS);
    slot_config.host_id = SPI2_HOST;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {};
    mount_config.format_if_mount_failed = false;
    mount_config.max_files = 5;
    mount_config.allocation_unit_size = 16 * 1024;

    err = esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot_config, &mount_config, out_card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD mount failed: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "SD mounted: %s", (*out_card)->cid.name);
    return true;
}

MediaFormatCounts count_media_formats(const lofi::LibraryIndex &library)
{
    MediaFormatCounts counts;
    for (const lofi::Track &track : library.tracks) {
        if (track.format == "mp3") {
            ++counts.mp3;
        } else if (track.format == "wav") {
            ++counts.wav;
        } else if (track.format == "m4a" || track.format == "aac") {
            ++counts.m4a_aac;
        } else {
            ++counts.other;
        }
    }
    return counts;
}

void log_media_format_counts(const lofi::LibraryIndex &library)
{
    const MediaFormatCounts counts = count_media_formats(library);
    ESP_LOGI(TAG, "MEDIA_FORMATS mp3=%u wav=%u m4a_aac=%u other=%u",
             static_cast<unsigned>(counts.mp3),
             static_cast<unsigned>(counts.wav),
             static_cast<unsigned>(counts.m4a_aac),
             static_cast<unsigned>(counts.other));
}

void log_library_summary(const lofi::LibraryIndex &library)
{
    ESP_LOGI(TAG, "Indexed %u tracks, %u albums, %u artists",
             static_cast<unsigned>(library.tracks.size()),
             static_cast<unsigned>(library.albums.size()),
             static_cast<unsigned>(library.artists.size()));
    log_media_format_counts(library);
}

bool file_exists(const std::string &path)
{
    struct stat st = {};
    return !path.empty() && stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

std::string parent_dir(const std::string &path)
{
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos || slash == 0) {
        return "";
    }
    return path.substr(0, slash);
}

std::string find_sidecar_album_art(const std::string &track_path)
{
    const std::string dir = parent_dir(track_path);
    if (dir.empty()) {
        return "";
    }
    static constexpr const char *kCandidates[] = {
        "cover.jpg", "folder.jpg", "album.jpg", "front.jpg",
        "Cover.jpg", "Folder.jpg", "Album.jpg", "Front.jpg",
        "cover.jpeg", "folder.jpeg", "album.jpeg", "front.jpeg",
    };
    for (const char *name : kCandidates) {
        const std::string candidate = dir + "/" + name;
        if (file_exists(candidate)) {
            return candidate;
        }
    }
    return "";
}

enum class EmbeddedArtKind {
    None,
    Jpeg,
    Other,
};

struct EmbeddedAlbumArt {
    EmbeddedArtKind kind = EmbeddedArtKind::None;
    uint32_t offset = 0;
    uint32_t size = 0;
};

bool range_starts_with_jpeg(FILE *file, uint32_t offset, uint32_t size)
{
    if (size < 2 || std::fseek(file, static_cast<long>(offset), SEEK_SET) != 0) {
        return false;
    }
    uint8_t marker[2] = {};
    return std::fread(marker, 1, sizeof(marker), file) == sizeof(marker) &&
           marker[0] == 0xff && marker[1] == 0xd8;
}

bool mime_is_jpeg(const std::string &mime)
{
    return strcasecmp(mime.c_str(), "image/jpeg") == 0 ||
           strcasecmp(mime.c_str(), "image/jpg") == 0;
}

EmbeddedAlbumArt parse_id3_apic_payload(FILE *file,
                                        uint32_t payload_offset,
                                        uint32_t payload_size)
{
    EmbeddedAlbumArt art;
    if (payload_size < 8 || std::fseek(file, static_cast<long>(payload_offset), SEEK_SET) != 0) {
        return art;
    }
    const uint32_t payload_end = payload_offset + payload_size;
    uint8_t encoding = 0;
    if (std::fread(&encoding, 1, 1, file) != 1) {
        return art;
    }

    std::string mime;
    while (static_cast<uint32_t>(std::ftell(file)) < payload_end) {
        int ch = std::fgetc(file);
        if (ch < 0 || ch == 0) {
            break;
        }
        if (mime.size() < 48) {
            mime.push_back(static_cast<char>(ch));
        }
    }
    if (static_cast<uint32_t>(std::ftell(file)) >= payload_end || std::fgetc(file) < 0) {
        return art;
    }

    if (encoding == 1 || encoding == 2) {
        int previous = -1;
        while (static_cast<uint32_t>(std::ftell(file)) < payload_end) {
            int ch = std::fgetc(file);
            if (previous == 0 && ch == 0) {
                break;
            }
            previous = ch;
        }
    } else {
        while (static_cast<uint32_t>(std::ftell(file)) < payload_end) {
            int ch = std::fgetc(file);
            if (ch < 0 || ch == 0) {
                break;
            }
        }
    }
    const long image_offset = std::ftell(file);
    if (image_offset < 0 || static_cast<uint32_t>(image_offset) >= payload_end) {
        return art;
    }

    art.offset = static_cast<uint32_t>(image_offset);
    art.size = payload_end - art.offset;
    art.kind = mime_is_jpeg(mime) || range_starts_with_jpeg(file, art.offset, art.size) ? EmbeddedArtKind::Jpeg
                                                                                         : EmbeddedArtKind::Other;
    return art;
}

EmbeddedAlbumArt find_mp3_embedded_album_art(const std::string &path)
{
    EmbeddedAlbumArt art;
    FILE *file = std::fopen(path.c_str(), "rb");
    if (!file) {
        return art;
    }
    uint8_t header[10] = {};
    if (std::fread(header, 1, sizeof(header), file) != sizeof(header) ||
        std::memcmp(header, "ID3", 3) != 0) {
        std::fclose(file);
        return art;
    }

    const uint8_t major = header[3];
    const uint8_t flags = header[5];
    const uint32_t tag_size = read_syncsafe32_bytes(header + 6);
    uint32_t cursor = 10;
    const uint32_t tag_end = cursor + tag_size;

    if (flags & 0x40) {
        if (std::fseek(file, static_cast<long>(cursor), SEEK_SET) == 0) {
            uint8_t ext[4] = {};
            if (std::fread(ext, 1, sizeof(ext), file) == sizeof(ext)) {
                uint32_t ext_size = major == 4 ? read_syncsafe32_bytes(ext) : read_be32_bytes(ext);
                cursor += 4 + ext_size;
            }
        }
    }

    while (cursor + 10 <= tag_end && std::fseek(file, static_cast<long>(cursor), SEEK_SET) == 0) {
        uint8_t frame[10] = {};
        if (std::fread(frame, 1, sizeof(frame), file) != sizeof(frame) || frame[0] == 0) {
            break;
        }
        const uint32_t frame_size = major == 4 ? read_syncsafe32_bytes(frame + 4) : read_be32_bytes(frame + 4);
        const uint32_t payload_offset = cursor + 10;
        if (frame_size == 0 || payload_offset + frame_size > tag_end) {
            break;
        }
        if (std::memcmp(frame, "APIC", 4) == 0) {
            art = parse_id3_apic_payload(file, payload_offset, frame_size);
            break;
        }
        cursor = payload_offset + frame_size;
    }
    std::fclose(file);
    return art;
}

bool is_mp4_cover_container_atom(uint32_t type)
{
    return type == 0x6d6f6f76 || // moov
           type == 0x75647461 || // udta
           type == 0x696c7374 || // ilst
           type == 0x636f7672;   // covr
}

EmbeddedAlbumArt scan_m4a_atoms_for_cover(FILE *file, long end_pos, int depth)
{
    EmbeddedAlbumArt art;
    if (depth > 8) {
        return art;
    }
    while (true) {
        const long atom_start = std::ftell(file);
        if (atom_start < 0 || atom_start + 8 > end_pos) {
            return art;
        }
        bool ok = false;
        uint64_t atom_size = read_be32(file, ok);
        if (!ok) {
            return art;
        }
        const uint32_t type = read_be32(file, ok);
        if (!ok) {
            return art;
        }
        uint64_t header_size = 8;
        if (atom_size == 1) {
            atom_size = read_be64(file, ok);
            if (!ok) {
                return art;
            }
            header_size = 16;
        } else if (atom_size == 0) {
            atom_size = static_cast<uint64_t>(end_pos - atom_start);
        }
        if (atom_size < header_size) {
            return art;
        }
        const uint64_t atom_end64 = static_cast<uint64_t>(atom_start) + atom_size;
        if (atom_end64 > static_cast<uint64_t>(end_pos)) {
            return art;
        }
        const long atom_end = static_cast<long>(atom_end64);
        long payload_start = atom_start + static_cast<long>(header_size);

        if (type == 0x64617461 && atom_end - payload_start > 8) { // data
            uint8_t data_header[8] = {};
            if (std::fread(data_header, 1, sizeof(data_header), file) != sizeof(data_header)) {
                return art;
            }
            const uint32_t data_type = read_be32_bytes(data_header) & 0xffffffu;
            art.offset = static_cast<uint32_t>(payload_start + 8);
            art.size = static_cast<uint32_t>(atom_end - payload_start - 8);
            art.kind = (data_type == 13 || range_starts_with_jpeg(file, art.offset, art.size))
                           ? EmbeddedArtKind::Jpeg
                           : EmbeddedArtKind::Other;
            return art;
        }

        if (type == 0x6d657461) { // meta full box
            payload_start += 4;
        }
        if (payload_start < atom_end && (is_mp4_cover_container_atom(type) || type == 0x6d657461)) {
            if (std::fseek(file, payload_start, SEEK_SET) != 0) {
                return art;
            }
            art = scan_m4a_atoms_for_cover(file, atom_end, depth + 1);
            if (art.kind != EmbeddedArtKind::None) {
                return art;
            }
        }
        if (std::fseek(file, atom_end, SEEK_SET) != 0) {
            return art;
        }
    }
}

EmbeddedAlbumArt find_m4a_embedded_album_art(const std::string &path)
{
    EmbeddedAlbumArt art;
    FILE *file = std::fopen(path.c_str(), "rb");
    if (!file) {
        return art;
    }
    if (std::fseek(file, 0, SEEK_END) == 0) {
        const long end_pos = std::ftell(file);
        if (end_pos > 0 && std::fseek(file, 0, SEEK_SET) == 0) {
            art = scan_m4a_atoms_for_cover(file, end_pos, 0);
        }
    }
    std::fclose(file);
    return art;
}

EmbeddedAlbumArt find_embedded_album_art(const lofi::Track &track)
{
    if (track.format == "mp3") {
        return find_mp3_embedded_album_art(track.path);
    }
    if (track.format == "m4a" || track.format == "aac") {
        return find_m4a_embedded_album_art(track.path);
    }
    return {};
}

uint32_t fnv1a32(const std::string &value)
{
    uint32_t hash = 2166136261u;
    for (unsigned char ch : value) {
        hash ^= static_cast<uint32_t>(ch);
        hash *= 16777619u;
    }
    return hash;
}

std::string cover_cache_path_for(const lofi::Track &track)
{
    struct stat st = {};
    uint32_t size_hash = 0;
    uint32_t mtime_hash = 0;
    if (stat(track.album_art_path.c_str(), &st) == 0) {
        size_hash = static_cast<uint32_t>(st.st_size & 0xffffffffu);
        mtime_hash = static_cast<uint32_t>(st.st_mtime & 0xffffffffu);
    }
    char identity[240] = {};
    std::snprintf(identity,
                  sizeof(identity),
                  "%s:%lu:%lu:%d",
                  track.album_art_path.c_str(),
                  static_cast<unsigned long>(track.album_art_offset),
                  static_cast<unsigned long>(track.album_art_size),
                  track.album_art_embedded ? 1 : 0);
    char path[160] = {};
    std::snprintf(path,
                  sizeof(path),
                  "%s/%02dx%02d_%08lx_%08lx_%08lx.rgb565",
                  kCoverCacheDir,
                  kCoverThumbWidth,
                  kCoverThumbHeight,
                  static_cast<unsigned long>(fnv1a32(identity)),
                  static_cast<unsigned long>(size_hash),
                  static_cast<unsigned long>(mtime_hash));
    return path;
}

std::string audio_cache_path_for(const lofi::Track &track)
{
    struct stat st = {};
    uint32_t size_hash = 0;
    uint32_t mtime_hash = 0;
    if (stat(track.path.c_str(), &st) == 0) {
        size_hash = static_cast<uint32_t>(st.st_size & 0xffffffffu);
        mtime_hash = static_cast<uint32_t>(st.st_mtime & 0xffffffffu);
    }
    char identity[256] = {};
    std::snprintf(identity,
                  sizeof(identity),
                  "%s:%lu:%lu",
                  track.path.c_str(),
                  static_cast<unsigned long>(size_hash),
                  static_cast<unsigned long>(mtime_hash));
    char path[160] = {};
    std::snprintf(path,
                  sizeof(path),
                  "%s/%08lx_%08lx_%08lx.aac",
                  kAudioCacheDir,
                  static_cast<unsigned long>(fnv1a32(identity)),
                  static_cast<unsigned long>(size_hash),
                  static_cast<unsigned long>(mtime_hash));
    return path;
}

std::string existing_audio_cache_path_for(const lofi::Track &track)
{
    if (track.format != "m4a") {
        return "";
    }
    const std::string cache_path = audio_cache_path_for(track);
    return file_exists(cache_path) ? cache_path : "";
}

bool build_audio_cache_for_track(const lofi::Track &track)
{
    if (track.format != "m4a" || !file_exists(track.path)) {
        return false;
    }
    const std::string cache_path = audio_cache_path_for(track);
    if (file_exists(cache_path)) {
        return false;
    }
    lofi::M4aAacSummary summary;
    std::string error;
    const int64_t parse_start = esp_timer_get_time();
    if (!lofi::inspect_m4a_aac_summary(track.path, summary, error)) {
        ESP_LOGW(TAG, "AAC_CACHE index failed path=%s err=%s", track.path.c_str(), error.c_str());
        return false;
    }
    if (summary.first_audio_offset < kSlowStartAudioOffsetThreshold) {
        ESP_LOGI(TAG,
                 "AAC_CACHE skip fast-start offset=%lu frames=%u path=%s",
                 static_cast<unsigned long>(summary.first_audio_offset),
                 static_cast<unsigned>(summary.frame_count),
                 track.path.c_str());
        return false;
    }
    if (!lofi::ensure_directory(kStateDir) || !lofi::ensure_directory(kAudioCacheDir)) {
        ESP_LOGW(TAG, "AAC_CACHE mkdir failed");
        return false;
    }
    const std::string temp_path = cache_path + ".tmp";
    std::remove(temp_path.c_str());
    const int64_t write_start = esp_timer_get_time();
    if (!lofi::write_adts_aac_cache_from_m4a(track.path,
                                             temp_path,
                                             summary,
                                             error,
                                             audio_cache_cancel_requested,
                                             nullptr)) {
        std::remove(temp_path.c_str());
        if (error == "cancelled") {
            ESP_LOGI(TAG, "AAC_CACHE cancelled path=%s", track.path.c_str());
        } else {
            ESP_LOGW(TAG, "AAC_CACHE write failed path=%s err=%s", track.path.c_str(), error.c_str());
        }
        return false;
    }
    std::remove(cache_path.c_str());
    if (std::rename(temp_path.c_str(), cache_path.c_str()) != 0) {
        std::remove(temp_path.c_str());
        ESP_LOGW(TAG, "AAC_CACHE rename failed cache=%s", cache_path.c_str());
        return false;
    }
    const int64_t now = esp_timer_get_time();
    ESP_LOGI(TAG,
             "AAC_CACHE built offset=%lu frames=%u payload=%lu parse_ms=%lld write_ms=%lld cache=%s src=%s",
             static_cast<unsigned long>(summary.first_audio_offset),
             static_cast<unsigned>(summary.frame_count),
             static_cast<unsigned long>(summary.audio_payload_bytes),
             static_cast<long long>((write_start - parse_start) / 1000),
             static_cast<long long>((now - write_start) / 1000),
             cache_path.c_str(),
             track.path.c_str());
    return true;
}

void background_audio_cache_task(void *arg)
{
    AudioCacheTaskContext *context = static_cast<AudioCacheTaskContext *>(arg);
    QueueHandle_t result_queue = context ? context->result_queue : nullptr;
    lofi::Track track;
    if (context) {
        track = std::move(context->track);
    }
    delete context;

    AudioCacheResult *result = new (std::nothrow) AudioCacheResult;
    if (!result) {
        ESP_LOGW(TAG, "AAC_CACHE result allocation failed");
        if (result_queue) {
            AudioCacheResult *empty_result = nullptr;
            xQueueSend(result_queue, &empty_result, 0);
        }
        vTaskDelete(nullptr);
        return;
    }
    result->attempted = true;
    result->built = build_audio_cache_for_track(track);
    if (!result_queue || xQueueSend(result_queue, &result, 0) != pdTRUE) {
        ESP_LOGW(TAG, "AAC_CACHE dropped result");
        delete result;
    }
    vTaskDelete(nullptr);
}

QueueHandle_t start_background_audio_cache(const lofi::Track &track)
{
    QueueHandle_t result_queue = xQueueCreate(1, sizeof(AudioCacheResult *));
    if (!result_queue) {
        ESP_LOGW(TAG, "AAC_CACHE queue allocation failed");
        return nullptr;
    }
    AudioCacheTaskContext *context = new (std::nothrow) AudioCacheTaskContext{track, result_queue};
    if (!context) {
        ESP_LOGW(TAG, "AAC_CACHE context allocation failed");
        vQueueDelete(result_queue);
        return nullptr;
    }
    g_audio_cache_cancel_requested = false;
    if (xTaskCreatePinnedToCore(background_audio_cache_task,
                                "aac_cache",
                                8192,
                                context,
                                tskIDLE_PRIORITY + 1,
                                nullptr,
                                1) != pdPASS) {
        ESP_LOGW(TAG, "AAC_CACHE task start failed");
        delete context;
        vQueueDelete(result_queue);
        return nullptr;
    }
    ESP_LOGI(TAG, "AAC_CACHE task started path=%s", track.path.c_str());
    return result_queue;
}

bool start_next_audio_cache_candidate(const lofi::LibraryIndex &library, size_t &cursor, QueueHandle_t &result_queue)
{
    if (library.tracks.empty()) {
        return false;
    }
    const size_t total = library.tracks.size();
    for (size_t attempts = 0; attempts < total; ++attempts) {
        const size_t index = cursor % total;
        cursor = (cursor + 1) % total;
        const lofi::Track &track = library.tracks[index];
        if (track.format != "m4a" || !existing_audio_cache_path_for(track).empty()) {
            continue;
        }
        if (!track.album_art_embedded || track.album_art_size < kSlowStartAudioOffsetThreshold) {
            continue;
        }
        const size_t largest_free = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
        if (largest_free < kAudioCacheMinLargestHeapBlock) {
            ESP_LOGI(TAG,
                     "AAC_CACHE defer low-heap largest=%u path=%s",
                     static_cast<unsigned>(largest_free),
                     track.path.c_str());
            return false;
        }
        result_queue = start_background_audio_cache(track);
        return result_queue != nullptr;
    }
    return false;
}

uint16_t cover_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return ((uint16_t)(r & 0xF8) << 8) | ((uint16_t)(g & 0xFC) << 3) | (uint16_t)(b >> 3);
}

struct CoverDecodeContext {
    FILE *file = nullptr;
    uint16_t *pixels = nullptr;
    bool limited = false;
    uint32_t remaining = 0;
    int decoded_width = 0;
    int decoded_height = 0;
    int crop_x = 0;
    int crop_y = 0;
    int crop_size = 0;
};

UINT cover_jpeg_input(JDEC *decoder, BYTE *buffer, UINT requested)
{
    CoverDecodeContext *context = static_cast<CoverDecodeContext *>(decoder->device);
    if (!context || !context->file) {
        return 0;
    }
    if (context->limited) {
        requested = std::min<UINT>(requested, context->remaining);
    }
    if (!buffer) {
        if (std::fseek(context->file, requested, SEEK_CUR) != 0) {
            return 0;
        }
        if (context->limited) {
            context->remaining -= requested;
        }
        return requested;
    }
    const UINT got = static_cast<UINT>(std::fread(buffer, 1, requested, context->file));
    if (context->limited) {
        context->remaining -= got;
    }
    return got;
}

UINT cover_jpeg_output(JDEC *decoder, void *bitmap, JRECT *rect)
{
    CoverDecodeContext *context = static_cast<CoverDecodeContext *>(decoder->device);
    if (!context || !context->pixels || !bitmap || !rect || context->crop_size <= 0) {
        return 0;
    }

    const int block_width = static_cast<int>(rect->right - rect->left + 1);
    const uint8_t *rgb = static_cast<const uint8_t *>(bitmap);
    for (int y = rect->top; y <= rect->bottom; ++y) {
        if (y < context->crop_y || y >= context->crop_y + context->crop_size) {
            continue;
        }
        for (int x = rect->left; x <= rect->right; ++x) {
            if (x < context->crop_x || x >= context->crop_x + context->crop_size) {
                continue;
            }
            const int tx = (x - context->crop_x) * kCoverThumbWidth / context->crop_size;
            const int ty = (y - context->crop_y) * kCoverThumbHeight / context->crop_size;
            if (tx < 0 || tx >= kCoverThumbWidth || ty < 0 || ty >= kCoverThumbHeight) {
                continue;
            }
            const int block_x = x - rect->left;
            const int block_y = y - rect->top;
            const uint8_t *src = rgb + (block_y * block_width + block_x) * 3;
            context->pixels[ty * kCoverThumbWidth + tx] = cover_rgb565(src[0], src[1], src[2]);
        }
    }
    return 1;
}

bool decode_jpeg_to_cover_cache(const lofi::Track &track, const std::string &cache_path)
{
    if (!lofi::ensure_directory(kStateDir) || !lofi::ensure_directory(kCoverCacheDir)) {
        return false;
    }

    FILE *file = std::fopen(track.album_art_path.c_str(), "rb");
    if (!file) {
        ESP_LOGW(TAG, "COVER_DECODE open failed path=%s", track.album_art_path.c_str());
        return false;
    }
    if (track.album_art_embedded &&
        std::fseek(file, static_cast<long>(track.album_art_offset), SEEK_SET) != 0) {
        std::fclose(file);
        ESP_LOGW(TAG,
                 "COVER_DECODE seek failed path=%s offset=%lu",
                 track.album_art_path.c_str(),
                 static_cast<unsigned long>(track.album_art_offset));
        return false;
    }

    JDEC decoder = {};
    CoverDecodeContext context;
    context.file = file;
    context.limited = track.album_art_embedded;
    context.remaining = track.album_art_size;
    uint8_t *work = static_cast<uint8_t *>(std::malloc(16 * 1024));
    uint16_t *pixels = static_cast<uint16_t *>(std::malloc(kCoverThumbWidth * kCoverThumbHeight * sizeof(uint16_t)));
    if (!work || !pixels) {
        std::free(work);
        std::free(pixels);
        std::fclose(file);
        ESP_LOGW(TAG, "COVER_DECODE allocation failed path=%s", track.album_art_path.c_str());
        return false;
    }
    std::fill_n(pixels, kCoverThumbWidth * kCoverThumbHeight, cover_rgb565(20, 18, 24));
    context.pixels = pixels;

    JRESULT result = jd_prepare(&decoder, cover_jpeg_input, work, 16 * 1024, &context);
    if (result == JDR_OK) {
        BYTE scale = 0;
        const UINT min_side = std::min(decoder.width, decoder.height);
        while (scale < 3 && (min_side >> (scale + 1)) >= static_cast<UINT>(kCoverThumbWidth)) {
            ++scale;
        }
        context.decoded_width = std::max<int>(1, (static_cast<int>(decoder.width) + (1 << scale) - 1) >> scale);
        context.decoded_height = std::max<int>(1, (static_cast<int>(decoder.height) + (1 << scale) - 1) >> scale);
        context.crop_size = std::min(context.decoded_width, context.decoded_height);
        context.crop_x = (context.decoded_width - context.crop_size) / 2;
        context.crop_y = (context.decoded_height - context.crop_size) / 2;
        result = jd_decomp(&decoder, cover_jpeg_output, scale);
    }
    std::fclose(file);

    bool ok = result == JDR_OK;
    if (ok) {
        FILE *out = std::fopen(cache_path.c_str(), "wb");
        ok = out && std::fwrite(pixels, sizeof(uint16_t), kCoverThumbWidth * kCoverThumbHeight, out) ==
                        static_cast<size_t>(kCoverThumbWidth * kCoverThumbHeight);
        if (out) {
            ok = std::fclose(out) == 0 && ok;
        }
    }

    ESP_LOGI(TAG,
             "COVER_DECODE result=%s code=%d src=%s cache=%s",
             ok ? "ok" : "failed",
             static_cast<int>(result),
             track.album_art_path.c_str(),
             cache_path.c_str());
    std::free(work);
    std::free(pixels);
    return ok;
}

void populate_track_album_art_sources(lofi::LibraryIndex &library)
{
    size_t sidecar = 0;
    size_t embedded_jpeg = 0;
    size_t embedded_other = 0;
    for (lofi::Track &track : library.tracks) {
        track.album_art_path.clear();
        track.album_art_cache_path.clear();
        track.album_art_offset = 0;
        track.album_art_size = 0;
        track.album_art_embedded = false;

        const EmbeddedAlbumArt embedded = find_embedded_album_art(track);
        if (embedded.kind == EmbeddedArtKind::Jpeg) {
            track.album_art_path = track.path;
            track.album_art_offset = embedded.offset;
            track.album_art_size = embedded.size;
            track.album_art_embedded = true;
            ++embedded_jpeg;
            continue;
        }
        if (embedded.kind == EmbeddedArtKind::Other) {
            ++embedded_other;
        }

        track.album_art_path = find_sidecar_album_art(track.path);
        if (!track.album_art_path.empty()) {
            ++sidecar;
        }
    }
    ESP_LOGI(TAG,
             "COVER_INDEX embedded_jpeg=%u embedded_other=%u sidecar=%u tracks=%u",
             static_cast<unsigned>(embedded_jpeg),
             static_cast<unsigned>(embedded_other),
             static_cast<unsigned>(sidecar),
             static_cast<unsigned>(library.tracks.size()));
}

bool ensure_track_album_art_cache(lofi::Track &track, bool allow_decode)
{
    if (track.album_art_path.empty()) {
        return false;
    }
    if (!track.album_art_cache_path.empty() && file_exists(track.album_art_cache_path)) {
        return false;
    }

    const std::string cache_path = cover_cache_path_for(track);
    if (file_exists(cache_path)) {
        const bool changed = track.album_art_cache_path != cache_path;
        track.album_art_cache_path = cache_path;
        return changed;
    }
    if (!allow_decode) {
        return false;
    }
    if (decode_jpeg_to_cover_cache(track, cache_path)) {
        track.album_art_cache_path = cache_path;
        return true;
    }

    track.album_art_path.clear();
    track.album_art_cache_path.clear();
    track.album_art_offset = 0;
    track.album_art_size = 0;
    track.album_art_embedded = false;
    return false;
}

bool ensure_current_album_art_cache(lofi::LibraryIndex &library, const lofi::PlaybackState &playback)
{
    const int current = lofi::queue_current_track(playback.queue);
    if (current < 0 || static_cast<size_t>(current) >= library.tracks.size()) {
        return false;
    }
    return ensure_track_album_art_cache(library.tracks[static_cast<size_t>(current)], !playback.playing);
}

bool ensure_current_track_duration(lofi::LibraryIndex &library, const lofi::PlaybackState &playback)
{
    const int current = lofi::queue_current_track(playback.queue);
    if (current < 0 || static_cast<size_t>(current) >= library.tracks.size()) {
        return false;
    }
    return ensure_track_duration(library.tracks[static_cast<size_t>(current)]);
}

void write_le16(FILE *file, uint16_t value)
{
    std::fputc(value & 0xff, file);
    std::fputc((value >> 8) & 0xff, file);
}

void write_le32(FILE *file, uint32_t value)
{
    std::fputc(value & 0xff, file);
    std::fputc((value >> 8) & 0xff, file);
    std::fputc((value >> 16) & 0xff, file);
    std::fputc((value >> 24) & 0xff, file);
}

bool write_selftest_wav_file()
{
    if (!lofi::ensure_directory(kStateDir)) {
        return false;
    }

    FILE *file = std::fopen(kSelfTestWavFile, "wb");
    if (!file) {
        return false;
    }

    constexpr uint32_t sample_rate = 44100;
    constexpr uint16_t channels = 2;
    constexpr uint16_t bits_per_sample = 16;
    constexpr uint32_t seconds = 2;
    constexpr uint32_t frame_count = sample_rate * seconds;
    constexpr uint32_t block_align = channels * bits_per_sample / 8;
    constexpr uint32_t byte_rate = sample_rate * block_align;
    constexpr uint32_t data_size = frame_count * block_align;

    std::fwrite("RIFF", 1, 4, file);
    write_le32(file, 36 + data_size);
    std::fwrite("WAVE", 1, 4, file);
    std::fwrite("fmt ", 1, 4, file);
    write_le32(file, 16);
    write_le16(file, 1);
    write_le16(file, channels);
    write_le32(file, sample_rate);
    write_le32(file, byte_rate);
    write_le16(file, block_align);
    write_le16(file, bits_per_sample);
    std::fwrite("data", 1, 4, file);
    write_le32(file, data_size);

    for (uint32_t i = 0; i < frame_count; ++i) {
        const uint32_t phase = (i * 440U) % sample_rate;
        const int32_t ramp = phase < sample_rate / 2 ? static_cast<int32_t>(phase) :
                                                        static_cast<int32_t>(sample_rate - phase);
        const int16_t sample = static_cast<int16_t>((ramp * 1800) / static_cast<int32_t>(sample_rate / 2) - 900);
        write_le16(file, static_cast<uint16_t>(sample));
        write_le16(file, static_cast<uint16_t>(sample));
    }

    const bool ok = std::fclose(file) == 0;
    ESP_LOGI(TAG, "WAV_SELFTEST_WRITE result=%s path=%s bytes=%u", ok ? "ok" : "error", kSelfTestWavFile,
             static_cast<unsigned>(44 + data_size));
    return ok;
}

bool write_selftest_mp3_file()
{
    if (!lofi::ensure_directory(kStateDir)) {
        return false;
    }

    FILE *file = std::fopen(kSelfTestMp3File, "wb");
    if (!file) {
        return false;
    }
    const size_t wrote = std::fwrite(kSelfTestMp3Data, 1, kSelfTestMp3DataLen, file);
    const int close_result = std::fclose(file);
    const bool ok = wrote == kSelfTestMp3DataLen && close_result == 0;
    ESP_LOGI(TAG, "MP3_SELFTEST_WRITE result=%s path=%s bytes=%u", ok ? "ok" : "error", kSelfTestMp3File,
             static_cast<unsigned>(wrote));
    return ok;
}

bool start_wav_selftest(bool sd_mounted, int volume, const lofi::LofiProfile &profile)
{
    if (!sd_mounted) {
        ESP_LOGW(TAG, "WAV_SELFTEST result=no_sd");
        return false;
    }
    if (!write_selftest_wav_file()) {
        ESP_LOGW(TAG, "WAV_SELFTEST result=write_failed path=%s", kSelfTestWavFile);
        return false;
    }

    lofi::Track track;
    track.id = "selftest-wav";
    track.path = kSelfTestWavFile;
    track.title = "Self Test WAV";
    track.artist = "LoFi Player";
    track.album = "Diagnostics";
    track.album_artist = "LoFi Player";
    track.duration_seconds = 2;
    track.format = "wav";

    (void)volume;
    const int test_volume = 0;
    const esp_err_t err = lofi_audio::play_track(track, test_volume, profile, 0);
    ESP_LOGI(TAG, "WAV_SELFTEST result=%s path=%s volume=%d", esp_err_to_name(err), kSelfTestWavFile, test_volume);
    return err == ESP_OK;
}

bool start_mp3_selftest(bool sd_mounted, int volume, const lofi::LofiProfile &profile)
{
    if (!sd_mounted) {
        ESP_LOGW(TAG, "MP3_SELFTEST result=no_sd");
        return false;
    }
    if (!write_selftest_mp3_file()) {
        ESP_LOGW(TAG, "MP3_SELFTEST result=write_failed path=%s", kSelfTestMp3File);
        return false;
    }

    lofi::Track track;
    track.id = "selftest-mp3";
    track.path = kSelfTestMp3File;
    track.title = "Self Test MP3";
    track.artist = "LoFi Player";
    track.album = "Diagnostics";
    track.album_artist = "LoFi Player";
    track.duration_seconds = 1;
    track.format = "mp3";

    (void)volume;
    const int test_volume = 0;
    const esp_err_t err = lofi_audio::play_track(track, test_volume, profile, 0);
    ESP_LOGI(TAG, "MP3_SELFTEST result=%s path=%s volume=%d", esp_err_to_name(err), kSelfTestMp3File, test_volume);
    return err == ESP_OK;
}

bool track_is_playable(const lofi::Track &track)
{
    return track.format == "mp3" || track.format == "wav" || track.format == "m4a" || track.format == "aac";
}

bool advance_to_next_playable(const lofi::LibraryIndex &library, lofi::PlaybackState &playback)
{
    if (playback.queue.track_indices.empty()) {
        return false;
    }
    const size_t count = playback.queue.track_indices.size();
    for (size_t attempt = 0; attempt < count; ++attempt) {
        const int current = lofi::queue_current_track(playback.queue);
        if (current >= 0 && static_cast<size_t>(current) < library.tracks.size() &&
            track_is_playable(library.tracks[static_cast<size_t>(current)])) {
            return true;
        }
        if (current >= 0 && static_cast<size_t>(current) < library.tracks.size()) {
            const lofi::Track &track = library.tracks[static_cast<size_t>(current)];
            ESP_LOGI(TAG, "AUDIO_SKIP unsupported format=%s path=%s", track.format.c_str(), track.path.c_str());
        }
        if (playback.queue.current_index + 1 < playback.queue.track_indices.size()) {
            ++playback.queue.current_index;
        } else {
            playback.queue.current_index = 0;
        }
        playback.current_track = lofi::queue_current_track(playback.queue);
        playback.position_seconds = 0;
    }
    return false;
}

void log_screen(const lofi::ScreenModel &screen)
{
    static uint32_t revision = 0;
    ++revision;
    ESP_LOGI(TAG, "%s", lofi::screen_auto_snapshot(screen, revision).c_str());
    const std::vector<std::string> lines = lofi::screen_to_lines(screen);
    for (const std::string &line : lines) {
        ESP_LOGI(TAG, "UI %s", line.c_str());
    }
}

const char *audio_status_name(lofi_audio::Status status)
{
    switch (status) {
    case lofi_audio::Status::Idle:
        return "idle";
    case lofi_audio::Status::Playing:
        return "playing";
    case lofi_audio::Status::Paused:
        return "paused";
    case lofi_audio::Status::Ended:
        return "ended";
    case lofi_audio::Status::Unsupported:
        return "unsupported";
    case lofi_audio::Status::Error:
        return "error";
    }
    return "unknown";
}

bool lofi_profiles_equal(const lofi::LofiProfile &a, const lofi::LofiProfile &b)
{
    return a.preset == b.preset && a.intensity == b.intensity && a.warmth == b.warmth && a.noise == b.noise &&
           a.wobble == b.wobble && a.space == b.space && a.softness == b.softness;
}

bool should_sync_audio_position(bool audio_loaded, lofi_audio::Status status)
{
    return audio_loaded || status == lofi_audio::Status::Playing || status == lofi_audio::Status::Ended;
}

bool should_pause_active_audio(lofi_audio::Status status)
{
    return status == lofi_audio::Status::Playing || status == lofi_audio::Status::Paused;
}

void configure_serial_debug_input()
{
    if (!usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_config_t usb_config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
        const esp_err_t err = usb_serial_jtag_driver_install(&usb_config);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "SERIAL_INPUT usb_serial_jtag_driver_install failed: %s", esp_err_to_name(err));
        }
    }
    if (usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_vfs_use_driver();
    }
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
    }
    ESP_LOGI(TAG, "SERIAL_INPUT ready enter=ok space=play n=next p=prev s=shuffle r=repeat l=lofi m=menu b=back h=home g=scan [=left ]=right <=seek_back >=seek_forward d=frame_dump t=wav_selftest y=mp3_selftest");
}

int read_serial_debug_char()
{
    if (usb_serial_jtag_is_driver_installed()) {
        uint8_t byte = 0;
        const int n = usb_serial_jtag_read_bytes(&byte, 1, 0);
        if (n == 1) {
            return byte;
        }
    }
    errno = 0;
    return getchar();
}

bool poll_serial_action(lofi::Action &action,
                        const char **name,
                        bool &frame_dump,
                        bool &wav_selftest,
                        bool &mp3_selftest)
{
    bool saw_input = false;
    frame_dump = false;
    wav_selftest = false;
    mp3_selftest = false;
    while (true) {
        const int ch = read_serial_debug_char();
        if (ch == EOF) {
            break;
        }
        saw_input = true;
        switch (ch) {
        case '\r':
        case '\n':
            action = lofi::Action::Ok;
            *name = "SERIAL_ENTER";
            return true;
        case ' ':
            action = lofi::Action::PlayPause;
            *name = "SERIAL_SPACE";
            return true;
        case 'n':
        case 'N':
            action = lofi::Action::Next;
            *name = "SERIAL_N";
            return true;
        case 'p':
        case 'P':
            action = lofi::Action::Previous;
            *name = "SERIAL_P";
            return true;
        case 's':
        case 'S':
            action = lofi::Action::Shuffle;
            *name = "SERIAL_S";
            return true;
        case 'r':
        case 'R':
            action = lofi::Action::Repeat;
            *name = "SERIAL_R";
            return true;
        case 'l':
        case 'L':
            action = lofi::Action::Lofi;
            *name = "SERIAL_L";
            return true;
        case 'm':
        case 'M':
            action = lofi::Action::Menu;
            *name = "SERIAL_M";
            return true;
        case 'b':
        case 'B':
            action = lofi::Action::Back;
            *name = "SERIAL_B";
            return true;
        case 'h':
        case 'H':
            action = lofi::Action::Home;
            *name = "SERIAL_HOME";
            return true;
        case '?':
            action = lofi::Action::Help;
            *name = "SERIAL_HELP";
            return true;
        case 'g':
        case 'G':
            action = lofi::Action::Scan;
            *name = "SERIAL_SCAN";
            return true;
        case '[':
        case ',':
            action = lofi::Action::Left;
            *name = "SERIAL_LEFT";
            return true;
        case ']':
        case '.':
            action = lofi::Action::Right;
            *name = "SERIAL_RIGHT";
            return true;
        case '<':
            action = lofi::Action::SeekBackward;
            *name = "SERIAL_SEEK_BACK";
            return true;
        case '>':
            action = lofi::Action::SeekForward;
            *name = "SERIAL_SEEK_FWD";
            return true;
        case 'd':
        case 'D':
            action = lofi::Action::None;
            *name = "SERIAL_FRAME_DUMP";
            frame_dump = true;
            return true;
        case '+':
        case '=':
            action = lofi::Action::Up;
            *name = "SERIAL_PLUS";
            return true;
        case '-':
        case '_':
            action = lofi::Action::Down;
            *name = "SERIAL_MINUS";
            return true;
        case 't':
        case 'T':
            action = lofi::Action::None;
            *name = "SERIAL_WAV_SELFTEST";
            wav_selftest = true;
            return true;
        case 'y':
        case 'Y':
            action = lofi::Action::None;
            *name = "SERIAL_MP3_SELFTEST";
            mp3_selftest = true;
            return true;
        default:
            break;
        }
    }
    if (saw_input) {
        ESP_LOGI(TAG, "SERIAL_INPUT ignored");
    }
    return false;
}

void log_runtime_status(const lofi::LibraryIndex &library, const lofi::PlaybackState &playback,
                        const lofi::UiState &ui, bool sd_mounted, bool state_saved)
{
    const lofi_audio::Snapshot audio = lofi_audio::snapshot();
    const lofi_board::KeyboardDiagnostics keyboard = lofi_board::keyboard_diagnostics();
    log_media_format_counts(library);
    ESP_LOGI(TAG,
             "LOFI_STATUS tick=%u page=%s nav=%u tracks=%u albums=%u artists=%u sd=%d playing=%d current=%d pos=%d "
             "queue=%u state=%d kbd=%d kbd_stage=%s kbd_err=%s bus_err=%s probe_err=%s audio=%s audio_pos=%d msg=%s",
             static_cast<unsigned>(xTaskGetTickCount()),
             lofi::to_string(ui.page),
             static_cast<unsigned>(ui.back_stack.size()),
             static_cast<unsigned>(library.tracks.size()),
             static_cast<unsigned>(library.albums.size()),
             static_cast<unsigned>(library.artists.size()),
             sd_mounted ? 1 : 0,
             playback.playing ? 1 : 0,
             playback.current_track,
             playback.position_seconds,
             static_cast<unsigned>(playback.queue.track_indices.size()),
             state_saved ? 1 : 0,
             keyboard.ready ? 1 : 0,
             keyboard.stage,
             esp_err_to_name(keyboard.init_err),
             esp_err_to_name(keyboard.bus_err),
             esp_err_to_name(keyboard.probe_err),
             audio_status_name(audio.status),
             audio.position_seconds,
             audio.message);
}

void log_i2c_probe_summary()
{
    const esp_err_t keyboard = lofi_board::probe_i2c_device(I2C_ADDR_TCA8418);
    const esp_err_t imu = lofi_board::probe_i2c_device(I2C_ADDR_BMI270);
    const esp_err_t codec = lofi_board::probe_i2c_device(I2C_ADDR_ES8311);
    ESP_LOGI(TAG, "I2C_PROBE tca8418=%s bmi270=%s es8311=%s",
             esp_err_to_name(keyboard),
             esp_err_to_name(imu),
             esp_err_to_name(codec));
}

void sync_audio(lofi::LibraryIndex &library, lofi::PlaybackState &playback, lofi::UiState &ui,
                int &active_track, bool &audio_loaded, bool &last_playing, lofi::LofiProfile &active_profile,
                int requested_seek_seconds)
{
    const lofi_audio::Snapshot snapshot = lofi_audio::snapshot();
    int current = lofi::queue_current_track(playback.queue);
    const bool same_loaded_track = audio_loaded && active_track == current;
    if (playback.playing && audio_loaded &&
        (snapshot.status == lofi_audio::Status::Unsupported || snapshot.status == lofi_audio::Status::Error)) {
        playback.playing = false;
        audio_loaded = false;
        active_track = -1;
        last_playing = false;
        ui.toast = snapshot.status == lofi_audio::Status::Unsupported ? "Decode failed" : "Audio error";
        return;
    }
    if (requested_seek_seconds >= 0) {
        playback.position_seconds = requested_seek_seconds;
    } else if (same_loaded_track && should_sync_audio_position(audio_loaded, snapshot.status)) {
        playback.position_seconds = snapshot.position_seconds;
    }

    if (snapshot.status == lofi_audio::Status::Ended && playback.playing) {
        playback.current_track = lofi::queue_next(playback.queue, playback.repeat);
        if (playback.current_track < 0) {
            playback.playing = false;
            audio_loaded = false;
            active_track = -1;
            ui.toast = "Queue ended";
        } else {
            playback.position_seconds = 0;
            audio_loaded = false;
            active_track = -1;
        }
    }

    if (!playback.playing) {
        if (requested_seek_seconds >= 0 && audio_loaded && current >= 0 &&
            static_cast<size_t>(current) < library.tracks.size()) {
            lofi_audio::seek(requested_seek_seconds);
        }
        if (last_playing && should_pause_active_audio(snapshot.status)) {
            lofi_audio::set_paused(true);
        }
        last_playing = false;
        return;
    }
    if (current < 0 || static_cast<size_t>(current) >= library.tracks.size()) {
        playback.playing = false;
        last_playing = false;
        return;
    }
    if (!track_is_playable(library.tracks[static_cast<size_t>(current)])) {
        if (!advance_to_next_playable(library, playback)) {
            playback.playing = false;
            audio_loaded = false;
            active_track = -1;
            ui.toast = "No playable in queue";
            return;
        }
        current = lofi::queue_current_track(playback.queue);
        audio_loaded = false;
        active_track = -1;
        ui.toast = "Skipped unsupported";
    }

    const int audio_volume = lofi::audio_volume_from_user_percent(playback.volume);
    lofi_audio::set_volume(audio_volume);
    if (!audio_loaded || active_track != current) {
        lofi::Track &track = library.tracks[static_cast<size_t>(current)];
        if (audio_loaded && active_track != current) {
            lofi_audio::stop();
            vTaskDelay(pdMS_TO_TICKS(60));
            audio_loaded = false;
            active_track = -1;
        }
        lofi_board::release_album_art_cache();
        lofi_board::set_framebuffer_capture_enabled(false);
        ensure_track_duration(track);
        ensure_track_album_art_cache(track, true);
        lofi::Track playback_track = track;
        const std::string audio_cache_path = existing_audio_cache_path_for(track);
        if (!audio_cache_path.empty()) {
            playback_track.path = audio_cache_path;
            playback_track.format = "aac";
            ESP_LOGI(TAG, "AAC_CACHE use cache=%s src=%s", audio_cache_path.c_str(), track.path.c_str());
        }
        esp_err_t err = lofi_audio::play_track(playback_track, audio_volume, playback.lofi, playback.position_seconds);
        if (err == ESP_ERR_NOT_SUPPORTED) {
            playback.playing = false;
            audio_loaded = false;
            active_track = -1;
            ui.toast = "Unsupported " + track.format;
            return;
        }
        if (err != ESP_OK) {
            playback.playing = false;
            audio_loaded = false;
            active_track = -1;
            ui.toast = "Audio error";
            return;
        }
        active_track = current;
        active_profile = playback.lofi;
        audio_loaded = true;
    } else if (requested_seek_seconds >= 0) {
        lofi_audio::seek(requested_seek_seconds);
    } else {
        if (!lofi_profiles_equal(active_profile, playback.lofi)) {
            lofi_audio::set_profile(playback.lofi);
            active_profile = playback.lofi;
        }
        if (!last_playing) {
            lofi_audio::set_paused(false);
        }
    }
    last_playing = true;
}

bool save_playback_state_if_possible(bool sd_mounted, const lofi::PlaybackState &playback)
{
    if (!sd_mounted) {
        return false;
    }
    if (!lofi::ensure_directory(kStateDir)) {
        ESP_LOGW(TAG, "Failed to create state dir");
        return false;
    }
    const bool ok = lofi::write_text_file(kStateFile, lofi::serialize_playback_state(playback));
    if (!ok) {
        ESP_LOGW(TAG, "Failed to write playback state");
    }
    return ok;
}

bool save_queue_snapshot_if_possible(bool sd_mounted,
                                     const lofi::LibraryIndex &library,
                                     const lofi::PlaybackState &playback)
{
    if (!sd_mounted) {
        return false;
    }
    if (!lofi::ensure_directory(kStateDir)) {
        ESP_LOGW(TAG, "Failed to create state dir");
        return false;
    }
    const bool ok = lofi::write_text_file(kQueueFile, lofi::serialize_queue_snapshot(library, playback));
    if (!ok) {
        ESP_LOGW(TAG, "Failed to write queue snapshot");
    }
    return ok;
}

void rebuild_library(bool sd_mounted, lofi::LibraryIndex &library, lofi::PlaybackState &playback,
                     lofi::UiState &ui, bool resume_playing, bool show_toast)
{
    const LibraryScanResult scan = scan_library(sd_mounted);
    library = scan.library;

    if (!playback.queue.track_indices.empty()) {
        lofi::PlaybackState restored = playback;
        if (lofi::restore_playback_queue(library, restored, resume_playing && playback.playing)) {
            playback = restored;
        } else {
            playback.queue = lofi::Queue{};
            playback.current_track = -1;
            playback.position_seconds = 0;
            playback.playing = false;
        }
    }

    ui.selected = 0;
    ui.scroll = 0;
    ui.back_stack.clear();
    ui.page = library.tracks.empty() ? lofi::Page::Empty : lofi::Page::LibraryHome;
    if (show_toast || scan.using_samples) {
        ui.toast = scan.using_samples ? "Using samples" : "Rescan done";
    }
}

bool load_playback_state_if_possible(bool sd_mounted, const lofi::LibraryIndex &library, lofi::PlaybackState &playback,
                                     lofi::UiState &ui)
{
    if (!sd_mounted) {
        return false;
    }
    std::string text;
    if (!lofi::read_text_file(kStateFile, text)) {
        return false;
    }

    lofi::PlaybackState restored;
    if (!lofi::parse_playback_state(text, restored)) {
        return false;
    }
    const lofi::PlaybackRestoreResult result = lofi::restore_saved_playback_state(library, restored, playback, false);
    ui.toast = result.queue_restored ? "Restored" : "Settings restored";
    ESP_LOGI(TAG,
             "STATE_LOAD ok path=%s settings=%d queue=%d volume=%d brightness=%d sleep=%d",
             kStateFile,
             result.settings_restored ? 1 : 0,
             result.queue_restored ? 1 : 0,
             playback.volume,
             playback.brightness_percent,
             playback.screen_off_seconds);
    return true;
}

bool load_queue_snapshot_if_possible(bool sd_mounted, const lofi::LibraryIndex &library, lofi::PlaybackState &playback,
                                     lofi::UiState &ui)
{
    if (!sd_mounted) {
        return false;
    }
    std::string text;
    if (!lofi::read_text_file(kQueueFile, text)) {
        return false;
    }

    const lofi::QueueSnapshotRestoreResult result = lofi::restore_queue_snapshot(library, text, playback, false);
    ESP_LOGI(TAG,
             "QUEUE_LOAD path=%s restored=%d current=%d saved=%u matched=%u missing=%u source=%s",
             kQueueFile,
             result.restored ? 1 : 0,
             result.current_restored ? 1 : 0,
             static_cast<unsigned>(result.saved_count),
             static_cast<unsigned>(result.restored_count),
             static_cast<unsigned>(result.missing_count),
             playback.queue.source_type.c_str());
    if (!result.restored) {
        return false;
    }
    ui.toast = "Queue restored";
    return true;
}

size_t find_track_by_path(const lofi::LibraryIndex &library, const std::string &path)
{
    for (size_t i = 0; i < library.tracks.size(); ++i) {
        if (library.tracks[i].path == path) {
            return i;
        }
    }
    return static_cast<size_t>(-1);
}

std::vector<std::string> queue_paths_for_library(const lofi::LibraryIndex &library, const lofi::Queue &queue)
{
    std::vector<std::string> paths;
    paths.reserve(queue.track_indices.size());
    for (size_t track_index : queue.track_indices) {
        if (track_index < library.tracks.size()) {
            paths.push_back(library.tracks[track_index].path);
        }
    }
    return paths;
}

bool restore_selection_queue_by_paths(const lofi::LibraryIndex &library,
                                      lofi::PlaybackState &playback,
                                      const lofi::Queue &previous_queue,
                                      const std::vector<std::string> &previous_paths,
                                      const std::string &previous_current_path,
                                      bool resume_playing)
{
    lofi::Queue restored = previous_queue;
    restored.track_indices.clear();
    size_t restored_current_index = 0;
    bool found_current = false;

    for (const std::string &path : previous_paths) {
        const size_t track_index = find_track_by_path(library, path);
        if (track_index == static_cast<size_t>(-1)) {
            continue;
        }
        if (!previous_current_path.empty() && path == previous_current_path) {
            restored_current_index = restored.track_indices.size();
            found_current = true;
        }
        restored.track_indices.push_back(track_index);
    }
    if (restored.track_indices.empty()) {
        return false;
    }
    if (found_current) {
        restored.current_index = restored_current_index;
    } else if (restored.current_index >= restored.track_indices.size()) {
        restored.current_index = restored.track_indices.size() - 1;
    }

    playback.queue = restored;
    playback.current_track = lofi::queue_current_track(playback.queue);
    playback.playing = resume_playing && playback.current_track >= 0;
    if (!found_current && !previous_current_path.empty()) {
        playback.position_seconds = 0;
    }
    return playback.current_track >= 0;
}

void preserve_current_track_by_path(const lofi::LibraryIndex &library,
                                    lofi::PlaybackState &playback,
                                    const std::string &previous_path)
{
    if (previous_path.empty()) {
        return;
    }
    const size_t track_index = find_track_by_path(library, previous_path);
    if (track_index == static_cast<size_t>(-1)) {
        return;
    }
    const auto it = std::find(playback.queue.track_indices.begin(),
                              playback.queue.track_indices.end(),
                              track_index);
    if (it != playback.queue.track_indices.end()) {
        playback.queue.current_index = static_cast<size_t>(it - playback.queue.track_indices.begin());
    }
    playback.current_track = static_cast<int>(track_index);
}

void apply_library_scan_result(LibraryScanResult *result,
                               lofi::LibraryIndex &library,
                               lofi::PlaybackState &playback,
                               lofi::UiState &ui,
                               int &active_track,
                               bool &audio_loaded)
{
    if (!result) {
        return;
    }

    const bool was_playing = playback.playing;
    const lofi::Queue previous_queue = playback.queue;
    const std::vector<std::string> previous_queue_paths = queue_paths_for_library(library, playback.queue);
    std::string previous_path;
    const int previous_current_track = lofi::queue_current_track(playback.queue);
    if (previous_current_track >= 0 &&
        static_cast<size_t>(previous_current_track) < library.tracks.size()) {
        previous_path = library.tracks[static_cast<size_t>(previous_current_track)].path;
    }

    library = std::move(result->library);
    if (!playback.queue.track_indices.empty()) {
        bool restored_ok = false;
        if (previous_queue.source_type == "selection") {
            restored_ok = restore_selection_queue_by_paths(library,
                                                           playback,
                                                           previous_queue,
                                                           previous_queue_paths,
                                                           previous_path,
                                                           was_playing);
        } else {
            lofi::PlaybackState restored = playback;
            if (lofi::restore_playback_queue(library, restored, was_playing)) {
                playback = restored;
                preserve_current_track_by_path(library, playback, previous_path);
                restored_ok = true;
            }
        }
        if (!restored_ok) {
            playback.queue = lofi::Queue{};
            playback.current_track = -1;
            playback.position_seconds = 0;
            playback.playing = false;
        }
    }

    if (active_track >= 0 && active_track != playback.current_track) {
        audio_loaded = false;
    }

    if (ui.page == lofi::Page::Scan || (ui.page == lofi::Page::Empty && !library.tracks.empty())) {
        ui.page = library.tracks.empty() ? lofi::Page::Empty : lofi::Page::LibraryHome;
        ui.selected = 0;
        ui.scroll = 0;
        ui.back_stack.clear();
    }
    ui.toast = result->using_samples ? "Using samples" : "Library synced";
    ESP_LOGI(TAG,
             "LIBRARY_SYNC applied source=%s tracks=%u albums=%u artists=%u",
             result->from_cache ? "cache" : "scan",
             static_cast<unsigned>(library.tracks.size()),
             static_cast<unsigned>(library.albums.size()),
             static_cast<unsigned>(library.artists.size()));
}

void background_library_scan_task(void *arg)
{
    LibraryScanTaskContext *context = static_cast<LibraryScanTaskContext *>(arg);
    QueueHandle_t result_queue = context ? context->result_queue : nullptr;
    const bool sd_mounted = context && context->sd_mounted;
    delete context;

    LibraryScanResult *result = new (std::nothrow) LibraryScanResult(scan_library(sd_mounted));
    if (!result) {
        ESP_LOGW(TAG, "LIBRARY_SYNC failed to allocate result");
        if (result_queue) {
            LibraryScanResult *empty_result = nullptr;
            xQueueSend(result_queue, &empty_result, 0);
        }
        vTaskDelete(nullptr);
        return;
    }
    if (!result_queue || xQueueSend(result_queue, &result, 0) != pdTRUE) {
        ESP_LOGW(TAG, "LIBRARY_SYNC dropped result");
        delete result;
    } else {
        ESP_LOGI(TAG, "LIBRARY_SYNC ready tracks=%u", static_cast<unsigned>(result->library.tracks.size()));
    }
    vTaskDelete(nullptr);
}

QueueHandle_t start_background_library_scan(bool sd_mounted)
{
    if (!sd_mounted) {
        return nullptr;
    }
    QueueHandle_t result_queue = xQueueCreate(1, sizeof(LibraryScanResult *));
    if (!result_queue) {
        ESP_LOGW(TAG, "LIBRARY_SYNC queue allocation failed");
        return nullptr;
    }
    LibraryScanTaskContext *context = new (std::nothrow) LibraryScanTaskContext{sd_mounted, result_queue};
    if (!context) {
        ESP_LOGW(TAG, "LIBRARY_SYNC context allocation failed");
        vQueueDelete(result_queue);
        return nullptr;
    }
    if (xTaskCreate(background_library_scan_task,
                    "library_scan",
                    8192,
                    context,
                    tskIDLE_PRIORITY + 1,
                    nullptr) != pdPASS) {
        ESP_LOGW(TAG, "LIBRARY_SYNC task start failed");
        delete context;
        vQueueDelete(result_queue);
        return nullptr;
    }
    ESP_LOGI(TAG, "LIBRARY_SYNC started");
    return result_queue;
}

} // namespace

extern "C" void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_LOGI(TAG, "Cardputer Adv Lo-Fi client boot");
    ESP_LOGI(TAG, "Uses: LCD ST7789, TCA8418 keyboard, microSD, ES8311/I2S audio");
    ESP_LOGI(TAG, "Shared buses: I2C G8/G9, SD/EXT SPI G14/G39/G40");
    configure_serial_debug_input();

    esp_err_t display_err = lofi_board::init_display();
    if (display_err != ESP_OK) {
        ESP_LOGE(TAG, "Display init failed: %s", esp_err_to_name(display_err));
    }

    esp_err_t audio_err = lofi_audio::init();
    if (audio_err != ESP_OK) {
        ESP_LOGW(TAG, "Audio init failed: %s", esp_err_to_name(audio_err));
    }

    lofi::PlaybackState playback;
    lofi::UiState ui;
    ui.page = lofi::Page::Scan;
    lofi::LibraryIndex boot_index;
    if (display_err == ESP_OK) {
        lofi_board::draw_screen(lofi::render_screen(boot_index, playback, ui));
    }

    sdmmc_card_t *card = nullptr;
    bool sd_mounted = false;
    if (mount_sdcard(&card)) {
        sd_mounted = true;
    }

    lofi::LibraryIndex library;
    bool cache_loaded = false;
    bool start_background_scan = false;
    const std::vector<std::string> cached_paths = load_cached_music_paths(sd_mounted, &cache_loaded);
    if (cache_loaded && !cached_paths.empty()) {
        LibraryScanResult cached = build_library_from_paths(cached_paths, false, true, false);
        library = std::move(cached.library);
        ui.page = library.tracks.empty() ? lofi::Page::Empty : lofi::Page::LibraryHome;
        ui.toast = "Library cached";
        start_background_scan = true;
        ESP_LOGI(TAG, "LIBRARY_BOOT source=cache tracks=%u", static_cast<unsigned>(library.tracks.size()));
    } else {
        const LibraryScanResult scanned = scan_library(sd_mounted);
        library = scanned.library;
        ui.page = library.tracks.empty() ? lofi::Page::Empty : lofi::Page::LibraryHome;
        if (scanned.using_samples) {
            ui.toast = "Using samples";
        }
        ESP_LOGI(TAG,
                 "LIBRARY_BOOT source=%s tracks=%u",
                 scanned.using_samples ? "samples" : "scan",
                 static_cast<unsigned>(library.tracks.size()));
    }

    load_playback_state_if_possible(sd_mounted, library, playback, ui);
    load_queue_snapshot_if_possible(sd_mounted, library, playback, ui);
    if (display_err == ESP_OK) {
        lofi_board::set_screen_brightness_percent(playback.brightness_percent);
    }

    esp_err_t keyboard_err = lofi_board::init_keyboard();
    if (keyboard_err != ESP_OK) {
        ESP_LOGW(TAG, "Keyboard init failed: %s", esp_err_to_name(keyboard_err));
        ui.toast = "Keyboard unavailable";
    }
    log_i2c_probe_summary();

    lofi::ScreenModel screen = lofi::render_screen(library, playback, ui);
    log_screen(screen);
    if (display_err == ESP_OK) {
        lofi_board::draw_screen(screen);
    }

    int active_track = -1;
    bool audio_loaded = false;
    bool last_playing = false;
    bool library_scan_pending = start_background_scan;
    QueueHandle_t library_scan_queue = nullptr;
    QueueHandle_t audio_cache_queue = nullptr;
    LibraryScanResult *deferred_library_scan_result = nullptr;
    lofi::LofiProfile active_profile;
    TickType_t last_redraw = xTaskGetTickCount();
    TickType_t last_state_save = xTaskGetTickCount();
    TickType_t last_status_log = xTaskGetTickCount();
    TickType_t last_keyboard_retry = xTaskGetTickCount();
    TickType_t last_user_activity = xTaskGetTickCount();
    TickType_t last_audio_cache_attempt = xTaskGetTickCount();
    TickType_t volume_overlay_until = 0;
    TickType_t mode_overlay_until = 0;
    std::string mode_overlay_kind;
    std::string mode_overlay_title;
    std::string mode_overlay_value;
    size_t audio_cache_cursor = 0;
    bool last_background_task_active = false;
    uint8_t last_background_task_frame = 0;
    int last_saved_position = playback.position_seconds;
    bool state_saved = false;
    log_runtime_status(library, playback, ui, sd_mounted, state_saved);
    if (save_playback_state_if_possible(sd_mounted, playback)) {
        state_saved = true;
        ESP_LOGI(TAG, "STATE_SAVE boot ok path=%s", kStateFile);
        last_saved_position = playback.position_seconds;
    }
    if (save_queue_snapshot_if_possible(sd_mounted, library, playback)) {
        ESP_LOGI(TAG, "QUEUE_SAVE boot ok path=%s count=%u", kQueueFile,
                 static_cast<unsigned>(playback.queue.track_indices.size()));
    }

    while (true) {
        bool needs_redraw = false;
        bool needs_screen_log = false;
        bool needs_state_save = false;
        int requested_seek_seconds = -1;
        lofi::Action action = lofi::Action::None;
        const char *key_name = nullptr;
        bool frame_dump = false;
        bool wav_selftest = false;
        bool mp3_selftest = false;
        const TickType_t now_ticks = xTaskGetTickCount();
        const bool library_sync_idle = sd_mounted && !playback.playing && !audio_loaded &&
                                       now_ticks - last_user_activity >= kLibrarySyncIdleDelay;
        const bool audio_cache_idle = sd_mounted && !playback.playing && !audio_loaded &&
                                      !library_scan_queue && !deferred_library_scan_result &&
                                      now_ticks - last_user_activity >= kAudioCacheIdleDelay;
        if (audio_cache_queue && !audio_cache_idle) {
            g_audio_cache_cancel_requested = true;
        }
        if (library_scan_pending && !library_scan_queue && !deferred_library_scan_result && library_sync_idle) {
            library_scan_queue = start_background_library_scan(sd_mounted);
            library_scan_pending = library_scan_queue == nullptr;
            if (library_scan_queue) {
                ui.toast = "Syncing library";
                needs_redraw = true;
                needs_screen_log = true;
            }
        }
        if (library_scan_queue) {
            LibraryScanResult *scan_result = nullptr;
            if (xQueueReceive(library_scan_queue, &scan_result, 0) == pdTRUE) {
                vQueueDelete(library_scan_queue);
                library_scan_queue = nullptr;
                if (scan_result && library_sync_idle) {
                    apply_library_scan_result(scan_result, library, playback, ui, active_track, audio_loaded);
                    delete scan_result;
                    save_queue_snapshot_if_possible(sd_mounted, library, playback);
                    needs_redraw = true;
                    needs_screen_log = true;
                } else if (scan_result) {
                    ESP_LOGI(TAG,
                             "LIBRARY_SYNC defer apply playing=%d audio_loaded=%d",
                             playback.playing ? 1 : 0,
                             audio_loaded ? 1 : 0);
                    deferred_library_scan_result = scan_result;
                }
            }
        }
        if (deferred_library_scan_result && library_sync_idle) {
            apply_library_scan_result(deferred_library_scan_result, library, playback, ui, active_track, audio_loaded);
            delete deferred_library_scan_result;
            deferred_library_scan_result = nullptr;
            save_queue_snapshot_if_possible(sd_mounted, library, playback);
            needs_redraw = true;
            needs_screen_log = true;
        }
        if (audio_cache_queue) {
            AudioCacheResult *cache_result = nullptr;
            if (xQueueReceive(audio_cache_queue, &cache_result, 0) == pdTRUE) {
                vQueueDelete(audio_cache_queue);
                audio_cache_queue = nullptr;
                if (cache_result) {
                    ESP_LOGI(TAG,
                             "AAC_CACHE task complete attempted=%d built=%d",
                             cache_result->attempted ? 1 : 0,
                             cache_result->built ? 1 : 0);
                    delete cache_result;
                } else {
                    ESP_LOGW(TAG, "AAC_CACHE task returned no result");
                }
            }
        }
        if (!audio_cache_queue && audio_cache_idle && now_ticks - last_audio_cache_attempt >= kAudioCacheAttemptInterval) {
            start_next_audio_cache_candidate(library, audio_cache_cursor, audio_cache_queue);
            last_audio_cache_attempt = xTaskGetTickCount();
        }
        const bool background_task_active = library_scan_queue || audio_cache_queue;
        const uint8_t background_task_frame =
            background_task_active ? static_cast<uint8_t>((xTaskGetTickCount() / pdMS_TO_TICKS(180)) % 4) : 0;
        if (background_task_active != last_background_task_active) {
            needs_redraw = true;
            needs_screen_log = true;
        } else if (background_task_active && background_task_frame != last_background_task_frame) {
            needs_redraw = true;
        }
        if (lofi_board::poll_action(action, &key_name) ||
            poll_serial_action(action, &key_name, frame_dump, wav_selftest, mp3_selftest)) {
            last_user_activity = xTaskGetTickCount();
            if (audio_cache_queue) {
                g_audio_cache_cancel_requested = true;
            }
            const bool wake_only = display_err == ESP_OK && !lofi_board::screen_awake();
            if (wake_only) {
                lofi_board::set_screen_awake(true);
                ESP_LOGI(TAG, "DISPLAY_WAKE key=%s", key_name ? key_name : "?");
                needs_redraw = true;
                needs_screen_log = true;
            }
            ESP_LOGI(TAG, "Key %s -> action %d", key_name ? key_name : "?", static_cast<int>(action));
            const int previous_brightness = playback.brightness_percent;
            if (wake_only) {
                // First input after screen-off only wakes the display to avoid accidental actions.
            } else if (frame_dump) {
                lofi_board::set_framebuffer_capture_enabled(true);
                if (display_err == ESP_OK) {
                    screen = lofi::render_screen(library, playback, ui);
                    set_background_task_indicator(screen, background_task_active, xTaskGetTickCount());
                    set_volume_overlay(screen,
                                       ui.page == lofi::Page::NowPlaying && volume_overlay_until != 0 &&
                                           xTaskGetTickCount() < volume_overlay_until,
                                       playback.volume);
                    set_mode_overlay(screen,
                                     ui.page == lofi::Page::NowPlaying && mode_overlay_until != 0 &&
                                         xTaskGetTickCount() < mode_overlay_until,
                                     mode_overlay_kind,
                                     mode_overlay_title,
                                     mode_overlay_value);
                    lofi_board::draw_screen(screen);
                }
                lofi_board::dump_framebuffer_to_serial();
                if (playback.playing) {
                    lofi_board::set_framebuffer_capture_enabled(false);
                }
            } else if (wav_selftest || mp3_selftest) {
                lofi_audio::stop();
                audio_loaded = false;
                active_track = -1;
                last_playing = false;
                playback.playing = false;
                if (wav_selftest && start_wav_selftest(sd_mounted, playback.volume, playback.lofi)) {
                    ui.toast = "WAV self-test";
                } else if (mp3_selftest && start_mp3_selftest(sd_mounted, playback.volume, playback.lofi)) {
                    ui.toast = "MP3 self-test";
                } else {
                    ui.toast = wav_selftest ? "WAV self-test failed" : "MP3 self-test failed";
                }
            } else if (ui.page == lofi::Page::Scan && (action == lofi::Action::Ok ||
                                                       action == lofi::Action::PlayPause)) {
                lofi_audio::stop();
                audio_loaded = false;
                active_track = -1;
                last_playing = false;
                rebuild_library(sd_mounted, library, playback, ui, false, true);
            } else {
                lofi::apply_action(library, playback, ui, action);
                if (action == lofi::Action::SeekForward || action == lofi::Action::SeekBackward) {
                    requested_seek_seconds = playback.position_seconds;
                }
                if (ui.page == lofi::Page::NowPlaying &&
                    (action == lofi::Action::Up || action == lofi::Action::Down)) {
                    volume_overlay_until = xTaskGetTickCount() + pdMS_TO_TICKS(2000);
                }
                if (ui.page == lofi::Page::NowPlaying && action == lofi::Action::Repeat) {
                    mode_overlay_until = xTaskGetTickCount() + pdMS_TO_TICKS(2000);
                    mode_overlay_kind = "repeat";
                    mode_overlay_title = "REPEAT";
                    mode_overlay_value = lofi::to_string(playback.repeat);
                } else if (ui.page == lofi::Page::NowPlaying && action == lofi::Action::Shuffle) {
                    mode_overlay_until = xTaskGetTickCount() + pdMS_TO_TICKS(2000);
                    mode_overlay_kind = "shuffle";
                    mode_overlay_title = "SHUFFLE";
                    mode_overlay_value = playback.queue.shuffle ? "On" : "Off";
                }
            }
            if (display_err == ESP_OK && previous_brightness != playback.brightness_percent) {
                lofi_board::set_screen_brightness_percent(playback.brightness_percent);
            }
            needs_redraw = true;
            needs_screen_log = true;
            needs_state_save = true;
        }

        const int previous_position = playback.position_seconds;
        const bool previous_playing = playback.playing;
        const std::string previous_toast = ui.toast;
        sync_audio(library, playback, ui, active_track, audio_loaded, last_playing, active_profile, requested_seek_seconds);
        if (previous_playing != playback.playing || previous_toast != ui.toast) {
            needs_redraw = true;
            needs_screen_log = true;
        }
        if (ensure_current_track_duration(library, playback)) {
            needs_redraw = true;
            needs_screen_log = true;
        }
        if (ensure_current_album_art_cache(library, playback)) {
            needs_redraw = true;
            needs_screen_log = true;
        }
        const bool animating_screen = background_task_active || ui.page == lofi::Page::NowPlaying ||
                                      ui.page == lofi::Page::Queue || ui.page == lofi::Page::Songs ||
                                      ui.page == lofi::Page::AlbumDetail;
        const TickType_t redraw_interval = animating_screen ? pdMS_TO_TICKS(70)
                                                            : (playback.playing ? pdMS_TO_TICKS(250)
                                                                                : pdMS_TO_TICKS(750));
        if (previous_position != playback.position_seconds) {
            needs_redraw = true;
            needs_screen_log = true;
        } else if (animating_screen && xTaskGetTickCount() - last_redraw > redraw_interval) {
            needs_redraw = true;
        }
        if (ui.page == lofi::Page::NowPlaying && volume_overlay_until != 0 &&
            xTaskGetTickCount() >= volume_overlay_until) {
            volume_overlay_until = 0;
            needs_redraw = true;
            needs_screen_log = true;
        }
        if (mode_overlay_until != 0 && xTaskGetTickCount() >= mode_overlay_until) {
            mode_overlay_until = 0;
            needs_redraw = true;
            needs_screen_log = true;
        }
        if (playback.position_seconds != last_saved_position &&
            xTaskGetTickCount() - last_state_save > (playback.playing ? pdMS_TO_TICKS(30000) : pdMS_TO_TICKS(5000))) {
            needs_state_save = true;
        }
        if (keyboard_err != ESP_OK && xTaskGetTickCount() - last_keyboard_retry > pdMS_TO_TICKS(5000)) {
            keyboard_err = lofi_board::init_keyboard();
            ESP_LOGI(TAG, "KEYBOARD_RETRY err=%s", esp_err_to_name(keyboard_err));
            if (keyboard_err == ESP_OK && ui.toast == "Keyboard unavailable") {
                ui.toast = "Keyboard ready";
                needs_redraw = true;
                needs_screen_log = true;
            }
            last_keyboard_retry = xTaskGetTickCount();
        }

        if (needs_redraw) {
            screen = lofi::render_screen(library, playback, ui);
            set_background_task_indicator(screen, background_task_active, xTaskGetTickCount());
            set_volume_overlay(screen,
                               ui.page == lofi::Page::NowPlaying && volume_overlay_until != 0 &&
                                   xTaskGetTickCount() < volume_overlay_until,
                               playback.volume);
            set_mode_overlay(screen,
                             ui.page == lofi::Page::NowPlaying && mode_overlay_until != 0 &&
                                 xTaskGetTickCount() < mode_overlay_until,
                             mode_overlay_kind,
                             mode_overlay_title,
                             mode_overlay_value);
            if (needs_screen_log) {
                log_screen(screen);
            }
            if (display_err == ESP_OK) {
                lofi_board::draw_screen(screen);
            }
            last_background_task_active = screen.background_task_active;
            last_background_task_frame = screen.background_task_frame;
            last_redraw = xTaskGetTickCount();
        }
        if (needs_state_save && save_playback_state_if_possible(sd_mounted, playback)) {
            state_saved = true;
            last_state_save = xTaskGetTickCount();
            last_saved_position = playback.position_seconds;
            save_queue_snapshot_if_possible(sd_mounted, library, playback);
        }
        if (xTaskGetTickCount() - last_status_log > pdMS_TO_TICKS(5000)) {
            log_runtime_status(library, playback, ui, sd_mounted, state_saved);
            last_status_log = xTaskGetTickCount();
        }
        if (display_err == ESP_OK && lofi_board::screen_awake() && playback.screen_off_seconds > 0 &&
            xTaskGetTickCount() - last_user_activity >= pdMS_TO_TICKS(playback.screen_off_seconds * 1000)) {
            lofi_board::set_screen_awake(false);
            ESP_LOGI(TAG, "DISPLAY_SLEEP timeout=%d", playback.screen_off_seconds);
        }
        lofi_board::tick_display();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
