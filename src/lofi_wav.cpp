#include "lofi_wav.hpp"

#include <sstream>

namespace lofi {
namespace {

bool read_exact(FILE *file, void *data, size_t size)
{
    return std::fread(data, 1, size, file) == size;
}

uint16_t le16(const uint8_t *data)
{
    return static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8);
}

uint32_t le32(const uint8_t *data)
{
    return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
}

bool id_eq(const uint8_t *data, const char *id)
{
    return data[0] == static_cast<uint8_t>(id[0]) && data[1] == static_cast<uint8_t>(id[1]) &&
           data[2] == static_cast<uint8_t>(id[2]) && data[3] == static_cast<uint8_t>(id[3]);
}

long padded_chunk_advance(uint32_t size)
{
    return static_cast<long>(size + (size & 1U));
}

} // namespace

bool is_supported_pcm_wav(const WavInfo &info)
{
    return info.audio_format == 1 && (info.channels == 1 || info.channels == 2) &&
           (info.sample_rate >= 8000 && info.sample_rate <= 48000) && info.bits_per_sample == 16 &&
           info.data_offset > 0 && info.data_size > 0;
}

WavParseResult parse_wav_file(FILE *file, WavInfo &out)
{
    if (!file) {
        return WavParseResult::IoError;
    }
    std::rewind(file);

    uint8_t riff[12] = {};
    if (!read_exact(file, riff, sizeof(riff))) {
        return WavParseResult::IoError;
    }
    if (!id_eq(riff, "RIFF")) {
        return WavParseResult::NotRiff;
    }
    if (!id_eq(riff + 8, "WAVE")) {
        return WavParseResult::NotWave;
    }

    bool have_fmt = false;
    bool have_data = false;
    while (!have_data) {
        uint8_t header[8] = {};
        if (!read_exact(file, header, sizeof(header))) {
            break;
        }

        const uint32_t chunk_size = le32(header + 4);
        const long chunk_data_pos = std::ftell(file);
        if (chunk_data_pos < 0) {
            return WavParseResult::IoError;
        }

        if (id_eq(header, "fmt ")) {
            if (chunk_size < 16) {
                return WavParseResult::UnsupportedFormat;
            }
            uint8_t fmt[16] = {};
            if (!read_exact(file, fmt, sizeof(fmt))) {
                return WavParseResult::IoError;
            }
            out.audio_format = le16(fmt);
            out.channels = le16(fmt + 2);
            out.sample_rate = le32(fmt + 4);
            out.bits_per_sample = le16(fmt + 14);
            have_fmt = true;
        } else if (id_eq(header, "data")) {
            out.data_offset = static_cast<uint32_t>(chunk_data_pos);
            out.data_size = chunk_size;
            have_data = true;
        }

        if (std::fseek(file, chunk_data_pos + padded_chunk_advance(chunk_size), SEEK_SET) != 0) {
            return WavParseResult::IoError;
        }
    }

    if (!have_fmt) {
        return WavParseResult::MissingFmt;
    }
    if (!have_data) {
        return WavParseResult::MissingData;
    }
    return is_supported_pcm_wav(out) ? WavParseResult::Ok : WavParseResult::UnsupportedFormat;
}

const char *to_string(WavParseResult result)
{
    switch (result) {
    case WavParseResult::Ok:
        return "ok";
    case WavParseResult::IoError:
        return "io_error";
    case WavParseResult::NotRiff:
        return "not_riff";
    case WavParseResult::NotWave:
        return "not_wave";
    case WavParseResult::MissingFmt:
        return "missing_fmt";
    case WavParseResult::MissingData:
        return "missing_data";
    case WavParseResult::UnsupportedFormat:
    default:
        return "unsupported_format";
    }
}

std::string describe_wav(const WavInfo &info)
{
    std::ostringstream out;
    out << info.sample_rate << "Hz " << info.channels << "ch " << info.bits_per_sample << "bit";
    return out.str();
}

} // namespace lofi
