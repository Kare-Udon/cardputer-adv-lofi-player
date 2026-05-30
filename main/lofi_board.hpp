#pragma once

#include "lofi_core.hpp"

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

namespace lofi_board {

esp_err_t init_display(void);
esp_err_t draw_screen(const lofi::ScreenModel &screen);
void set_screen_brightness_percent(int percent);
void set_screen_awake(bool awake);
bool screen_awake(void);
void tick_display(void);
#if LOFI_DEBUG_AUTOMATION_ENABLED
void dump_framebuffer_to_serial(void);
void set_framebuffer_capture_enabled(bool enabled);
#endif
void release_album_art_cache(void);

struct KeyboardDiagnostics {
    bool ready = false;
    esp_err_t init_err = ESP_ERR_INVALID_STATE;
    esp_err_t bus_err = ESP_ERR_INVALID_STATE;
    esp_err_t probe_err = ESP_ERR_INVALID_STATE;
    const char *stage = "not-started";
    bool input_task_started = false;
    uint32_t queue_depth = 0;
    uint32_t queued_events = 0;
    uint32_t consumed_events = 0;
    uint32_t dropped_repeats = 0;
    uint32_t dropped_events = 0;
    uint32_t last_event_age_ms = 0;
    uint32_t max_event_age_ms = 0;
};

esp_err_t init_keyboard(void);
bool poll_action(lofi::Action &action, const char **key_name);
#if LOFI_DEBUG_AUTOMATION_ENABLED
KeyboardDiagnostics keyboard_diagnostics(void);
#endif
esp_err_t probe_i2c_device(uint8_t addr);
esp_err_t read_i2c_reg(uint8_t addr, uint8_t reg, uint8_t *data, size_t len);
esp_err_t write_i2c_reg(uint8_t addr, uint8_t reg, uint8_t value);

} // namespace lofi_board
