#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace lofi {

struct M4aAacFrame {
    uint64_t offset = 0;
    uint32_t size = 0;
};

struct M4aAacIndex {
    std::vector<M4aAacFrame> frames;
    uint64_t first_audio_offset = 0;
    uint64_t audio_payload_bytes = 0;
    uint32_t sample_rate = 0;
    uint8_t channels = 0;
    uint8_t audio_object_type = 0;
    uint8_t sampling_frequency_index = 0;
    uint8_t channel_config = 0;
};

struct M4aAacSummary {
    uint64_t first_audio_offset = 0;
    uint64_t audio_payload_bytes = 0;
    uint32_t frame_count = 0;
    uint32_t sample_rate = 0;
    uint8_t channels = 0;
};

using M4aAacCancelCallback = bool (*)(void *user);

bool parse_m4a_aac_index(const std::string &path, M4aAacIndex &out, std::string &error);
bool inspect_m4a_aac_summary(const std::string &path, M4aAacSummary &out, std::string &error);
bool write_adts_aac_cache(const std::string &source_path,
                          const std::string &cache_path,
                          const M4aAacIndex &index,
                          std::string &error);
bool write_adts_aac_cache_from_m4a(const std::string &source_path,
                                   const std::string &cache_path,
                                   M4aAacSummary &summary,
                                   std::string &error,
                                   M4aAacCancelCallback should_cancel = nullptr,
                                   void *cancel_user = nullptr);

} // namespace lofi
