#pragma once

#include "lofi_core.hpp"

#include "esp_err.h"

namespace lofi_audio {

enum class Status {
    Idle,
    Playing,
    Paused,
    Ended,
    Unsupported,
    Error,
};

struct Snapshot {
    Status status = Status::Idle;
    int position_seconds = 0;
    char message[64] = "-";
};

esp_err_t init(void);
esp_err_t play_track(const lofi::Track &track, int volume, const lofi::LofiProfile &profile, int start_seconds = 0);
void set_paused(bool paused);
void stop(void);
void set_volume(int volume);
void set_profile(const lofi::LofiProfile &profile);
void seek(int seconds);
Snapshot snapshot(void);

} // namespace lofi_audio
