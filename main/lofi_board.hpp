#pragma once

#include "lofi_core.hpp"

#include <cstddef>

#include "esp_err.h"

namespace lofi_board {

esp_err_t init_display(void);
esp_err_t draw_screen(const lofi::ScreenModel &screen);
void set_screen_brightness_percent(int percent);
void set_screen_awake(bool awake);
bool screen_awake(void);
void tick_display(void);
void dump_framebuffer_to_serial(void);

struct KeyboardDiagnostics {
    bool ready = false;
    esp_err_t init_err = ESP_ERR_INVALID_STATE;
    esp_err_t bus_err = ESP_ERR_INVALID_STATE;
    esp_err_t probe_err = ESP_ERR_INVALID_STATE;
    const char *stage = "not-started";
};

esp_err_t init_keyboard(void);
bool poll_action(lofi::Action &action, const char **key_name);
KeyboardDiagnostics keyboard_diagnostics(void);
esp_err_t probe_i2c_device(uint8_t addr);
esp_err_t read_i2c_reg(uint8_t addr, uint8_t reg, uint8_t *data, size_t len);
esp_err_t write_i2c_reg(uint8_t addr, uint8_t reg, uint8_t value);

} // namespace lofi_board
