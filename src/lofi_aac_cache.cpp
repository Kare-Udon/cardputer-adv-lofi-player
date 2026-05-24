#include "lofi_aac_cache.hpp"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>

namespace lofi {
namespace {

constexpr uint32_t kMoov = 0x6d6f6f76;
constexpr uint32_t kTrak = 0x7472616b;
constexpr uint32_t kMdia = 0x6d646961;
constexpr uint32_t kHdlr = 0x68646c72;
constexpr uint32_t kMinf = 0x6d696e66;
constexpr uint32_t kStbl = 0x7374626c;
constexpr uint32_t kStsd = 0x73747364;
constexpr uint32_t kStsz = 0x7374737a;
constexpr uint32_t kStsc = 0x73747363;
constexpr uint32_t kStco = 0x7374636f;
constexpr uint32_t kCo64 = 0x636f3634;
constexpr uint32_t kMp4a = 0x6d703461;
constexpr uint32_t kEsds = 0x65736473;
constexpr uint32_t kSoun = 0x736f756e;

struct Atom {
    uint64_t offset = 0;
    uint64_t size = 0;
    uint64_t header_size = 0;
    uint32_t type = 0;
};

struct StscEntry {
    uint32_t first_chunk = 0;
    uint32_t samples_per_chunk = 0;
};

struct TrackTables {
    bool is_audio = false;
    uint32_t sample_size = 0;
    uint32_t sample_count = 0;
    uint64_t sample_size_table_offset = 0;
    std::vector<StscEntry> sample_to_chunk;
    std::vector<uint64_t> chunk_offsets;
    uint32_t sample_rate = 0;
    uint8_t channels = 0;
    uint8_t audio_object_type = 0;
    uint8_t sampling_frequency_index = 0;
    uint8_t channel_config = 0;
};

bool seek_to(FILE *file, uint64_t offset)
{
    return offset <= static_cast<uint64_t>(LONG_MAX) &&
           std::fseek(file, static_cast<long>(offset), SEEK_SET) == 0;
}

bool read_exact(FILE *file, void *buffer, size_t size)
{
    return std::fread(buffer, 1, size, file) == size;
}

bool read_u8(FILE *file, uint8_t &value)
{
    return read_exact(file, &value, 1);
}

bool read_be16(FILE *file, uint16_t &value)
{
    uint8_t bytes[2] = {};
    if (!read_exact(file, bytes, sizeof(bytes))) {
        return false;
    }
    value = (static_cast<uint16_t>(bytes[0]) << 8) | bytes[1];
    return true;
}

bool read_be32(FILE *file, uint32_t &value)
{
    uint8_t bytes[4] = {};
    if (!read_exact(file, bytes, sizeof(bytes))) {
        return false;
    }
    value = (static_cast<uint32_t>(bytes[0]) << 24) |
            (static_cast<uint32_t>(bytes[1]) << 16) |
            (static_cast<uint32_t>(bytes[2]) << 8) |
            bytes[3];
    return true;
}

bool read_be64(FILE *file, uint64_t &value)
{
    uint32_t hi = 0;
    uint32_t lo = 0;
    if (!read_be32(file, hi) || !read_be32(file, lo)) {
        return false;
    }
    value = (static_cast<uint64_t>(hi) << 32) | lo;
    return true;
}

bool read_atom(FILE *file, uint64_t offset, uint64_t parent_end, Atom &atom)
{
    if (offset + 8 > parent_end || !seek_to(file, offset)) {
        return false;
    }
    uint32_t size32 = 0;
    uint32_t type = 0;
    if (!read_be32(file, size32) || !read_be32(file, type)) {
        return false;
    }
    atom.offset = offset;
    atom.type = type;
    atom.header_size = 8;
    if (size32 == 1) {
        uint64_t size64 = 0;
        if (!read_be64(file, size64)) {
            return false;
        }
        atom.size = size64;
        atom.header_size = 16;
    } else if (size32 == 0) {
        atom.size = parent_end - offset;
    } else {
        atom.size = size32;
    }
    return atom.size >= atom.header_size && atom.offset + atom.size <= parent_end;
}

uint32_t descriptor_length(FILE *file, uint64_t &pos, uint64_t end, bool &ok)
{
    uint32_t length = 0;
    ok = false;
    for (int i = 0; i < 4 && pos < end; ++i) {
        uint8_t byte = 0;
        if (!read_u8(file, byte)) {
            return 0;
        }
        ++pos;
        length = (length << 7) | (byte & 0x7f);
        if ((byte & 0x80) == 0) {
            ok = true;
            return length;
        }
    }
    return 0;
}

bool read_descriptor_header(FILE *file, uint64_t &pos, uint64_t end, uint8_t &tag, uint32_t &length)
{
    if (pos >= end || !seek_to(file, pos) || !read_u8(file, tag)) {
        return false;
    }
    ++pos;
    bool ok = false;
    length = descriptor_length(file, pos, end, ok);
    return ok && pos + length <= end;
}

bool parse_audio_specific_config(FILE *file, uint64_t offset, uint32_t length, TrackTables &track)
{
    if (length < 2 || !seek_to(file, offset)) {
        return false;
    }
    uint8_t bytes[2] = {};
    if (!read_exact(file, bytes, sizeof(bytes))) {
        return false;
    }
    track.audio_object_type = bytes[0] >> 3;
    track.sampling_frequency_index = ((bytes[0] & 0x07) << 1) | (bytes[1] >> 7);
    track.channel_config = (bytes[1] >> 3) & 0x0f;
    return track.audio_object_type != 0 && track.sampling_frequency_index != 0x0f && track.channel_config != 0;
}

bool parse_esds(FILE *file, const Atom &atom, TrackTables &track)
{
    uint64_t pos = atom.offset + atom.header_size + 4; // version and flags
    const uint64_t end = atom.offset + atom.size;
    while (pos < end) {
        uint8_t tag = 0;
        uint32_t length = 0;
        if (!read_descriptor_header(file, pos, end, tag, length)) {
            return false;
        }
        const uint64_t desc_data = pos;
        const uint64_t desc_end = pos + length;
        if (tag == 0x03 && length >= 3) {
            uint64_t child = desc_data + 3; // ES_ID and flags
            while (child < desc_end) {
                uint8_t child_tag = 0;
                uint32_t child_len = 0;
                if (!read_descriptor_header(file, child, desc_end, child_tag, child_len)) {
                    return false;
                }
                const uint64_t child_data = child;
                const uint64_t child_end = child + child_len;
                if (child_tag == 0x04 && child_len >= 13) {
                    uint64_t dsi = child_data + 13;
                    while (dsi < child_end) {
                        uint8_t dsi_tag = 0;
                        uint32_t dsi_len = 0;
                        if (!read_descriptor_header(file, dsi, child_end, dsi_tag, dsi_len)) {
                            return false;
                        }
                        if (dsi_tag == 0x05) {
                            return parse_audio_specific_config(file, dsi, dsi_len, track);
                        }
                        dsi += dsi_len;
                    }
                }
                child = child_end;
            }
        }
        pos = desc_end;
    }
    return false;
}

bool parse_stsd(FILE *file, const Atom &atom, TrackTables &track)
{
    if (!seek_to(file, atom.offset + atom.header_size + 4)) {
        return false;
    }
    uint32_t entry_count = 0;
    if (!read_be32(file, entry_count)) {
        return false;
    }
    uint64_t entry_pos = atom.offset + atom.header_size + 8;
    const uint64_t end = atom.offset + atom.size;
    for (uint32_t i = 0; i < entry_count && entry_pos < end; ++i) {
        Atom entry;
        if (!read_atom(file, entry_pos, end, entry)) {
            return false;
        }
        if (entry.type == kMp4a && entry.size >= entry.header_size + 36) {
            uint16_t channels = 0;
            uint32_t sample_rate_fixed = 0;
            if (!seek_to(file, entry.offset + entry.header_size + 16) ||
                !read_be16(file, channels) ||
                !seek_to(file, entry.offset + entry.header_size + 24) ||
                !read_be32(file, sample_rate_fixed)) {
                return false;
            }
            track.channels = static_cast<uint8_t>(std::min<uint16_t>(channels, 255));
            track.sample_rate = sample_rate_fixed >> 16;

            uint64_t child_pos = entry.offset + entry.header_size + 28;
            const uint64_t entry_end = entry.offset + entry.size;
            while (child_pos < entry_end) {
                Atom child;
                if (!read_atom(file, child_pos, entry_end, child)) {
                    return false;
                }
                if (child.type == kEsds && parse_esds(file, child, track)) {
                    return true;
                }
                child_pos += child.size;
            }
        }
        entry_pos += entry.size;
    }
    return false;
}

bool parse_stsz(FILE *file, const Atom &atom, TrackTables &track)
{
    if (!seek_to(file, atom.offset + atom.header_size + 4)) {
        return false;
    }
    uint32_t sample_size = 0;
    uint32_t sample_count = 0;
    if (!read_be32(file, sample_size) || !read_be32(file, sample_count)) {
        return false;
    }
    const uint64_t table_offset = atom.offset + atom.header_size + 12;
    const uint64_t atom_end = atom.offset + atom.size;
    if (sample_size == 0 && table_offset + static_cast<uint64_t>(sample_count) * 4 > atom_end) {
        return false;
    }
    track.sample_size = sample_size;
    track.sample_count = sample_count;
    track.sample_size_table_offset = sample_size == 0 ? table_offset : 0;
    return sample_count > 0;
}

bool parse_stsc(FILE *file, const Atom &atom, TrackTables &track)
{
    if (!seek_to(file, atom.offset + atom.header_size + 4)) {
        return false;
    }
    uint32_t entry_count = 0;
    if (!read_be32(file, entry_count)) {
        return false;
    }
    track.sample_to_chunk.clear();
    track.sample_to_chunk.reserve(entry_count);
    for (uint32_t i = 0; i < entry_count; ++i) {
        uint32_t first_chunk = 0;
        uint32_t samples_per_chunk = 0;
        uint32_t sample_description_index = 0;
        if (!read_be32(file, first_chunk) ||
            !read_be32(file, samples_per_chunk) ||
            !read_be32(file, sample_description_index)) {
            return false;
        }
        (void)sample_description_index;
        if (first_chunk == 0 || samples_per_chunk == 0) {
            return false;
        }
        track.sample_to_chunk.push_back({first_chunk, samples_per_chunk});
    }
    return !track.sample_to_chunk.empty();
}

bool parse_chunk_offsets(FILE *file, const Atom &atom, TrackTables &track, bool is_co64)
{
    if (!seek_to(file, atom.offset + atom.header_size + 4)) {
        return false;
    }
    uint32_t entry_count = 0;
    if (!read_be32(file, entry_count)) {
        return false;
    }
    track.chunk_offsets.clear();
    track.chunk_offsets.reserve(entry_count);
    for (uint32_t i = 0; i < entry_count; ++i) {
        uint64_t offset = 0;
        if (is_co64) {
            if (!read_be64(file, offset)) {
                return false;
            }
        } else {
            uint32_t offset32 = 0;
            if (!read_be32(file, offset32)) {
                return false;
            }
            offset = offset32;
        }
        track.chunk_offsets.push_back(offset);
    }
    return !track.chunk_offsets.empty();
}

bool parse_stbl(FILE *file, const Atom &atom, TrackTables &track)
{
    uint64_t pos = atom.offset + atom.header_size;
    const uint64_t end = atom.offset + atom.size;
    bool have_stsd = false;
    bool have_stsz = false;
    bool have_stsc = false;
    bool have_stco = false;
    while (pos < end) {
        Atom child;
        if (!read_atom(file, pos, end, child)) {
            return false;
        }
        if (child.type == kStsd) {
            have_stsd = parse_stsd(file, child, track);
        } else if (child.type == kStsz) {
            have_stsz = parse_stsz(file, child, track);
        } else if (child.type == kStsc) {
            have_stsc = parse_stsc(file, child, track);
        } else if (child.type == kStco || child.type == kCo64) {
            have_stco = parse_chunk_offsets(file, child, track, child.type == kCo64);
        }
        pos += child.size;
    }
    return have_stsd && have_stsz && have_stsc && have_stco;
}

bool parse_container_children(FILE *file, const Atom &atom, uint32_t wanted_type, Atom &out)
{
    uint64_t pos = atom.offset + atom.header_size;
    const uint64_t end = atom.offset + atom.size;
    while (pos < end) {
        Atom child;
        if (!read_atom(file, pos, end, child)) {
            return false;
        }
        if (child.type == wanted_type) {
            out = child;
            return true;
        }
        pos += child.size;
    }
    return false;
}

bool parse_hdlr(FILE *file, const Atom &atom, uint32_t &handler_type)
{
    if (!seek_to(file, atom.offset + atom.header_size + 8)) {
        return false;
    }
    return read_be32(file, handler_type);
}

bool parse_trak(FILE *file, const Atom &trak, TrackTables &track)
{
    Atom mdia;
    if (!parse_container_children(file, trak, kMdia, mdia)) {
        return false;
    }
    Atom hdlr;
    uint32_t handler = 0;
    if (!parse_container_children(file, mdia, kHdlr, hdlr) || !parse_hdlr(file, hdlr, handler) || handler != kSoun) {
        return false;
    }
    Atom minf;
    Atom stbl;
    if (!parse_container_children(file, mdia, kMinf, minf) ||
        !parse_container_children(file, minf, kStbl, stbl) ||
        !parse_stbl(file, stbl, track)) {
        return false;
    }
    track.is_audio = true;
    return true;
}

struct SampleSizeReader {
    FILE *file = nullptr;
    const TrackTables *track = nullptr;
    uint32_t next_index = 0;

