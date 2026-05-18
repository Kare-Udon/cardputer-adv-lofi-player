#pragma once

#include "lofi_core.hpp"

#include <cstddef>
#include <cstdint>

namespace lofi {

struct LofiDspState {
    int32_t lowpass[2] = {0, 0};
    uint32_t noise_state = 0x51f15eedu;
    uint32_t frame_index = 0;
    int32_t wobble = 0;
    int32_t wobble_target = 0;
};

bool lofi_dsp_active(const LofiProfile &profile);
void process_lofi_pcm16(int16_t *samples,
                        size_t sample_count,
                        int channels,
                        int volume,
                        const LofiProfile &profile,
                        LofiDspState &state);

} // namespace lofi
