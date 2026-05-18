#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace lofi {

struct WavInfo {
    uint16_t audio_format = 0;
    uint16_t channels = 0;
    uint32_t sample_rate = 0;
    uint16_t bits_per_sample = 0;
    uint32_t data_offset = 0;
    uint32_t data_size = 0;
};

enum class WavParseResult {
    Ok,
    IoError,
    NotRiff,
    NotWave,
    MissingFmt,
    MissingData,
    UnsupportedFormat,
};

WavParseResult parse_wav_file(FILE *file, WavInfo &out);
const char *to_string(WavParseResult result);
bool is_supported_pcm_wav(const WavInfo &info);
std::string describe_wav(const WavInfo &info);

} // namespace lofi