    bool begin(FILE *source, const TrackTables &tables)
    {
        file = source;
        track = &tables;
        next_index = 0;
        return tables.sample_size != 0 || seek_to(source, tables.sample_size_table_offset);
    }

    bool read_next(uint32_t &size)
    {
        if (!track || next_index >= track->sample_count) {
            return false;
        }
        if (track->sample_size != 0) {
            size = track->sample_size;
            ++next_index;
            return true;
        }
        if (!read_be32(file, size)) {
            return false;
        }
        ++next_index;
        return true;
    }
};

bool build_frames(FILE *file, const TrackTables &track, M4aAacIndex &out)
{
    if (track.sample_count == 0 || track.sample_to_chunk.empty() || track.chunk_offsets.empty()) {
        return false;
    }
    SampleSizeReader sizes;
    if (!sizes.begin(file, track)) {
        return false;
    }
    out.frames.clear();
    out.frames.reserve(track.sample_count);
    size_t sample_index = 0;
    size_t stsc_index = 0;
    for (size_t chunk = 1; chunk <= track.chunk_offsets.size(); ++chunk) {
        while (stsc_index + 1 < track.sample_to_chunk.size() &&
               track.sample_to_chunk[stsc_index + 1].first_chunk <= chunk) {
            ++stsc_index;
        }
        uint64_t offset = track.chunk_offsets[chunk - 1];
        const uint32_t samples_per_chunk = track.sample_to_chunk[stsc_index].samples_per_chunk;
        for (uint32_t i = 0; i < samples_per_chunk && sample_index < track.sample_count; ++i) {
            uint32_t size = 0;
            if (!sizes.read_next(size)) {
                return false;
            }
            ++sample_index;
            if (size == 0) {
                return false;
            }
            out.frames.push_back({offset, size});
            out.audio_payload_bytes += size;
            offset += size;
        }
    }
    if (sample_index != track.sample_count || out.frames.empty()) {
        return false;
    }
    out.first_audio_offset = out.frames[0].offset;
    out.sample_rate = track.sample_rate;
    out.channels = track.channels;
    out.audio_object_type = track.audio_object_type;
    out.sampling_frequency_index = track.sampling_frequency_index;
    out.channel_config = track.channel_config;
    return out.sample_rate > 0 && out.channels > 0 && out.audio_object_type > 0;
}

uint8_t adts_profile(uint8_t audio_object_type)
{
    return audio_object_type > 0 ? static_cast<uint8_t>(audio_object_type - 1) : 1;
}

bool write_adts_header(FILE *file,
                       uint8_t audio_object_type,
                       uint8_t sampling_frequency_index,
                       uint8_t channel_config,
                       uint32_t payload_size)
{
    const uint8_t profile = adts_profile(audio_object_type);
    const uint8_t freq = sampling_frequency_index & 0x0f;
    const uint8_t channels = channel_config & 0x0f;
    const uint32_t full_size = payload_size + 7;
    if (full_size > 0x1fff || freq == 0x0f || channels == 0) {
        return false;
    }
    uint8_t header[7] = {};
    header[0] = 0xff;
    header[1] = 0xf1;
    header[2] = static_cast<uint8_t>(((profile & 0x03) << 6) | (freq << 2) | ((channels >> 2) & 0x01));
    header[3] = static_cast<uint8_t>(((channels & 0x03) << 6) | ((full_size >> 11) & 0x03));
    header[4] = static_cast<uint8_t>((full_size >> 3) & 0xff);
    header[5] = static_cast<uint8_t>(((full_size & 0x07) << 5) | 0x1f);
    header[6] = 0xfc;
    return std::fwrite(header, 1, sizeof(header), file) == sizeof(header);
}

bool parse_m4a_aac_tables(const std::string &path, TrackTables &out, std::string &error)
{
    out = TrackTables{};
    error.clear();
    FILE *file = std::fopen(path.c_str(), "rb");
    if (!file) {
        error = "open failed";
        return false;
    }
    if (std::fseek(file, 0, SEEK_END) != 0) {
        std::fclose(file);
        error = "seek end failed";
        return false;
    }
    const long file_size_long = std::ftell(file);
    if (file_size_long <= 0) {
        std::fclose(file);
        error = "empty file";
        return false;
    }
    const uint64_t file_size = static_cast<uint64_t>(file_size_long);
    uint64_t pos = 0;
    Atom moov;
    bool found_moov = false;
    while (pos < file_size) {
        Atom atom;
        if (!read_atom(file, pos, file_size, atom)) {
            std::fclose(file);
            error = "invalid top-level atom";
            return false;
        }
        if (atom.type == kMoov) {
            moov = atom;
            found_moov = true;
            break;
        }
        pos += atom.size;
    }
    if (!found_moov) {
        std::fclose(file);
        error = "missing moov atom";
        return false;
    }

    uint64_t trak_pos = moov.offset + moov.header_size;
    const uint64_t moov_end = moov.offset + moov.size;
    while (trak_pos < moov_end) {
        Atom child;
        if (!read_atom(file, trak_pos, moov_end, child)) {
            std::fclose(file);
            error = "invalid moov child";
            return false;
        }
        if (child.type == kTrak && parse_trak(file, child, out)) {
            std::fclose(file);
            return true;
        }
        trak_pos += child.size;
    }
    std::fclose(file);
    error = "missing AAC audio track";
    return false;
}

bool summarize_tables(FILE *file, const TrackTables &track, M4aAacSummary &summary)
{
    summary = M4aAacSummary{};
    if (track.sample_count == 0 || track.sample_to_chunk.empty() || track.chunk_offsets.empty()) {
        return false;
    }
    SampleSizeReader sizes;
    if (!sizes.begin(file, track)) {
        return false;
    }
    size_t sample_index = 0;
    size_t stsc_index = 0;
    for (size_t chunk = 1; chunk <= track.chunk_offsets.size(); ++chunk) {
        while (stsc_index + 1 < track.sample_to_chunk.size() &&
               track.sample_to_chunk[stsc_index + 1].first_chunk <= chunk) {
            ++stsc_index;
        }
        uint64_t offset = track.chunk_offsets[chunk - 1];
        const uint32_t samples_per_chunk = track.sample_to_chunk[stsc_index].samples_per_chunk;
        for (uint32_t i = 0; i < samples_per_chunk && sample_index < track.sample_count; ++i) {
            uint32_t size = 0;
            if (!sizes.read_next(size)) {
                return false;
            }
            ++sample_index;
            if (size == 0) {
                return false;
            }
            if (summary.frame_count == 0) {
                summary.first_audio_offset = offset;
            }
            ++summary.frame_count;
            summary.audio_payload_bytes += size;
            offset += size;
        }
    }
    summary.sample_rate = track.sample_rate;
    summary.channels = track.channels;
    return sample_index == track.sample_count && summary.frame_count > 0;
}

bool stream_adts_aac_cache(FILE *source,
                           FILE *sample_size_source,
                           FILE *cache,
                           const TrackTables &track,
                           M4aAacSummary &summary,
                           std::string &error,
                           M4aAacCancelCallback should_cancel,
                           void *cancel_user)
{
    summary = M4aAacSummary{};
    uint8_t buffer[4096] = {};
    SampleSizeReader sizes;
    if (!sizes.begin(sample_size_source, track)) {
        error = "sample size table seek failed";
        return false;
    }
    size_t sample_index = 0;
    size_t stsc_index = 0;
    for (size_t chunk = 1; chunk <= track.chunk_offsets.size(); ++chunk) {
        if (should_cancel && should_cancel(cancel_user)) {
            error = "cancelled";
            return false;
        }
        while (stsc_index + 1 < track.sample_to_chunk.size() &&
               track.sample_to_chunk[stsc_index + 1].first_chunk <= chunk) {
            ++stsc_index;
        }
        uint64_t offset = track.chunk_offsets[chunk - 1];
        const uint32_t samples_per_chunk = track.sample_to_chunk[stsc_index].samples_per_chunk;
        if (!seek_to(source, offset)) {
            error = "seek chunk failed";
            return false;
        }
        for (uint32_t i = 0; i < samples_per_chunk && sample_index < track.sample_count; ++i) {
            uint32_t size = 0;
            if (!sizes.read_next(size)) {
                error = "read sample size failed";
                return false;
            }
            ++sample_index;
            if (summary.frame_count == 0) {
                summary.first_audio_offset = offset;
            }
            if (!write_adts_header(cache,
                                   track.audio_object_type,
                                   track.sampling_frequency_index,
                                   track.channel_config,
                                   size)) {
                error = "write header failed";
                return false;
            }
            uint32_t remaining = size;
            while (remaining > 0) {
                if (should_cancel && should_cancel(cancel_user)) {
                    error = "cancelled";
                    return false;
                }
                const size_t part = std::min<size_t>(sizeof(buffer), remaining);
                if (std::fread(buffer, 1, part, source) != part ||
                    std::fwrite(buffer, 1, part, cache) != part) {
                    error = "copy frame failed";
                    return false;
                }
                remaining -= static_cast<uint32_t>(part);
            }
            ++summary.frame_count;
            summary.audio_payload_bytes += size;
            offset += size;
        }
    }
    summary.sample_rate = track.sample_rate;
    summary.channels = track.channels;
    return sample_index == track.sample_count && summary.frame_count > 0;
}

} // namespace

bool parse_m4a_aac_index(const std::string &path, M4aAacIndex &out, std::string &error)
{
    out = M4aAacIndex{};
    TrackTables track;
    if (!parse_m4a_aac_tables(path, track, error)) {
        return false;
    }
    FILE *file = std::fopen(path.c_str(), "rb");
    if (!file) {
        error = "open failed";
        return false;
    }
    const bool ok = build_frames(file, track, out);
    std::fclose(file);
    if (!ok) {
        error = "invalid audio sample table";
        return false;
    }
    return true;
}

bool inspect_m4a_aac_summary(const std::string &path, M4aAacSummary &out, std::string &error)
{
    TrackTables track;
    if (!parse_m4a_aac_tables(path, track, error)) {
        return false;
    }
    FILE *file = std::fopen(path.c_str(), "rb");
    if (!file) {
        error = "open failed";
        return false;
    }
    const bool ok = summarize_tables(file, track, out);
    std::fclose(file);
    if (!ok) {
        error = "invalid audio sample table";
        return false;
    }
    return true;
}

bool write_adts_aac_cache(const std::string &source_path,
                          const std::string &cache_path,
                          const M4aAacIndex &index,
                          std::string &error)
{
    error.clear();
    if (index.frames.empty()) {
        error = "empty AAC index";
        return false;
    }
    FILE *source = std::fopen(source_path.c_str(), "rb");
    if (!source) {
        error = "source open failed";
        return false;
    }
    FILE *cache = std::fopen(cache_path.c_str(), "wb");
    if (!cache) {
        std::fclose(source);
        error = "cache open failed";
        return false;
    }

    uint8_t buffer[4096] = {};
    for (const M4aAacFrame &frame : index.frames) {
        if (!write_adts_header(cache,
                               index.audio_object_type,
                               index.sampling_frequency_index,
                               index.channel_config,
                               frame.size) ||
            !seek_to(source, frame.offset)) {
            std::fclose(source);
            std::fclose(cache);
            error = "write header or seek failed";
            return false;
        }
        uint32_t remaining = frame.size;
        while (remaining > 0) {
            const size_t chunk = std::min<size_t>(sizeof(buffer), remaining);
            if (std::fread(buffer, 1, chunk, source) != chunk ||
                std::fwrite(buffer, 1, chunk, cache) != chunk) {
                std::fclose(source);
                std::fclose(cache);
                error = "copy frame failed";
                return false;
            }
            remaining -= static_cast<uint32_t>(chunk);
        }
    }

    const bool ok = std::fclose(cache) == 0;
    std::fclose(source);
    if (!ok) {
        error = "cache close failed";
    }
    return ok;
}

bool write_adts_aac_cache_from_m4a(const std::string &source_path,
                                   const std::string &cache_path,
                                   M4aAacSummary &summary,
                                   std::string &error,
                                   M4aAacCancelCallback should_cancel,
                                   void *cancel_user)
{
    error.clear();
    if (should_cancel && should_cancel(cancel_user)) {
        error = "cancelled";
        return false;
    }
    TrackTables track;
    if (!parse_m4a_aac_tables(source_path, track, error)) {
        return false;
    }
    if (should_cancel && should_cancel(cancel_user)) {
        error = "cancelled";
        return false;
    }
    FILE *source = std::fopen(source_path.c_str(), "rb");
    if (!source) {
        error = "source open failed";
        return false;
    }
    FILE *sample_size_source = std::fopen(source_path.c_str(), "rb");
    if (!sample_size_source) {
        std::fclose(source);
        error = "sample size source open failed";
        return false;
    }
    FILE *cache = std::fopen(cache_path.c_str(), "wb");
    if (!cache) {
        std::fclose(sample_size_source);
        std::fclose(source);
        error = "cache open failed";
        return false;
    }
    const bool copied = stream_adts_aac_cache(source,
                                             sample_size_source,
                                             cache,
                                             track,
                                             summary,
                                             error,
                                             should_cancel,
                                             cancel_user);
    const bool closed = std::fclose(cache) == 0;
    std::fclose(sample_size_source);
    std::fclose(source);
    if (!copied) {
        return false;
    }
    if (!closed) {
        error = "cache close failed";
        return false;
    }
    return true;
}

} // namespace lofi
