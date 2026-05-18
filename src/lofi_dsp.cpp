#include "lofi_dsp.hpp"

#include <algorithm>
#include <cstdlib>

namespace lofi {
namespace {

int clamp_percent(int value)
{
    return std::max(0, std::min(100, value));
}

int16_t clamp_i16(int value)
{
    return static_cast<int16_t>(std::max(-32768, std::min(32767, value)));
}

uint32_t next_noise(uint32_t &state)
{
    state = state * 1664525u + 1013904223u;
    return state;
}

int soft_clip(int value)
{
    const int sign = value < 0 ? -1 : 1;
    int magnitude = std::abs(value);
    if (magnitude > 26000) {
        magnitude = 26000 + (magnitude - 26000) / 4;
    }
    return sign * std::min(magnitude, 32767);
}

} // namespace

bool lofi_dsp_active(const LofiProfile &profile)
{
    return profile.preset != LofiPreset::Off &&
           (clamp_percent(profile.intensity) > 0 ||
            clamp_percent(profile.warmth) > 0 ||
            clamp_percent(profile.noise) > 0 ||
            clamp_percent(profile.wobble) > 0 ||
            clamp_percent(profile.space) > 0 ||
            clamp_percent(profile.softness) > 0);
}

void process_lofi_pcm16(int16_t *samples,
                        size_t sample_count,
                        int channels,
                        int volume,
                        const LofiProfile &profile,
                        LofiDspState &state)
{
    if (!samples || sample_count == 0) {
        return;
    }
    if (volume <= 0) {
        std::fill(samples, samples + sample_count, 0);
        return;
    }
    if (!lofi_dsp_active(profile)) {
        return;
    }

    channels = std::max(1, std::min(2, channels));
    const int intensity = clamp_percent(profile.intensity);
    const int warmth = clamp_percent(profile.warmth);
    const int noise = clamp_percent(profile.noise);
    const int wobble = clamp_percent(profile.wobble);
    const int space = clamp_percent(profile.space);
    const int softness = clamp_percent(profile.softness);

    const int wet = std::max(1, intensity);
    const int smooth = wet * softness / 100;
    const int alpha = std::max(58, 256 - smooth * 190 / 100);
    const int low_mix = std::min(190, (warmth * wet / 100 + smooth) * 150 / 100);
    const int drive = 100 + warmth * wet / 250 + wet / 8;
    const int trim = std::max(72, 100 - wet / 9);
    const int noise_amp = noise * wet / 12;
    const int wobble_depth = wobble * wet / 70;
    const int space_mix = channels == 2 ? space * wet * 96 / 10000 : 0;
    const int texture = (wet + noise + softness) / 3;
    const int crush_step = 1 << std::min(8, 2 + texture / 14);

    for (size_t i = 0; i < sample_count; i += static_cast<size_t>(channels)) {
        if (wobble_depth > 0 && (state.frame_index & 0x3f) == 0) {
            const int rnd = static_cast<int>((next_noise(state.noise_state) >> 16) & 0x1ff) - 256;
            state.wobble_target = rnd * wobble_depth / 120;
        }
        state.wobble += (state.wobble_target - state.wobble) / 16;
        const int wobble_gain = 1024 + state.wobble;

        for (int channel = 0; channel < channels && i + static_cast<size_t>(channel) < sample_count; ++channel) {
            const size_t sample_index = i + static_cast<size_t>(channel);
            int value = samples[sample_index];
            int32_t &low = state.lowpass[channel];
            low += (static_cast<int32_t>(value) - low) * alpha / 256;

            if (low_mix > 0) {
                value = (value * (256 - low_mix) + static_cast<int>(low) * low_mix) / 256;
            }
            value = soft_clip(value * drive / 100);
            if (noise_amp > 0) {
                const int hiss = static_cast<int>((next_noise(state.noise_state) >> 16) & 0x7ff) - 1024;
                value += hiss * noise_amp / 1024;
            }
            if (wobble_depth > 0) {
                value = value * wobble_gain / 1024;
            }
            if (crush_step > 4) {
                value = value >= 0 ? (value / crush_step) * crush_step
                                   : -(((-value) / crush_step) * crush_step);
            }
            value = value * trim / 100;
            samples[sample_index] = clamp_i16(value);
        }

        if (space_mix > 0 && i + 1 < sample_count) {
            const int left = samples[i];
            const int right = samples[i + 1];
            const int mid = (left + right) / 2;
            const int side = (left - right) / 2;
            const int widened = side * (256 + space_mix) / 256;
            samples[i] = clamp_i16(mid + widened);
            samples[i + 1] = clamp_i16(mid - widened);
        }
        ++state.frame_index;
    }
}

} // namespace lofi
