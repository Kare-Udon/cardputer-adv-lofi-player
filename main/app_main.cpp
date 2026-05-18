#include "lofi_core.hpp"
#include "lofi_audio.hpp"
#include "lofi_board.hpp"
#include "lofi_selftest_mp3.hpp"
#include "lofi_storage.hpp"
#include "lofi_wav.hpp"

#include "board_pins.h"
#include "driver/sdspi_host.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdmmc_cmd.h"

#include <cerrno>
#include <cstring>
#include <cstdio>
#include <cstdint>
#include <fcntl.h>
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
constexpr const char *kIndexFile = "/sdcard/Music/LOFI/INDEX.TXT";
constexpr const char *kSelfTestWavFile = "/sdcard/Music/LOFI/SELFTEST.WAV";
constexpr const char *kSelfTestMp3File = "/sdcard/Music/LOFI/SELFTEST.MP3";

struct MediaFormatCounts {
    size_t mp3 = 0;
    size_t wav = 0;
    size_t m4a_aac = 0;
    size_t other = 0;
};

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
        if (track.format == "m4a" || track.format == "aac") {
            track.duration_seconds = estimate_m4a_duration_seconds(track.path);
        } else if (track.format == "wav") {
            track.duration_seconds = estimate_wav_duration_seconds(track.path);
        }
        if (track.duration_seconds > 0) {
            ++known;
        }
    }
    ESP_LOGI(TAG, "DURATION_INDEX known=%u total=%u",
             static_cast<unsigned>(known),
             static_cast<unsigned>(library.tracks.size()));
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
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
        ESP_LOGI(TAG, "SERIAL_INPUT ready enter=ok space=play n=next p=prev s=shuffle r=repeat l=lofi m=menu b=back [=left ]=right <=seek_back >=seek_forward c=color_test d=frame_dump t=wav_selftest y=mp3_selftest");
    }
}

bool poll_serial_action(lofi::Action &action,
                        const char **name,
                        bool &color_test,
                        bool &frame_dump,
                        bool &wav_selftest,
                        bool &mp3_selftest)
{
    bool saw_input = false;
    color_test = false;
    frame_dump = false;
    wav_selftest = false;
    mp3_selftest = false;
    while (true) {
        errno = 0;
        const int ch = getchar();
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
        case 'c':
        case 'C':
            action = lofi::Action::None;
            *name = "SERIAL_COLOR_TEST";
            color_test = true;
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
             "LOFI_STATUS tick=%u page=%s tracks=%u albums=%u artists=%u sd=%d playing=%d current=%d pos=%d "
             "queue=%u state=%d kbd=%d kbd_stage=%s kbd_err=%s bus_err=%s probe_err=%s audio=%s audio_pos=%d msg=%s",
             static_cast<unsigned>(xTaskGetTickCount()),
             lofi::to_string(ui.page),
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

void sync_audio(const lofi::LibraryIndex &library, lofi::PlaybackState &playback, lofi::UiState &ui,
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

    lofi_audio::set_volume(playback.volume);
    if (!audio_loaded || active_track != current) {
        const lofi::Track &track = library.tracks[static_cast<size_t>(current)];
        esp_err_t err = lofi_audio::play_track(track, playback.volume, playback.lofi, playback.position_seconds);
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

void rebuild_library(bool sd_mounted, lofi::LibraryIndex &library, lofi::PlaybackState &playback,
                     lofi::UiState &ui, bool resume_playing, bool show_toast)
{
    bool using_samples = false;
    const std::vector<std::string> music_paths = scan_music_paths(sd_mounted, &using_samples);
    library = lofi::build_library_index(music_paths);
    populate_track_durations(library);
    log_library_summary(library);

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
    ui.page = library.tracks.empty() ? lofi::Page::Empty : lofi::Page::LibraryHome;
    if (show_toast || using_samples) {
        ui.toast = using_samples ? "Using samples" : "Rescan done";
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
    if (!lofi::restore_playback_queue(library, restored, false)) {
        return false;
    }
    playback = restored;
    ui.page = lofi::Page::NowPlaying;
    ui.toast = "Restored";
    ESP_LOGI(TAG, "Restored playback state from %s", kStateFile);
    return true;
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
    rebuild_library(sd_mounted, library, playback, ui, false, false);

    load_playback_state_if_possible(sd_mounted, library, playback, ui);
    if (playback.volume < 35 || playback.volume > 80) {
        playback.volume = 70;
        ui.toast = "Volume 70";
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
    lofi::LofiProfile active_profile;
    TickType_t last_redraw = xTaskGetTickCount();
    TickType_t last_state_save = xTaskGetTickCount();
    TickType_t last_status_log = xTaskGetTickCount();
    TickType_t last_keyboard_retry = xTaskGetTickCount();
    int last_saved_position = playback.position_seconds;
    bool state_saved = false;
    log_runtime_status(library, playback, ui, sd_mounted, state_saved);
    if (save_playback_state_if_possible(sd_mounted, playback)) {
        state_saved = true;
        ESP_LOGI(TAG, "STATE_SAVE boot ok path=%s", kStateFile);
        last_saved_position = playback.position_seconds;
    }

    while (true) {
        bool needs_redraw = false;
        bool needs_state_save = false;
        int requested_seek_seconds = -1;
        lofi::Action action = lofi::Action::None;
        const char *key_name = nullptr;
        bool frame_dump = false;
        bool color_test = false;
        bool wav_selftest = false;
        bool mp3_selftest = false;
        if (lofi_board::poll_action(action, &key_name) ||
            poll_serial_action(action, &key_name, color_test, frame_dump, wav_selftest, mp3_selftest)) {
            if (key_name && std::strcmp(key_name, "C") == 0 && action == lofi::Action::None) {
                color_test = true;
            }
            ESP_LOGI(TAG, "Key %s -> action %d", key_name ? key_name : "?", static_cast<int>(action));
            if (color_test) {
                ESP_ERROR_CHECK_WITHOUT_ABORT(lofi_board::draw_color_test());
            } else if (frame_dump) {
                lofi_board::dump_framebuffer_to_serial();
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
            }
            if (!color_test) {
                needs_redraw = true;
                needs_state_save = true;
            }
        }

        const int previous_position = playback.position_seconds;
        const bool previous_playing = playback.playing;
        const std::string previous_toast = ui.toast;
        sync_audio(library, playback, ui, active_track, audio_loaded, last_playing, active_profile, requested_seek_seconds);
        if (previous_playing != playback.playing || previous_toast != ui.toast) {
            needs_redraw = true;
        }
        const TickType_t redraw_interval = playback.playing ? pdMS_TO_TICKS(250) : pdMS_TO_TICKS(750);
        if (previous_position != playback.position_seconds &&
            xTaskGetTickCount() - last_redraw > redraw_interval) {
            needs_redraw = true;
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
            }
            last_keyboard_retry = xTaskGetTickCount();
        }

        if (needs_redraw) {
            screen = lofi::render_screen(library, playback, ui);
            log_screen(screen);
            if (display_err == ESP_OK) {
                lofi_board::draw_screen(screen);
            }
            last_redraw = xTaskGetTickCount();
        }
        if (needs_state_save && save_playback_state_if_possible(sd_mounted, playback)) {
            state_saved = true;
            last_state_save = xTaskGetTickCount();
            last_saved_position = playback.position_seconds;
        }
        if (xTaskGetTickCount() - last_status_log > pdMS_TO_TICKS(5000)) {
            log_runtime_status(library, playback, ui, sd_mounted, state_saved);
            last_status_log = xTaskGetTickCount();
        }
        lofi_board::tick_display();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
