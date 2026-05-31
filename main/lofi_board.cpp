#include "lofi_board.hpp"

#include "board_pins.h"
#include "lofi_input.hpp"
#include "lofi_volume_icons.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/ledc.h"
#include "lvgl.h"

extern "C" const lv_font_t lofi_font_fusion_pixel_10;
extern "C" const lv_font_t lofi_font_fusion_pixel_12;
extern "C" const lv_font_t lofi_font_fusion_pixel_14;
extern "C" const lv_font_t lofi_font_fusion_pixel_8;
extern "C" const lv_font_t lofi_font_fusion_pixel_ui_12;
extern "C" const lv_font_t lofi_font_fusion_pixel_ui_16;
extern "C" const lv_font_t lofi_font_fusion_pixel_ui_20;
extern "C" const lv_image_dsc_t boot_cassette_rgb565;
extern "C" const lv_image_dsc_t library_center_rgb565;
extern "C" const lv_image_dsc_t library_side_rgb565;
extern "C" const lv_image_dsc_t lofi_center_rgb565;
extern "C" const lv_image_dsc_t lofi_side_rgb565;
extern "C" const lv_image_dsc_t now_center_rgb565;
extern "C" const lv_image_dsc_t now_side_rgb565;
extern "C" const lv_image_dsc_t queue_center_rgb565;
extern "C" const lv_image_dsc_t queue_side_rgb565;
extern "C" const lv_image_dsc_t settings_center_rgb565;
extern "C" const lv_image_dsc_t settings_side_rgb565;

namespace lofi_board {
namespace {

const char *TAG = "lofi_board";

#ifndef LOFI_INPUT_TRACE_LOGS
#define LOFI_INPUT_TRACE_LOGS 0
#endif

#if LOFI_INPUT_TRACE_LOGS
#define LOFI_KBD_TRACE(fmt, ...) ESP_LOGI(TAG, fmt, __VA_ARGS__)
#else
#define LOFI_KBD_TRACE(fmt, ...) do { } while (0)
#endif

struct KeyboardEvent {
    lofi::Action action = lofi::Action::None;
    char key_name[12] = "-";
    bool repeated = false;
    uint16_t repeat_count = 0;
    TickType_t tick = 0;
};

esp_lcd_panel_handle_t s_lcd_panel = nullptr;
esp_lcd_panel_io_handle_t s_lcd_panel_io = nullptr;
i2c_master_bus_handle_t s_i2c_bus = nullptr;
SemaphoreHandle_t s_i2c_mutex = nullptr;
QueueHandle_t s_keyboard_event_queue = nullptr;
TaskHandle_t s_keyboard_task = nullptr;
bool s_keyboard_int_handler_added = false;
bool s_keyboard_ready = false;
bool s_fn_down = false;
char s_last_key_name[12] = "-";
char s_polled_key_name[12] = "-";
KeyboardDiagnostics s_keyboard_diag;
bool s_hold_active = false;
lofi::Action s_hold_action = lofi::Action::None;
char s_hold_key_name[12] = "-";
TickType_t s_hold_started_tick = 0;
TickType_t s_hold_last_emit_tick = 0;
uint16_t s_hold_repeat_count = 0;
bool s_last_hardware_event_repeated = false;
uint32_t s_keyboard_queued_events = 0;
uint32_t s_keyboard_consumed_events = 0;
uint32_t s_keyboard_dropped_repeats = 0;
uint32_t s_keyboard_dropped_events = 0;
uint32_t s_keyboard_last_event_age_ms = 0;
uint32_t s_keyboard_max_event_age_ms = 0;
#if LOFI_DEBUG_AUTOMATION_ENABLED
uint16_t *s_shadow_framebuffer = nullptr;
bool s_shadow_framebuffer_valid = false;
uint32_t s_framebuffer_dump_seq = 0;
#endif
lv_display_t *s_lvgl_display = nullptr;
esp_timer_handle_t s_lvgl_tick_timer = nullptr;
alignas(4) uint16_t s_lvgl_draw_buffer[LCD_WIDTH * 36] = {};
volatile bool s_lvgl_flush_pending = false;
bool s_screen_awake = true;
int s_screen_brightness_percent = 100;
bool s_backlight_pwm_ready = false;
uint16_t *s_album_art_pixels = nullptr;
lv_image_dsc_t s_album_art_dsc = {};
std::string s_album_art_cache_path;
std::string s_album_art_failed_path;
bool s_album_art_valid = false;

constexpr ledc_mode_t kBacklightLedcMode = LEDC_LOW_SPEED_MODE;
constexpr ledc_timer_t kBacklightLedcTimer = LEDC_TIMER_0;
constexpr ledc_channel_t kBacklightLedcChannel = LEDC_CHANNEL_0;
constexpr ledc_timer_bit_t kBacklightLedcResolution = LEDC_TIMER_10_BIT;
constexpr uint32_t kBacklightLedcMaxDuty = (1U << 10) - 1U;
constexpr UBaseType_t kKeyboardTaskPriority = 6;
constexpr uint32_t kKeyboardTaskStackBytes = 4096;
constexpr UBaseType_t kKeyboardEventQueueLength = 8;
constexpr TickType_t kKeyboardPollHoldDelay = 1;
constexpr TickType_t kKeyboardPollFallbackInterval = pdMS_TO_TICKS(25);

struct MarqueeState {
    std::string text;
    uint32_t started_ms = 0;
};

struct MarqueeTiming {
    uint32_t lead_hold_ms = 2200;
    uint32_t tail_hold_ms = 1600;
    uint32_t start_delay_ms = 0;
    uint16_t speed_px_per_sec = 18;
    uint16_t cell_step_ms = 780;
};

MarqueeState s_title_marquee;
MarqueeState s_artist_marquee;
MarqueeState s_queue_marquee;
MarqueeState s_library_marquee;
int s_home_last_selected = -1;
std::string s_last_panel_page_title;

constexpr int kAlbumArtWidth = 72;
constexpr int kAlbumArtHeight = 72;

uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return ((uint16_t)(r & 0xF8) << 8) | ((uint16_t)(g & 0xFC) << 3) | (uint16_t)(b >> 3);
}

uint16_t lcd_color565(uint8_t r, uint8_t g, uint8_t b)
{
    uint16_t color = rgb565(r, g, b);
    return (uint16_t)((color << 8) | (color >> 8));
}

int normalize_backlight_percent(int percent)
{
    return std::max(10, std::min(100, ((percent + 5) / 10) * 10));
}

uint32_t backlight_duty_for_percent(int percent)
{
    percent = normalize_backlight_percent(percent);
    if (percent >= 100) {
        return kBacklightLedcMaxDuty;
    }

    // The Cardputer Adv BL enable path is only useful near full duty:
    // physical testing showed 90% duty is already about half brightness and
    // 80% duty can effectively turn the backlight off.
    constexpr uint32_t kUsableMinDutyPercent = 92;
    constexpr uint32_t kUsableSpanPercent = 100 - kUsableMinDutyPercent;
    const uint32_t duty_percent =
        kUsableMinDutyPercent +
        (static_cast<uint32_t>(percent - 10) * kUsableSpanPercent + 45U) / 90U;
    return kBacklightLedcMaxDuty * duty_percent / 100U;
}

uint32_t backlight_duty_for_state(void)
{
    if (!s_screen_awake) {
        return 0;
    }
    return backlight_duty_for_percent(s_screen_brightness_percent);
}

void apply_backlight(void)
{
    const uint32_t duty = backlight_duty_for_state();
    if (s_backlight_pwm_ready) {
        ledc_set_duty(kBacklightLedcMode, kBacklightLedcChannel, duty);
        ledc_update_duty(kBacklightLedcMode, kBacklightLedcChannel);
    } else {
        gpio_set_level(static_cast<gpio_num_t>(PIN_LCD_BL), duty > 0 ? 1 : 0);
    }
}

esp_err_t lcd_draw_solid(uint16_t color)
{
    if (!s_lcd_panel) {
        return ESP_ERR_INVALID_STATE;
    }

    uint16_t line[LCD_WIDTH];
    for (int i = 0; i < LCD_WIDTH; ++i) {
        line[i] = color;
    }
    for (int y = 0; y < LCD_HEIGHT; ++y) {
        ESP_RETURN_ON_ERROR(esp_lcd_panel_draw_bitmap(s_lcd_panel, 0, y, LCD_WIDTH, y + 1, line), TAG, "draw line");
#if LOFI_DEBUG_AUTOMATION_ENABLED
        if (s_shadow_framebuffer) {
            std::fill_n(&s_shadow_framebuffer[y * LCD_WIDTH], LCD_WIDTH, color);
        }
#endif
    }
#if LOFI_DEBUG_AUTOMATION_ENABLED
    if (s_shadow_framebuffer) {
        s_shadow_framebuffer_valid = true;
    }
#endif
    return ESP_OK;
}

void wait_for_lvgl_flush_idle(uint32_t timeout_ms = 30)
{
    const int64_t deadline = esp_timer_get_time() + static_cast<int64_t>(timeout_ms) * 1000;
    while (s_lvgl_flush_pending && esp_timer_get_time() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

esp_err_t preclear_panel_on_page_change(const std::string &title, uint16_t bg)
{
    if (s_last_panel_page_title == title) {
        return ESP_OK;
    }
    wait_for_lvgl_flush_idle();
    ESP_RETURN_ON_ERROR(lcd_draw_solid(bg), TAG, "page preclear");
    wait_for_lvgl_flush_idle();
    s_last_panel_page_title = title;
    return ESP_OK;
}

esp_err_t ensure_i2c_mutex(void)
{
    if (!s_i2c_mutex) {
        s_i2c_mutex = xSemaphoreCreateMutex();
    }
    return s_i2c_mutex ? ESP_OK : ESP_ERR_NO_MEM;
}

bool lock_i2c(void)
{
    return ensure_i2c_mutex() == ESP_OK && xSemaphoreTake(s_i2c_mutex, pdMS_TO_TICKS(500)) == pdTRUE;
}

void unlock_i2c(void)
{
    if (s_i2c_mutex) {
        xSemaphoreGive(s_i2c_mutex);
    }
}

#if LOFI_DEBUG_AUTOMATION_ENABLED
uint32_t framebuffer_hash(void)
{
    uint32_t hash = 2166136261u;
    if (!s_shadow_framebuffer) {
        return hash;
    }
    for (size_t i = 0; i < LCD_WIDTH * LCD_HEIGHT; ++i) {
        const uint16_t word = s_shadow_framebuffer[i];
        hash ^= static_cast<uint8_t>(word & 0xff);
        hash *= 16777619u;
        hash ^= static_cast<uint8_t>(word >> 8);
        hash *= 16777619u;
    }
    return hash;
}
#endif

std::string strip_selection_marker(const std::string &value)
{
    size_t pos = 0;
    while (pos < value.size() && (value[pos] == '>' || value[pos] == '*' || value[pos] == ' ')) {
        ++pos;
    }
    return value.substr(pos);
}

uint32_t now_ms(void)
{
    return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
}

void reset_marquee_if_needed(MarqueeState &state, const std::string &text, uint32_t now)
{
    if (state.text != text) {
        state.text = text;
        state.started_ms = now;
    }
}

uint32_t marquee_phase_ms(MarqueeState &state,
                          const std::string &text,
                          uint32_t now,
                          uint32_t scroll_span,
                          uint32_t speed_per_sec,
                          const MarqueeTiming &timing)
{
    reset_marquee_if_needed(state, text, now);
    const uint32_t speed = std::max<uint32_t>(1, speed_per_sec);
    const uint32_t scroll_ms = std::max<uint32_t>(1, (scroll_span * 1000U + speed - 1U) / speed);
    const uint32_t cycle_ms = timing.start_delay_ms + timing.lead_hold_ms + scroll_ms + timing.tail_hold_ms;
    return cycle_ms > 0 ? (now - state.started_ms) % cycle_ms : 0;
}

uint32_t marquee_offset(uint32_t phase_ms,
                        uint32_t scroll_span,
                        uint32_t speed_per_sec,
                        const MarqueeTiming &timing)
{
    const uint32_t scroll_start = timing.start_delay_ms + timing.lead_hold_ms;
    if (phase_ms < scroll_start) {
        return 0;
    }
    const uint32_t speed = std::max<uint32_t>(1, speed_per_sec);
    const uint32_t scrolled = (phase_ms - scroll_start) * speed / 1000U;
    return std::min<uint32_t>(scroll_span, scrolled);
}

std::string localize_right_label(const std::string &value)
{
    return value;
}

int parse_position_seconds(const lofi::ScreenModel &screen)
{
    for (const lofi::ScreenLine &row : screen.rows) {
        const std::string prefix = "Pos ";
        if (row.left.rfind(prefix, 0) == 0) {
            return std::max(0, std::atoi(row.left.c_str() + prefix.size()));
        }
    }
    return 0;
}

std::string format_mmss(int seconds)
{
    seconds = std::max(0, seconds);
    char buf[16] = {};
    std::snprintf(buf, sizeof(buf), "%02d:%02d", (seconds / 60) % 100, seconds % 60);
    return std::string(buf);
}

std::string lofi_strength_summary(const std::string &status, const std::string &meta = "")
{
    if (meta.rfind("Intensity ", 0) == 0) {
        return meta;
    }
    const size_t lf_pos = status.find("LF ");
    if (lf_pos == std::string::npos) {
        return "Intensity --";
    }
    const std::string preset = status.substr(lf_pos + 3);
    if (preset.find("Off") == 0) {
        return "Intensity 0/10";
    }
    if (preset.find("Warm Tape") == 0) {
        return "Intensity 7/10";
    }
    if (preset.find("Vinyl Cafe") == 0 || preset.find("Rainy Window") == 0 || preset.find("Tiny Radio") == 0) {
        return "Intensity 8/10";
    }
    if (preset.find("Late Night") == 0) {
        return "Intensity 6/10";
    }
    if (preset.find("Custom") == 0) {
        return "Intensity custom";
    }
    return "Intensity --";
}

esp_err_t try_new_i2c_bus(i2c_port_num_t port, const char *stage)
{
    i2c_master_bus_config_t bus_config = {};
    bus_config.i2c_port = port;
    bus_config.sda_io_num = static_cast<gpio_num_t>(PIN_I2C_SDA);
    bus_config.scl_io_num = static_cast<gpio_num_t>(PIN_I2C_SCL);
    bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_config.glitch_ignore_cnt = 7;
    bus_config.trans_queue_depth = 0;
    bus_config.flags.enable_internal_pullup = true;
    esp_err_t err = i2c_new_master_bus(&bus_config, &s_i2c_bus);
    s_keyboard_diag.stage = stage;
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Initialized I2C bus port=%d stage=%s", port, stage);
    }
    return err;
}

esp_err_t board_i2c_init(void)
{
    if (s_i2c_bus) {
        s_keyboard_diag.bus_err = ESP_OK;
        return ESP_OK;
    }

    esp_err_t err = try_new_i2c_bus(I2C_PORT, "bus");
    if (err == ESP_ERR_INVALID_STATE) {
        i2c_master_bus_handle_t existing = nullptr;
        const esp_err_t get_err = i2c_master_get_bus_handle(I2C_PORT, &existing);
        if (get_err == ESP_OK && existing != nullptr) {
            s_i2c_bus = existing;
            err = ESP_OK;
            ESP_LOGI(TAG, "Reusing existing I2C bus %d", I2C_PORT);
        } else {
            ESP_LOGW(TAG, "I2C port %d unavailable: new=%s get=%s; trying auto port",
                     I2C_PORT, esp_err_to_name(err), esp_err_to_name(get_err));
            err = try_new_i2c_bus(static_cast<i2c_port_num_t>(-1), "bus-auto");
        }
    }

    s_keyboard_diag.bus_err = err;
    if (err == ESP_OK) {
        err = ensure_i2c_mutex();
        s_keyboard_diag.bus_err = err;
    }
    return err;
}

esp_err_t i2c_probe(uint8_t addr)
{
    if (!s_i2c_bus) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!lock_i2c()) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = i2c_master_probe(s_i2c_bus, addr, pdMS_TO_TICKS(300));
    unlock_i2c();
    return err;
}

esp_err_t i2c_read_reg(uint8_t addr, uint8_t reg, uint8_t *data, size_t len)
{
    if (!s_i2c_bus) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!lock_i2c()) {
        return ESP_ERR_TIMEOUT;
    }
    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = addr;
    dev_cfg.scl_speed_hz = 100000;
    dev_cfg.scl_wait_us = 20000;

    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < 3; ++attempt) {
        i2c_master_dev_handle_t dev = nullptr;
        err = i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &dev);
        if (err == ESP_OK) {
            err = i2c_master_transmit_receive(dev, &reg, 1, data, len, pdMS_TO_TICKS(200));
            i2c_master_bus_rm_device(dev);
        }
        if (err == ESP_OK) {
            break;
        }
        i2c_master_bus_reset(s_i2c_bus);
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    unlock_i2c();
    return err;
}

esp_err_t i2c_write_reg(uint8_t addr, uint8_t reg, uint8_t value)
{
    if (!s_i2c_bus) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!lock_i2c()) {
        return ESP_ERR_TIMEOUT;
    }
    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = addr;
    dev_cfg.scl_speed_hz = 100000;
    dev_cfg.scl_wait_us = 20000;

    uint8_t data[2] = {reg, value};
    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < 3; ++attempt) {
        i2c_master_dev_handle_t dev = nullptr;
        err = i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &dev);
        if (err == ESP_OK) {
            err = i2c_master_transmit(dev, data, sizeof(data), pdMS_TO_TICKS(200));
            i2c_master_bus_rm_device(dev);
        }
        if (err == ESP_OK) {
            break;
        }
        i2c_master_bus_reset(s_i2c_bus);
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    unlock_i2c();
    return err;
}

#define TCA8418_REG_CFG 0x01
#define TCA8418_REG_INT_STAT 0x02
#define TCA8418_REG_KEY_LCK_EC 0x03
#define TCA8418_REG_KEY_EVENT_A 0x04
#define TCA8418_REG_GPIO_INT_STAT_1 0x11
#define TCA8418_REG_GPIO_INT_STAT_2 0x12
#define TCA8418_REG_GPIO_INT_STAT_3 0x13
#define TCA8418_REG_GPIO_INT_EN_1 0x1A
#define TCA8418_REG_GPIO_INT_EN_2 0x1B
#define TCA8418_REG_GPIO_INT_EN_3 0x1C
#define TCA8418_REG_KP_GPIO_1 0x1D
#define TCA8418_REG_KP_GPIO_2 0x1E
#define TCA8418_REG_KP_GPIO_3 0x1F
#define TCA8418_REG_GPI_EM_1 0x20
#define TCA8418_REG_GPI_EM_2 0x21
#define TCA8418_REG_GPI_EM_3 0x22
#define TCA8418_REG_GPIO_DIR_1 0x23
#define TCA8418_REG_GPIO_DIR_2 0x24
#define TCA8418_REG_GPIO_DIR_3 0x25
#define TCA8418_REG_GPIO_INT_LVL_1 0x26
#define TCA8418_REG_GPIO_INT_LVL_2 0x27
#define TCA8418_REG_GPIO_INT_LVL_3 0x28
#define TCA8418_REG_DEBOUNCE_DIS_1 0x29
#define TCA8418_REG_DEBOUNCE_DIS_2 0x2A
#define TCA8418_REG_DEBOUNCE_DIS_3 0x2B
#define TCA8418_REG_CFG_GPI_IEN 0x02
#define TCA8418_REG_CFG_KE_IEN 0x01

esp_err_t tca8418_flush(void)
{
    uint8_t count = 0;
    if (i2c_read_reg(I2C_ADDR_TCA8418, TCA8418_REG_KEY_LCK_EC, &count, 1) != ESP_OK) {
        ESP_LOGW(TAG, "kbd flush count read failed; continuing");
        i2c_write_reg(I2C_ADDR_TCA8418, TCA8418_REG_INT_STAT, 0x03);
        return ESP_OK;
    }

    uint8_t event = 0;
    for (int i = 0; i < (count & 0x0F) && i < 16; ++i) {
        if (i2c_read_reg(I2C_ADDR_TCA8418, TCA8418_REG_KEY_EVENT_A, &event, 1) != ESP_OK) {
            ESP_LOGW(TAG, "kbd flush event read failed; continuing");
            break;
        }
        if (event == 0) {
            break;
        }
    }
    uint8_t scratch = 0;
    i2c_read_reg(I2C_ADDR_TCA8418, TCA8418_REG_GPIO_INT_STAT_1, &scratch, 1);
    i2c_read_reg(I2C_ADDR_TCA8418, TCA8418_REG_GPIO_INT_STAT_2, &scratch, 1);
    i2c_read_reg(I2C_ADDR_TCA8418, TCA8418_REG_GPIO_INT_STAT_3, &scratch, 1);
    i2c_write_reg(I2C_ADDR_TCA8418, TCA8418_REG_INT_STAT, 0x03);
    return ESP_OK;
}

void keyboard_remap(uint8_t raw, uint8_t *row, uint8_t *col)
{
    uint16_t pos = raw & 0x7F;
    if (pos > 0) {
        --pos;
    }
    uint8_t raw_row = pos / 10;
    uint8_t raw_col = pos % 10;
    uint8_t mapped_col = raw_row * 2;
    if (raw_col > 3) {
        ++mapped_col;
    }
    *row = (raw_col + 4) % 4;
    *col = mapped_col;
}

const char *keyboard_key_name(uint8_t row, uint8_t col)
{
    static const char *const names[4][14] = {
        {"`", "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "-", "=", "DEL"},
        {"TAB", "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "[", "]", "\\"},
        {"FN", "SHIFT", "A", "S", "D", "F", "G", "H", "J", "K", "L", ";", "'", "ENTER"},
        {"CTRL", "OPT", "ALT", "Z", "X", "C", "V", "B", "N", "M", ",", ".", "/", "SPACE"},
    };
    if (row >= 4 || col >= 14) {
        return "?";
    }
    return names[row][col];
}

lofi::Action keyboard_action_from_key(uint8_t row, uint8_t col)
{
    if (row == 2 && col == 0) {
        return lofi::Action::None;
    }
    if (s_fn_down) {
        if (row == 2 && col == 11) {
            return lofi::Action::Up;
        }
        if (row == 3 && col == 11) {
            return lofi::Action::Down;
        }
        if (row == 3 && col == 10) {
            return lofi::Action::SeekBackward;
        }
        if (row == 3 && col == 12) {
            return lofi::Action::SeekForward;
        }
        if (row == 0 && col == 0) {
            return lofi::Action::Back;
        }
    }
    if ((row == 1 && col == 2) || (row == 2 && col == 11)) {
        return lofi::Action::Up;
    }
    if (row == 2 && col == 3) {
        return lofi::Action::Shuffle;
    }
    if ((row == 3 && col == 4) || (row == 3 && col == 11)) {
        return lofi::Action::Down;
    }
    if ((row == 2 && col == 2) || (row == 3 && col == 10)) {
        return lofi::Action::Left;
    }
    if ((row == 2 && col == 4) || (row == 3 && col == 12)) {
        return lofi::Action::Right;
    }
    if (row == 2 && col == 7) {
        return lofi::Action::Help;
    }
    if (row == 1 && col == 4) {
        return lofi::Action::Repeat;
    }
    if (row == 2 && col == 10) {
        return lofi::Action::Lofi;
    }
    if (row == 3 && col == 9) {
        return lofi::Action::Menu;
    }
    if ((row == 2 && col == 13) || (row == 3 && col == 13)) {
        return lofi::Action::Ok;
    }
    if (row == 0 && col == 13) {
        return lofi::Action::Back;
    }
    return lofi::Action::None;
}

bool keyboard_action_repeats(lofi::Action action)
{
    return lofi_input::action_repeats(action);
}

TickType_t keyboard_initial_repeat_delay(lofi::Action action)
{
    return pdMS_TO_TICKS(lofi_input::initial_repeat_delay_ms(action));
}

TickType_t keyboard_repeat_interval(lofi::Action action, uint32_t repeat_count)
{
    return pdMS_TO_TICKS(lofi_input::repeat_interval_ms(action, repeat_count));
}

void keyboard_hold_begin(lofi::Action action, const char *key_name)
{
    if (!keyboard_action_repeats(action)) {
        s_hold_active = false;
        s_hold_action = lofi::Action::None;
        return;
    }
    s_hold_active = true;
    s_hold_action = action;
    std::strncpy(s_hold_key_name, key_name, sizeof(s_hold_key_name) - 1);
    s_hold_key_name[sizeof(s_hold_key_name) - 1] = '\0';
    s_hold_started_tick = xTaskGetTickCount();
    s_hold_last_emit_tick = s_hold_started_tick;
    s_hold_repeat_count = 0;
    LOFI_KBD_TRACE("KBD_HOLD_BEGIN key=%s action=%d repeat=%d initial_ms=%u",
                   s_hold_key_name,
                   static_cast<int>(action),
                   keyboard_action_repeats(action) ? 1 : 0,
                   static_cast<unsigned>(keyboard_initial_repeat_delay(action) * portTICK_PERIOD_MS));
}

void keyboard_hold_end(const char *key_name)
{
    if (s_hold_active && std::strcmp(s_hold_key_name, key_name) == 0) {
#if LOFI_INPUT_TRACE_LOGS
        const TickType_t now = xTaskGetTickCount();
        LOFI_KBD_TRACE("KBD_HOLD_END key=%s held_ms=%u repeats=%u",
                       key_name,
                       static_cast<unsigned>((now - s_hold_started_tick) * portTICK_PERIOD_MS),
                       static_cast<unsigned>(s_hold_repeat_count));
#endif
        s_hold_active = false;
        s_hold_action = lofi::Action::None;
        s_hold_key_name[0] = '-';
        s_hold_key_name[1] = '\0';
    }
}

bool keyboard_hold_repeat(lofi::Action &action, const char **key_name)
{
    if (!s_hold_active || s_hold_action == lofi::Action::None) {
        return false;
    }
    const TickType_t now = xTaskGetTickCount();
    const TickType_t held = now - s_hold_started_tick;
    if (held < keyboard_initial_repeat_delay(s_hold_action)) {
        return false;
    }

    const TickType_t interval = keyboard_repeat_interval(s_hold_action, s_hold_repeat_count);
    const TickType_t since_last = now - s_hold_last_emit_tick;
    if (since_last < interval) {
        return false;
    }

    s_hold_last_emit_tick = now;
    ++s_hold_repeat_count;
    action = s_hold_action;
    s_last_hardware_event_repeated = true;
    std::strncpy(s_last_key_name, s_hold_key_name, sizeof(s_last_key_name) - 1);
    s_last_key_name[sizeof(s_last_key_name) - 1] = '\0';
    if (key_name) {
        *key_name = s_last_key_name;
    }
    LOFI_KBD_TRACE("KBD_REPEAT key=%s action=%d count=%u held_ms=%u since_last_ms=%u target_ms=%u",
                   s_last_key_name,
                   static_cast<int>(action),
                   static_cast<unsigned>(s_hold_repeat_count),
                   static_cast<unsigned>(held * portTICK_PERIOD_MS),
                   static_cast<unsigned>(since_last * portTICK_PERIOD_MS),
                   static_cast<unsigned>(interval * portTICK_PERIOD_MS));
    return true;
}

bool keyboard_poll_hardware(lofi::Action &action, const char **key_name)
{
    if (!s_keyboard_ready) {
        return false;
    }

    uint8_t count = 0;
    if (i2c_read_reg(I2C_ADDR_TCA8418, TCA8418_REG_KEY_LCK_EC, &count, 1) != ESP_OK || (count & 0x0F) == 0) {
        return keyboard_hold_repeat(action, key_name);
    }

    uint8_t raw = 0;
    if (i2c_read_reg(I2C_ADDR_TCA8418, TCA8418_REG_KEY_EVENT_A, &raw, 1) != ESP_OK || raw == 0) {
        return keyboard_hold_repeat(action, key_name);
    }
    i2c_write_reg(I2C_ADDR_TCA8418, TCA8418_REG_INT_STAT, 0x01);

    uint8_t row = 0;
    uint8_t col = 0;
    keyboard_remap(raw, &row, &col);
    bool pressed = (raw & 0x80) != 0;
    if (row == 2 && col == 0) {
        s_fn_down = pressed;
    }
    std::strncpy(s_last_key_name, keyboard_key_name(row, col), sizeof(s_last_key_name) - 1);
    s_last_key_name[sizeof(s_last_key_name) - 1] = '\0';
    if (key_name) {
        *key_name = s_last_key_name;
    }
    const lofi::Action mapped_action = keyboard_action_from_key(row, col);
    LOFI_KBD_TRACE("KBD raw=0x%02x fifo=%u key=%s pressed=%d row=%u col=%u action=%d tick=%u",
                   raw,
                   static_cast<unsigned>(count & 0x0F),
                   s_last_key_name,
                   pressed ? 1 : 0,
                   static_cast<unsigned>(row),
                   static_cast<unsigned>(col),
                   static_cast<int>(mapped_action),
                   static_cast<unsigned>(xTaskGetTickCount()));
    if (!pressed) {
        keyboard_hold_end(s_last_key_name);
        return false;
    }

    action = mapped_action;
    s_last_hardware_event_repeated = false;
    keyboard_hold_begin(action, s_last_key_name);
    if (action == lofi::Action::None) {
        if (std::strcmp(s_last_key_name, "C") == 0) {
            return true;
        }
        return false;
    }
    return true;
}

void enqueue_keyboard_event(lofi::Action action, const char *key_name, bool repeated, uint16_t repeat_count)
{
    if (!s_keyboard_event_queue) {
        return;
    }

    KeyboardEvent event;
    event.action = action;
    std::strncpy(event.key_name, key_name ? key_name : "?", sizeof(event.key_name) - 1);
    event.key_name[sizeof(event.key_name) - 1] = '\0';
    event.repeated = repeated;
    event.repeat_count = repeat_count;
    event.tick = xTaskGetTickCount();

    const bool queue_full = uxQueueSpacesAvailable(s_keyboard_event_queue) == 0;
    switch (lofi_input::queue_admission(queue_full, repeated)) {
    case lofi_input::QueueAdmission::Enqueue:
        if (xQueueSend(s_keyboard_event_queue, &event, 0) == pdTRUE) {
            ++s_keyboard_queued_events;
        } else if (repeated) {
            ++s_keyboard_dropped_repeats;
        } else {
            ++s_keyboard_dropped_events;
        }
        break;
    case lofi_input::QueueAdmission::DropIncomingRepeat:
        ++s_keyboard_dropped_repeats;
        break;
    case lofi_input::QueueAdmission::DropOldestThenEnqueue: {
        KeyboardEvent discarded;
        if (xQueueReceive(s_keyboard_event_queue, &discarded, 0) == pdTRUE) {
            ++s_keyboard_dropped_events;
        }
        if (xQueueSend(s_keyboard_event_queue, &event, 0) == pdTRUE) {
            ++s_keyboard_queued_events;
        } else {
            ++s_keyboard_dropped_events;
        }
        break;
    }
    }
}

void keyboard_input_task(void *)
{
    while (true) {
        if (!s_hold_active && gpio_get_level(static_cast<gpio_num_t>(PIN_KBD_INT)) != 0) {
            ulTaskNotifyTake(pdTRUE, kKeyboardPollFallbackInterval);
        }

        lofi::Action action = lofi::Action::None;
        const char *key_name = nullptr;
        if (keyboard_poll_hardware(action, &key_name)) {
            enqueue_keyboard_event(action, key_name, s_last_hardware_event_repeated, s_hold_repeat_count);
            vTaskDelay(kKeyboardPollHoldDelay);
        } else {
            vTaskDelay(s_hold_active ? kKeyboardPollHoldDelay : 1);
        }
    }
}

void IRAM_ATTR keyboard_int_isr(void *)
{
    BaseType_t higher_priority_woken = pdFALSE;
    if (s_keyboard_task) {
        vTaskNotifyGiveFromISR(s_keyboard_task, &higher_priority_woken);
    }
    if (higher_priority_woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

esp_err_t start_keyboard_input_task(void)
{
    if (!s_keyboard_event_queue) {
        s_keyboard_event_queue = xQueueCreate(kKeyboardEventQueueLength, sizeof(KeyboardEvent));
        if (!s_keyboard_event_queue) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (!s_keyboard_task) {
        if (xTaskCreate(keyboard_input_task,
                        "lofi_kbd",
                        kKeyboardTaskStackBytes,
                        nullptr,
                        kKeyboardTaskPriority,
                        &s_keyboard_task) != pdPASS) {
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

esp_err_t enable_keyboard_interrupt(void)
{
    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    if (!s_keyboard_int_handler_added) {
        err = gpio_isr_handler_add(static_cast<gpio_num_t>(PIN_KBD_INT), keyboard_int_isr, nullptr);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            return err;
        }
        s_keyboard_int_handler_added = true;
    }
    ESP_RETURN_ON_ERROR(gpio_set_intr_type(static_cast<gpio_num_t>(PIN_KBD_INT), GPIO_INTR_NEGEDGE),
                        TAG,
                        "keyboard int type");
    ESP_RETURN_ON_ERROR(gpio_intr_enable(static_cast<gpio_num_t>(PIN_KBD_INT)), TAG, "keyboard int enable");
    return ESP_OK;
}

lv_color_t lv_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return lv_color_make(r, g, b);
}

const lv_font_t *lv_font_small(void)
{
    return &lofi_font_fusion_pixel_10;
}

const lv_font_t *lv_font_tiny(void)
{
    return &lofi_font_fusion_pixel_8;
}

const lv_font_t *lv_font_header(void)
{
    return &lofi_font_fusion_pixel_ui_16;
}

const lv_font_t *lv_font_now_chrome(void)
{
    return &lofi_font_fusion_pixel_ui_12;
}

const lv_font_t *lv_font_normal(void)
{
    return &lofi_font_fusion_pixel_ui_16;
}

const lv_font_t *lv_font_title(void)
{
    return &lofi_font_fusion_pixel_ui_20;
}

const lv_font_t *lv_font_symbol_small(void)
{
#if LV_FONT_MONTSERRAT_12
    return &lv_font_montserrat_12;
#else
    return LV_FONT_DEFAULT;
#endif
}

const lv_font_t *lv_font_symbol_normal(void)
{
#if LV_FONT_MONTSERRAT_14
    return &lv_font_montserrat_14;
#else
    return LV_FONT_DEFAULT;
#endif
}

const lv_font_t *lv_font_symbol_title(void)
{
#if LV_FONT_MONTSERRAT_20
    return &lv_font_montserrat_20;
#elif LV_FONT_MONTSERRAT_18
    return &lv_font_montserrat_18;
#else
    return LV_FONT_DEFAULT;
#endif
}

const lv_font_t *lv_font_large_symbol(void)
{
#if LV_FONT_MONTSERRAT_24
    return &lv_font_montserrat_24;
#else
    return lv_font_title();
#endif
}

const lv_font_t *lv_font_home_label(void)
{
    return &lofi_font_fusion_pixel_ui_20;
}

const lv_font_t *lv_font_cjk(void)
{
    return &lofi_font_fusion_pixel_12;
}

const lv_font_t *lv_font_library_cjk(void)
{
    return &lofi_font_fusion_pixel_10;
}

const lv_font_t *lv_font_library_cjk_large(void)
{
    return &lofi_font_fusion_pixel_12;
}

const lv_font_t *lv_font_now_title(void)
{
    return &lofi_font_fusion_pixel_14;
}

void lvgl_tick_timer_cb(void *)
{
    lv_tick_inc(1);
}

bool lvgl_flush_ready_cb(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t *, void *user_ctx)
{
    if (!s_lvgl_flush_pending) {
        return false;
    }
    s_lvgl_flush_pending = false;
    lv_display_t *display = static_cast<lv_display_t *>(user_ctx);
    if (display) {
        lv_display_flush_ready(display);
    }
    return false;
}

void lvgl_flush_cb(lv_display_t *display, const lv_area_t *area, uint8_t *px_map)
{
    if (!s_lcd_panel || !area || !px_map) {
        lv_display_flush_ready(display);
        return;
    }

    const int32_t x1 = std::max<int32_t>(0, area->x1);
    const int32_t y1 = std::max<int32_t>(0, area->y1);
    const int32_t x2 = std::min<int32_t>(LCD_WIDTH - 1, area->x2);
    const int32_t y2 = std::min<int32_t>(LCD_HEIGHT - 1, area->y2);
    if (x1 > x2 || y1 > y2) {
        lv_display_flush_ready(display);
        return;
    }

    const int32_t width = area->x2 - area->x1 + 1;
    const int32_t height = area->y2 - area->y1 + 1;
    uint16_t *pixels = reinterpret_cast<uint16_t *>(px_map);
    const size_t pixel_count = static_cast<size_t>(width * height);
    for (size_t i = 0; i < pixel_count; ++i) {
        const uint16_t color = pixels[i];
        pixels[i] = static_cast<uint16_t>((color << 8) | (color >> 8));
    }

    s_lvgl_flush_pending = true;
    esp_err_t err = esp_lcd_panel_draw_bitmap(s_lcd_panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, pixels);
    if (err != ESP_OK) {
        s_lvgl_flush_pending = false;
        ESP_LOGW(TAG, "LVGL flush failed: %s", esp_err_to_name(err));
        lv_display_flush_ready(display);
    }

#if LOFI_DEBUG_AUTOMATION_ENABLED
    if (s_shadow_framebuffer) {
        for (int32_t row = 0; row < height; ++row) {
            const int32_t dst_y = area->y1 + row;
            if (dst_y < 0 || dst_y >= LCD_HEIGHT) {
                continue;
            }
            const int32_t src_x = x1 - area->x1;
            const int32_t dst_x = x1;
            const int32_t copy_width = x2 - x1 + 1;
            const uint16_t *src = &pixels[row * width + src_x];
            uint16_t *dst = &s_shadow_framebuffer[dst_y * LCD_WIDTH + dst_x];
            std::copy_n(src, copy_width, dst);
        }
        s_shadow_framebuffer_valid = true;
    }
#endif
}

esp_err_t init_lvgl_display(void)
{
    if (s_lvgl_display) {
        return ESP_OK;
    }

    lv_init();
    s_lvgl_display = lv_display_create(LCD_WIDTH, LCD_HEIGHT);
    if (!s_lvgl_display) {
        return ESP_ERR_NO_MEM;
    }
    lv_display_set_default(s_lvgl_display);
    lv_display_set_color_format(s_lvgl_display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(s_lvgl_display, lvgl_flush_cb);
    lv_display_set_buffers(s_lvgl_display,
                           s_lvgl_draw_buffer,
                           nullptr,
                           sizeof(s_lvgl_draw_buffer),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    if (!s_lcd_panel_io) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_lcd_panel_io_callbacks_t cbs = {};
    cbs.on_color_trans_done = lvgl_flush_ready_cb;
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_register_event_callbacks(s_lcd_panel_io, &cbs, s_lvgl_display),
                        TAG,
                        "lvgl flush callback");

    esp_timer_create_args_t tick_args = {};
    tick_args.callback = lvgl_tick_timer_cb;
    tick_args.name = "lvgl_tick";
    esp_err_t err = esp_timer_create(&tick_args, &s_lvgl_tick_timer);
    if (err != ESP_OK) {
        return err;
    }
    return esp_timer_start_periodic(s_lvgl_tick_timer, 1000);
}

lv_obj_t *lv_rect(lv_obj_t *parent,
                  int x,
                  int y,
                  int w,
                  int h,
                  lv_color_t color,
                  int radius = 0,
                  lv_color_t border = lv_color_black(),
                  int border_width = 0)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, color, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_border_color(obj, border, 0);
    lv_obj_set_style_border_width(obj, border_width, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

lv_obj_t *lv_label(lv_obj_t *parent,
                   const std::string &text,
                   int x,
                   int y,
                   int w,
                   lv_color_t color,
                   const lv_font_t *font,
                   lv_label_long_mode_t long_mode = LV_LABEL_LONG_CLIP)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_width(label, w);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_bg_opa(label, LV_OPA_TRANSP, 0);
    lv_label_set_long_mode(label, long_mode);
    lv_label_set_text(label, text.c_str());
    return label;
}

lv_obj_t *lv_clipped_label(lv_obj_t *parent,
                           const std::string &text,
                           int x,
                           int y,
                           int w,
                           int h,
                           int text_y,
                           lv_color_t color,
                           const lv_font_t *font,
                           int scale = 256)
{
    lv_obj_t *clip = lv_obj_create(parent);
    lv_obj_set_pos(clip, x, y);
    lv_obj_set_size(clip, w, h);
    lv_obj_set_style_bg_opa(clip, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(clip, 0, 0);
    lv_obj_set_style_pad_all(clip, 0, 0);
    lv_obj_set_style_radius(clip, 0, 0);
    lv_obj_remove_flag(clip, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = lv_label_create(clip);
    lv_obj_set_pos(label, 1, text_y);
    lv_obj_set_width(label, w);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_bg_opa(label, LV_OPA_TRANSP, 0);
    if (scale != 256) {
        lv_obj_set_style_transform_pivot_x(label, 0, 0);
        lv_obj_set_style_transform_pivot_y(label, 0, 0);
        lv_obj_set_style_transform_scale(label, scale, 0);
    }
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_label_set_text(label, text.c_str());
    return label;
}

int lv_text_width_px(const std::string &text, const lv_font_t *font)
{
    lv_point_t size = {};
    lv_text_get_size(&size, text.c_str(), font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return std::max<int>(0, size.x);
}

lv_obj_t *lv_marquee_label(lv_obj_t *parent,
                           const std::string &text,
                           int x,
                           int y,
                           int w,
                           lv_color_t color,
                           const lv_font_t *font,
                           MarqueeState &state,
                           const MarqueeTiming &timing,
                           int clip_h = 0,
                           int text_y = 0)
{
    const int line_height = std::max<int>(12, lv_font_get_line_height(font));
    lv_obj_t *clip = lv_obj_create(parent);
    lv_obj_set_pos(clip, x, y);
    lv_obj_set_size(clip, w, clip_h > 0 ? clip_h : line_height + 2);
    lv_obj_set_style_bg_opa(clip, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(clip, 0, 0);
    lv_obj_set_style_pad_all(clip, 0, 0);
    lv_obj_set_style_radius(clip, 0, 0);
    lv_obj_remove_flag(clip, LV_OBJ_FLAG_SCROLLABLE);

    const int text_width = lv_text_width_px(text, font);
    const uint32_t now = now_ms();
    int offset = 0;
    if (text_width > w) {
        const uint32_t span_px = static_cast<uint32_t>(text_width - w);
        const uint32_t phase = marquee_phase_ms(state, text, now, span_px, timing.speed_px_per_sec, timing);
        offset = -static_cast<int>(marquee_offset(phase, span_px, timing.speed_px_per_sec, timing));
    } else {
        reset_marquee_if_needed(state, text, now);
    }

    lv_obj_t *label = lv_label_create(clip);
    lv_obj_set_pos(label, offset, text_y);
    lv_obj_set_width(label, std::max(w, text_width + 2));
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_bg_opa(label, LV_OPA_TRANSP, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_label_set_text(label, text.c_str());
    return label;
}

void draw_lvgl_battery(lv_obj_t *root, lv_color_t ink, lv_color_t dim, lv_color_t fill)
{
    const int x = 217;
    const int y = 3;
    lv_rect(root, x + 1, y, 17, 1, dim);
    lv_rect(root, x, y + 1, 1, 8, dim);
    lv_rect(root, x + 18, y + 1, 1, 8, dim);
    lv_rect(root, x + 1, y + 9, 17, 1, dim);
    lv_rect(root, x + 19, y + 3, 2, 4, dim);
    lv_rect(root, x + 4, y + 3, 3, 4, fill);
    lv_rect(root, x + 9, y + 3, 3, 4, fill);
    lv_rect(root, x + 14, y + 3, 3, 4, fill);
    lv_rect(root, x + 21, y + 4, 1, 2, ink);
}

void draw_lvgl_background_task_indicator(lv_obj_t *root, bool active, uint8_t frame, lv_color_t accent, lv_color_t dim)
{
    if (!active) {
        return;
    }
    const int x = 63;
    const int y = 8;
    const uint8_t active_dot = frame % 4;
    for (uint8_t i = 0; i < 4; ++i) {
        const lv_color_t color = i == active_dot ? accent : dim;
        const int dot_y = y + (i == active_dot ? -1 : 0);
        lv_rect(root, x + i * 6, dot_y, 2, 2, color);
    }
}

void draw_lvgl_header(lv_obj_t *root,
                      const std::string &title,
                      lv_color_t chrome,
                      lv_color_t accent,
                      lv_color_t ink,
                      lv_color_t dim,
                      lv_color_t line,
                      lv_color_t battery,
                      bool background_task_active = false,
                      uint8_t background_task_frame = 0)
{
    lv_rect(root, 0, 0, LCD_WIDTH, 18, chrome);
    lv_rect(root, 0, 18, LCD_WIDTH, 1, line);
    lv_label(root, "LOFI", 4, 0, 54, accent, lv_font_header());
    draw_lvgl_background_task_indicator(root, background_task_active, background_task_frame, accent, dim);
    lv_obj_t *page = lv_label(root, title, 103, 0, 106, ink, lv_font_header());
    lv_obj_set_style_text_align(page, LV_TEXT_ALIGN_RIGHT, 0);
    draw_lvgl_battery(root, ink, dim, battery);
}

void draw_lvgl_now_header(lv_obj_t *root,
                          const std::string &title,
                          lv_color_t chrome,
                          lv_color_t accent,
                          lv_color_t ink,
                          lv_color_t dim,
                          lv_color_t line,
                          lv_color_t battery,
                          bool background_task_active = false,
                          uint8_t background_task_frame = 0)
{
    lv_rect(root, 0, 0, LCD_WIDTH, 16, chrome);
    lv_rect(root, 0, 16, LCD_WIDTH, 1, line);
    lv_label(root, "LOFI", 4, 2, 48, accent, lv_font_now_chrome());
    draw_lvgl_background_task_indicator(root, background_task_active, background_task_frame, accent, dim);
    lv_obj_t *page = lv_label(root, title, 111, 2, 98, ink, lv_font_now_chrome());
    lv_obj_set_style_text_align(page, LV_TEXT_ALIGN_RIGHT, 0);
    draw_lvgl_battery(root, ink, dim, battery);
}

struct PlaybackSettingItem {
    std::string label;
    std::string value;
    const char *symbol = "";
    bool adjustable = true;
    int volume = -1;
};

int selected_row_index(const lofi::ScreenModel &screen)
{
    for (size_t i = 0; i < screen.rows.size(); ++i) {
        if (screen.rows[i].left.find('>') != std::string::npos) {
            return static_cast<int>(i);
        }
    }
    return 0;
}

const char *setting_symbol_for_label(const std::string &label)
{
    if (label == "Volume") {
        return LV_SYMBOL_VOLUME_MAX;
    }
    if (label == "Brightness") {
        return LV_SYMBOL_EYE_OPEN;
    }
    if (label == "Repeat") {
        return LV_SYMBOL_LOOP;
    }
    if (label == "Shuffle") {
        return LV_SYMBOL_SHUFFLE;
    }
    if (label == "Sleep Timer") {
        return LV_SYMBOL_AUDIO;
    }
    if (label == "Screen Off") {
        return LV_SYMBOL_EYE_CLOSE;
    }
    if (label == "Queue") {
        return LV_SYMBOL_LIST;
    }
    return LV_SYMBOL_SETTINGS;
}

PlaybackSettingItem setting_item_from_row(const lofi::ScreenLine &row)
{
    PlaybackSettingItem item;
    item.label = strip_selection_marker(row.left);
    item.value = row.right;
    item.symbol = setting_symbol_for_label(item.label);
    item.adjustable = item.label != "Queue";
    if (item.label == "Volume" || item.label == "Brightness") {
        item.volume = std::max(0, std::min(100, std::atoi(row.right.c_str())));
    }
    return item;
}

void draw_cut_corner_frame(lv_obj_t *parent, int x, int y, int w, int h, lv_color_t color, int thickness);

std::string setting_display_label(const std::string &label)
{
    if (label == "Lo-Fi") {
        return "LOFI";
    }
    if (label == "Brightness") {
        return "BRIGHT";
    }
    if (label == "Screen Off") {
        return "SLEEP";
    }
    if (label == "Sleep Timer") {
        return "TIMER";
    }
    std::string out = label;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    return out;
}

void align_center(lv_obj_t *obj)
{
    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, 0);
}

void draw_playback_setting_preview(lv_obj_t *root,
                                   const PlaybackSettingItem &item,
                                   int y,
                                   lv_color_t panel,
                                   lv_color_t ink,
                                   lv_color_t dim,
                                   lv_color_t line)
{
    lv_rect(root, 28, y, 181, 17, panel, 2, line, 1);
    lv_obj_t *icon = lv_label(root, item.symbol, 36, y + 1, 18, dim, lv_font_symbol_small());
    align_center(icon);
    lv_label(root, setting_display_label(item.label), 62, y + 1, 84, dim, lv_font_small());
    lv_obj_t *value = lv_label(root, item.value, 152, y + 1, 48, ink, lv_font_small());
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);
}

void draw_playback_setting_dots(lv_obj_t *root, int selected, int count, lv_color_t accent, lv_color_t line)
{
    const int x = 228;
    const int y = 52;
    for (int i = 0; i < count; ++i) {
        lv_rect(root, x, y + i * 9, 4, 4, i == selected ? accent : line);
    }
}

struct QueuePageInfo {
    int first = 0;
    int last = 0;
    int total = 0;
    int current = 0;
};

QueuePageInfo parse_queue_status(const std::string &status)
{
    QueuePageInfo info;
    const char *text = status.c_str();
    std::sscanf(text, "range=%d-%d/%d current=%d", &info.first, &info.last, &info.total, &info.current);
    return info;
}

std::string library_header_title(const std::string &title)
{
    if (title == "Library Root") {
        return "LIBRARY";
    }
    if (title == "Library Action") {
        return "ACTION";
    }
    if (title == "Album") {
        return "ALBUM";
    }
    if (title == "Artist") {
        return "ARTIST";
    }
    std::string out = title;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    return out;
}

std::string format_library_right(const std::string &right)
{
    if (right.empty() || right == "SD" || right.find('>') != std::string::npos) {
        return right;
    }
    const int seconds = std::atoi(right.c_str());
    if (seconds > 59 && right.find(':') == std::string::npos) {
        return format_mmss(seconds);
    }
    return right;
}

bool contains_non_ascii(const std::string &value)
{
    for (unsigned char ch : value) {
        if (ch >= 0x80) {
            return true;
        }
    }
    return false;
}

void draw_library_header_text(lv_obj_t *root,
                              const lofi::ScreenModel &screen,
                              lv_color_t ink,
                              lv_color_t dim)
{
    const bool subtitle_cjk = contains_non_ascii(screen.subtitle);
    const bool meta_cjk = contains_non_ascii(screen.meta);
    const lv_font_t *subtitle_font = subtitle_cjk ? lv_font_library_cjk() : lv_font_tiny();
    const lv_font_t *meta_font = meta_cjk ? lv_font_library_cjk() : lv_font_tiny();
    const int subtitle_h = subtitle_cjk ? 17 : 12;
    const int meta_h = meta_cjk ? 17 : 12;
    const int subtitle_text_y = subtitle_cjk ? -2 : -1;
    const int meta_text_y = meta_cjk ? -2 : -1;

    if (!screen.subtitle.empty()) {
        lv_clipped_label(root, screen.subtitle, 9, 20, 132, subtitle_h, subtitle_text_y, ink, subtitle_font);
    }
    if (!screen.meta.empty()) {
        lv_obj_t *clip = lv_obj_create(root);
        lv_obj_set_pos(clip, 137, 20);
        lv_obj_set_size(clip, 94, meta_h);
        lv_obj_set_style_bg_opa(clip, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(clip, 0, 0);
        lv_obj_set_style_pad_all(clip, 0, 0);
        lv_obj_set_style_radius(clip, 0, 0);
        lv_obj_remove_flag(clip, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *meta = lv_label_create(clip);
        lv_obj_set_pos(meta, 0, meta_text_y);
        lv_obj_set_width(meta, 94);
        lv_obj_set_style_text_color(meta, dim, 0);
        lv_obj_set_style_text_font(meta, meta_font, 0);
        lv_obj_set_style_text_align(meta, meta_cjk ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_style_bg_opa(meta, LV_OPA_TRANSP, 0);
        lv_label_set_long_mode(meta, LV_LABEL_LONG_CLIP);
        lv_label_set_text(meta, screen.meta.c_str());
    }
}

void draw_lvgl_library_row(lv_obj_t *root,
                           const lofi::ScreenLine &row,
                           int y,
                           bool selected,
                           bool action_style,
                           const lv_font_t *row_font,
                           int row_h,
                           lv_color_t bg,
                           lv_color_t panel,
                           lv_color_t accent,
                           lv_color_t teal,
                           lv_color_t ink,
                           lv_color_t dim,
                           lv_color_t line)
{
    std::string left = strip_selection_marker(row.left);
    const bool checkbox = left.rfind("[x] ", 0) == 0 || left.rfind("[ ] ", 0) == 0;
    const bool checked = left.rfind("[x] ", 0) == 0;
    if (checkbox) {
        left = left.substr(4);
    }

    if (selected) {
        const int x = action_style ? 42 : 8;
        const int w = action_style ? 156 : 224;
        lv_rect(root, x, y, w, row_h, panel, 2, line, 1);
        lv_rect(root, x + 3, y + 2, 3, row_h - 4, teal);
        lv_rect(root, x + 8, y + 2, w - 12, row_h - 4, bg, 1);
    } else if (!action_style) {
        lv_rect(root, 10, y + row_h, 220, 1, line);
    } else {
        lv_rect(root, 44, y + row_h, 152, 1, line);
    }

    int text_x = action_style ? 58 : 17;
    int text_w = action_style ? 126 : 150;
    const int text_y = y + (!action_style && row_h > 20 ? 2 : 0);
    if (checkbox) {
        lv_rect(root, 17, y + std::max(4, (row_h - 5) / 2), 5, 5, checked ? teal : bg, 0, checked ? teal : dim, 1);
        text_x = 30;
        text_w = 142;
    } else if (action_style) {
        const char *symbol = LV_SYMBOL_RIGHT;
        if (left == "ADD TO END" || left == "ADD TO FRONT") {
            symbol = LV_SYMBOL_PLUS;
        } else if (left == "CANCEL") {
            symbol = LV_SYMBOL_CLOSE;
        } else if (left == "REPLACE QUEUE") {
            symbol = LV_SYMBOL_SHUFFLE;
        }
        lv_obj_t *icon = lv_label(root, symbol, 47, y + 1, 11, selected ? accent : teal, lv_font_symbol_small());
        lv_obj_set_style_text_align(icon, LV_TEXT_ALIGN_CENTER, 0);
    }

    const lv_color_t text_color = selected ? ink : dim;
    if (selected && lv_text_width_px(left, row_font) > text_w) {
        const MarqueeTiming timing{2200, 1600, 0, 18, 780};
        lv_marquee_label(root, left, text_x, text_y - 1, text_w, text_color, row_font, s_library_marquee, timing, row_h + 2, -1);
    } else {
        lv_clipped_label(root, left, text_x, text_y, text_w, row_h + 1, 0, text_color, row_font);
    }

    const std::string right = format_library_right(row.right);
    if (!right.empty()) {
        lv_obj_t *value = lv_label(root, right, action_style ? 158 : 174, y + (row_h > 20 ? 3 : 1), action_style ? 28 : 54,
                                   selected ? accent : dim,
                                   lv_font_tiny());
        lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);
    }
}

esp_err_t draw_screen_lvgl_library_list(const lofi::ScreenModel &screen)
{
    if (!s_lvgl_display) {
        return ESP_ERR_INVALID_STATE;
    }

    const lv_color_t bg = lv_rgb(20, 18, 24);
    const lv_color_t chrome = lv_rgb(13, 12, 16);
    const lv_color_t panel = lv_rgb(35, 32, 44);
    const lv_color_t accent = lv_rgb(245, 174, 94);
    const lv_color_t teal = lv_rgb(125, 205, 205);
    const lv_color_t ink = lv_rgb(248, 233, 207);
    const lv_color_t dim = lv_rgb(162, 152, 138);
    const lv_color_t line = lv_rgb(69, 60, 71);

    lv_obj_t *root = lv_screen_active();
    lv_obj_clean(root);
    lv_obj_set_style_bg_color(root, bg, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    draw_lvgl_header(root,
                     library_header_title(screen.title),
                     chrome,
                     accent,
                     ink,
                     dim,
                     line,
                     accent,
                     screen.background_task_active,
                     screen.background_task_frame);

    draw_library_header_text(root, screen, ink, dim);

    const bool action_style = screen.title == "Library Action";
    const bool root_style = screen.title == "Library Root";
    const bool dense_header = contains_non_ascii(screen.subtitle) || contains_non_ascii(screen.meta);
    const int row_h = action_style ? 17 : (root_style ? 20 : 25);
    const int first_y = action_style ? 48 : (screen.subtitle.empty() && screen.meta.empty() ? 24 : (dense_header ? 32 : 31));
    const int step = action_style ? 17 : (root_style ? 20 : 25);
    const size_t max_rows = root_style ? 5 : 4;
    const lv_font_t *row_font = action_style ? lv_font_small() : (root_style ? lv_font_library_cjk() : lv_font_library_cjk_large());
    if (action_style) {
        lv_rect(root, 37, 42, 166, 91, lv_rgb(24, 22, 31), 2, line, 1);
    }
    for (size_t i = 0; i < screen.rows.size() && i < max_rows; ++i) {
        const bool selected = !screen.rows[i].left.empty() && screen.rows[i].left[0] == '>';
        draw_lvgl_library_row(root,
                              screen.rows[i],
                              first_y + static_cast<int>(i) * step,
                              selected,
                              action_style,
                              row_font,
                              row_h,
                              bg,
                              panel,
                              accent,
                              teal,
                              ink,
                              dim,
                              line);
    }

    if (action_style) {
        lv_label(root, "BACK", 10, 120, 42, teal, lv_font_tiny());
        lv_obj_t *ok = lv_label(root, "OK", 200, 120, 30, teal, lv_font_tiny());
        lv_obj_set_style_text_align(ok, LV_TEXT_ALIGN_RIGHT, 0);
    }

    lv_refr_now(s_lvgl_display);
    return ESP_OK;
}

void draw_lvgl_queue_row(lv_obj_t *root,
                         const lofi::ScreenLine &row,
                         int y,
                         bool selected,
                         lv_color_t bg,
                         lv_color_t panel,
                         lv_color_t accent,
                         lv_color_t teal,
                         lv_color_t ink,
                         lv_color_t dim,
                         lv_color_t line)
{
    const bool now = row.right == "NOW" || row.left.find('*') != std::string::npos;
    const std::string title = strip_selection_marker(row.left);
    const lv_color_t title_color = selected ? ink : (now ? teal : dim);
    const lv_color_t index_color = now ? accent : (selected ? teal : dim);
    const int row_h = 17;

    if (selected) {
        lv_rect(root, 8, y, 224, row_h, panel, 2, line, 1);
        lv_rect(root, 11, y + 2, 3, row_h - 4, teal);
        lv_rect(root, 16, y + 2, 213, row_h - 4, bg, 1);
    } else {
        lv_rect(root, 10, y + row_h, 220, 1, line);
    }

    if (now) {
        lv_label(root, LV_SYMBOL_PLAY, 17, y + 2, 12, accent, lv_font_symbol_small());
    } else {
        lv_obj_t *index = lv_label(root, row.right, 10, y + 2, 27, index_color, lv_font_small());
        lv_obj_set_style_text_align(index, LV_TEXT_ALIGN_RIGHT, 0);
    }

    const int title_w = selected ? 125 : 152;
    if (selected) {
        const MarqueeTiming queue_timing{2200, 1600, 0, 18, 780};
        lv_marquee_label(root, title, 43, y, title_w, title_color, lv_font_cjk(), s_queue_marquee, queue_timing, row_h, -1);
    } else {
        lv_clipped_label(root, title, 43, y, title_w, row_h, -1, title_color, lv_font_cjk());
    }

    if (selected) {
        lv_obj_t *tag = lv_label(root, now ? "NOW" : "PLAY", 188, y + 2, 32, now ? accent : teal, lv_font_small());
        lv_obj_set_style_text_align(tag, LV_TEXT_ALIGN_RIGHT, 0);
    } else if (now) {
        lv_obj_t *tag = lv_label(root, "NOW", 191, y + 2, 29, accent, lv_font_small());
        lv_obj_set_style_text_align(tag, LV_TEXT_ALIGN_RIGHT, 0);
    }
}

int repeat_indicator_index(const std::string &value)
{
    if (value == "One") {
        return 1;
    }
    if (value == "List") {
        return 2;
    }
    return 0;
}

int screen_off_seconds_from_setting_value(const std::string &value)
{
    return lofi::setting_duration_seconds_from_value(value);
}

int sleep_timer_choice_index_from_value(const std::string &value)
{
    constexpr int choices[] = {0, 300, 600, 900, 1800, 3600, 7200, 10800, 18000, 36000};
    const int seconds = screen_off_seconds_from_setting_value(value);
    if (seconds <= 0) {
        return 0;
    }
    int best_index = 1;
    int best_distance = std::abs(seconds - choices[1]);
    for (int i = 2; i < static_cast<int>(sizeof(choices) / sizeof(choices[0])); ++i) {
        const int distance = std::abs(seconds - choices[i]);
        if (distance < best_distance) {
            best_distance = distance;
            best_index = i;
        }
    }
    return best_index;
}

int screen_off_choice_index_from_value(const std::string &value)
{
    constexpr int choices[] = {10, 15, 20, 30, 60, 120, 180, 300, 600, 0};
    const int seconds = screen_off_seconds_from_setting_value(value);
    if (seconds <= 0) {
        return static_cast<int>(sizeof(choices) / sizeof(choices[0])) - 1;
    }
    int best_index = 0;
    int best_distance = std::abs(seconds - choices[0]);
    for (int i = 1; i < static_cast<int>(sizeof(choices) / sizeof(choices[0])); ++i) {
        const int distance = std::abs(seconds - choices[i]);
        if (distance < best_distance) {
            best_distance = distance;
            best_index = i;
        }
    }
    return best_index;
}

void draw_screen_off_discrete_timeline(lv_obj_t *root,
                                       const std::string &value,
                                       lv_color_t accent,
                                       lv_color_t teal,
                                       lv_color_t line,
                                       lv_color_t progress)
{
    constexpr int kX = 82;
    constexpr int kY = 86;
    constexpr int kW = 96;
    constexpr int kChoices = 10;
    const int active = screen_off_choice_index_from_value(value);

    lv_rect(root, kX, kY + 6, kW, 2, progress);
    for (int i = 0; i < kChoices; ++i) {
        const int cx = kX + (kW * i + (kChoices - 2) / 2) / (kChoices - 1);
        if (i < active) {
            lv_rect(root, cx - 2, kY + 4, 4, 4, accent, 2);
        } else if (i == active) {
            lv_rect(root, cx - 4, kY + 2, 8, 8, teal, 4, accent, 1);
        } else {
            lv_rect(root, cx - 2, kY + 4, 4, 4, line, 2);
        }
    }
}

void draw_sleep_timer_discrete_timeline(lv_obj_t *root,
                                        const std::string &value,
                                        lv_color_t accent,
                                        lv_color_t teal,
                                        lv_color_t line,
                                        lv_color_t progress)
{
    constexpr int kX = 82;
    constexpr int kY = 86;
    constexpr int kW = 96;
    constexpr int kChoices = 10;
    const int active = sleep_timer_choice_index_from_value(value);

    lv_rect(root, kX, kY + 6, kW, 2, progress);
    for (int i = 0; i < kChoices; ++i) {
        const int cx = kX + (kW * i + (kChoices - 2) / 2) / (kChoices - 1);
        if (i < active) {
            lv_rect(root, cx - 2, kY + 4, 4, 4, accent, 2);
        } else if (i == active) {
            lv_rect(root, cx - 4, kY + 2, 8, 8, teal, 4, accent, 1);
        } else {
            lv_rect(root, cx - 2, kY + 4, 4, 4, line, 2);
        }
    }
}

void draw_lvgl_setting_switch(lv_obj_t *root,
                              int x,
                              int y,
                              bool on,
                              lv_color_t accent,
                              lv_color_t ink,
                              lv_color_t line,
                              lv_color_t progress)
{
#if LV_USE_SWITCH
    lv_obj_t *sw = lv_switch_create(root);
    lv_obj_set_pos(sw, x, y);
    lv_obj_set_size(sw, 44, 16);
    lv_obj_remove_flag(sw, LV_OBJ_FLAG_CLICKABLE);
    if (on) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    lv_obj_set_style_bg_color(sw, progress, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(sw, line, LV_PART_MAIN);
    lv_obj_set_style_border_width(sw, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(sw, 8, LV_PART_MAIN);
    const lv_style_selector_t checked_indicator =
        static_cast<lv_style_selector_t>(LV_PART_INDICATOR) | static_cast<lv_style_selector_t>(LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, accent, checked_indicator);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, checked_indicator);
    lv_obj_set_style_radius(sw, 8, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sw, ink, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_radius(sw, 6, LV_PART_KNOB);
#else
    lv_rect(root, x, y + 3, 42, 10, on ? accent : progress, 5, line, 1);
    lv_rect(root, on ? x + 28 : x + 4, y + 5, 6, 6, ink);
#endif
}

void draw_playback_setting_card(lv_obj_t *root,
                                const PlaybackSettingItem &item,
                                lv_color_t bg,
                                lv_color_t panel,
                                lv_color_t accent,
                                lv_color_t teal,
                                lv_color_t ink,
                                lv_color_t dim,
                                lv_color_t line,
                                lv_color_t progress)
{
    lv_rect(root, 14, 43, 207, 66, panel, 6, line, 1);
    draw_cut_corner_frame(root, 14, 43, 207, 66, teal, 2);
    lv_rect(root, 18, 47, 199, 58, bg, 4);

    lv_obj_t *icon = lv_label(root, item.symbol, 26, 57, 42, teal, lv_font_large_symbol());
    align_center(icon);

    lv_label(root, setting_display_label(item.label), 73, 52, 72, ink, lv_font_normal());
    const bool compact_value = item.value.size() > 3 || item.value.find('h') != std::string::npos;
    const lv_font_t *value_font = compact_value ? lv_font_home_label() : lv_font_title();
    lv_obj_t *value = lv_label(root, item.value, 126, 50, 70, ink, value_font);
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);

    if (item.adjustable) {
        lv_obj_t *left = lv_label(root, LV_SYMBOL_LEFT, 60, 75, 14, accent, lv_font_symbol_normal());
        align_center(left);
        lv_obj_t *right = lv_label(root, LV_SYMBOL_RIGHT, 204, 75, 14, accent, lv_font_symbol_normal());
        align_center(right);
    } else {
        lv_obj_t *right = lv_label(root, LV_SYMBOL_RIGHT, 204, 75, 14, accent, lv_font_symbol_normal());
        align_center(right);
    }

    if (item.volume >= 0) {
        lv_rect(root, 76, 89, 122, 9, progress, 0, line, 1);
        const int fill = std::max(0, std::min(118, 118 * item.volume / 100));
        if (fill > 0) {
            lv_rect(root, 78, 92, fill, 3, accent);
        }
    } else if (item.label == "Repeat") {
        const int active = repeat_indicator_index(item.value);
        for (int i = 0; i < 3; ++i) {
            lv_rect(root, 81 + i * 16, 90, 9, 5, i == active ? accent : line);
        }
    } else if (item.label == "Shuffle") {
        const bool on = item.value == "On";
        draw_lvgl_setting_switch(root, 82, 86, on, accent, ink, line, progress);
    } else if (item.label == "Sleep Timer") {
        draw_sleep_timer_discrete_timeline(root, item.value, accent, teal, line, progress);
    } else if (item.label == "Screen Off") {
        draw_screen_off_discrete_timeline(root, item.value, accent, teal, line, progress);
    } else {
        lv_rect(root, 82, 89, 82, 2, line);
        lv_rect(root, 82, 94, 44, 2, accent);
    }
}

lv_obj_t *lv_plain_container(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

void draw_cut_corner_frame(lv_obj_t *parent, int x, int y, int w, int h, lv_color_t color, int thickness = 2)
{
    lv_rect(parent, x + 8, y, w - 16, thickness, color);
    lv_rect(parent, x + 8, y + h - thickness, w - 16, thickness, color);
    lv_rect(parent, x, y + 8, thickness, h - 16, color);
    lv_rect(parent, x + w - thickness, y + 8, thickness, h - 16, color);

    for (int i = 0; i < 4; ++i) {
        const int o = i * 2;
        lv_rect(parent, x + 2 + o, y + 6 - o, thickness, thickness, color);
        lv_rect(parent, x + w - 4 - o, y + 6 - o, thickness, thickness, color);
        lv_rect(parent, x + 2 + o, y + h - 8 + o, thickness, thickness, color);
        lv_rect(parent, x + w - 4 - o, y + h - 8 + o, thickness, thickness, color);
    }
}

struct HomeItem {
    const char *label;
    const lv_image_dsc_t *center_icon;
    const lv_image_dsc_t *side_icon;
};

const HomeItem *home_items(size_t &count)
{
    static const HomeItem items[] = {
        {"LIBRARY", &library_center_rgb565, &library_side_rgb565},
        {"QUEUE", &queue_center_rgb565, &queue_side_rgb565},
        {"LOFI", &lofi_center_rgb565, &lofi_side_rgb565},
        {"SETTINGS", &settings_center_rgb565, &settings_side_rgb565},
        {"NOW", &now_center_rgb565, &now_side_rgb565},
    };
    count = sizeof(items) / sizeof(items[0]);
    return items;
}

int selected_home_index(const lofi::ScreenModel &screen)
{
    for (size_t i = 0; i < screen.rows.size(); ++i) {
        if (screen.rows[i].left.find('>') != std::string::npos) {
            return static_cast<int>(std::min<size_t>(i, 4));
        }
    }
    return 0;
}

int wrap_home_index(int index, int count)
{
    if (count <= 0) {
        return 0;
    }
    while (index < 0) {
        index += count;
    }
    return index % count;
}

void anim_set_x(void *obj, int32_t value)
{
    lv_obj_set_x(static_cast<lv_obj_t *>(obj), value);
}

void anim_set_y(void *obj, int32_t value)
{
    lv_obj_set_y(static_cast<lv_obj_t *>(obj), value);
}

void anim_set_opa(void *obj, int32_t value)
{
    lv_obj_set_style_opa(static_cast<lv_obj_t *>(obj), static_cast<lv_opa_t>(value), 0);
}

void start_home_anim(lv_obj_t *obj,
                     lv_anim_exec_xcb_t exec,
                     int32_t start,
                     int32_t end,
                     uint16_t duration_ms,
                     uint16_t delay_ms = 0)
{
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, obj);
    lv_anim_set_values(&anim, start, end);
    lv_anim_set_duration(&anim, duration_ms);
    lv_anim_set_delay(&anim, delay_ms);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&anim, exec);
    lv_anim_start(&anim);
}

void draw_home_icon(lv_obj_t *parent, const lv_image_dsc_t *icon, int x, int y, lv_opa_t opa)
{
    lv_obj_t *image = lv_image_create(parent);
    lv_image_set_src(image, icon);
    lv_obj_set_pos(image, x, y);
    lv_obj_set_style_opa(image, opa, 0);
}

void draw_home_stage(lv_obj_t *parent,
                     const HomeItem *items,
                     int count,
                     int selected,
                     lv_color_t accent,
                     lv_color_t teal,
                     lv_color_t line)
{
    const int previous = wrap_home_index(selected - 1, count);
    const int next = wrap_home_index(selected + 1, count);

    draw_cut_corner_frame(parent, -21, 33, 61, 54, line, 2);
    draw_cut_corner_frame(parent, 201, 33, 61, 54, line, 2);
    draw_home_icon(parent, items[previous].side_icon, -35, 16, LV_OPA_70);
    draw_home_icon(parent, items[next].side_icon, 206, 16, LV_OPA_70);

    lv_obj_t *left = lv_label(parent, "<", 50, 52, 26, accent, lv_font_title());
    lv_obj_set_style_text_align(left, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *right = lv_label(parent, ">", 164, 52, 26, accent, lv_font_title());
    lv_obj_set_style_text_align(right, LV_TEXT_ALIGN_CENTER, 0);

    draw_cut_corner_frame(parent, 75, 4, 90, 80, teal, 2);
    draw_home_icon(parent, items[selected].center_icon, 82, 5, LV_OPA_COVER);
}

void draw_home_caption(lv_obj_t *parent,
                       const HomeItem *items,
                       int count,
                       int selected,
                       int y_offset,
                       lv_color_t accent,
                       lv_color_t ink,
                       lv_color_t line)
{
    lv_obj_t *name = lv_label(parent, items[selected].label, 0, 88, LCD_WIDTH, ink, lv_font_home_label());
    lv_obj_set_y(name, y_offset + 88);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);

    const int dot_y = y_offset + 110;
    const int dot_gap = 13;
    const int dots_w = static_cast<int>(count - 1) * dot_gap + 4;
    int dot_x = (LCD_WIDTH - dots_w) / 2;
    for (int i = 0; i < count; ++i) {
        lv_rect(parent, dot_x + i * dot_gap, dot_y, 4, 4, i == selected ? accent : line);
    }
}

void draw_home_page(lv_obj_t *parent,
                    const HomeItem *items,
                    int count,
                    int selected,
                    lv_color_t accent,
                    lv_color_t teal,
                    lv_color_t ink,
                    lv_color_t line)
{
    draw_home_stage(parent, items, count, selected, accent, teal, line);
    draw_home_caption(parent, items, count, selected, 0, accent, ink, line);
}

void draw_boot_frame(lv_obj_t *root, lv_color_t line, lv_color_t accent)
{
    lv_rect(root, 7, 6, 226, 1, line);
    lv_rect(root, 7, 128, 226, 1, line);
    lv_rect(root, 6, 7, 1, 121, line);
    lv_rect(root, 233, 7, 1, 121, line);

    lv_rect(root, 10, 9, 8, 2, accent);
    lv_rect(root, 10, 9, 2, 8, accent);
    lv_rect(root, 222, 9, 8, 2, accent);
    lv_rect(root, 228, 9, 2, 8, accent);
    lv_rect(root, 10, 124, 8, 2, accent);
    lv_rect(root, 10, 118, 2, 8, accent);
    lv_rect(root, 222, 124, 8, 2, accent);
    lv_rect(root, 228, 118, 2, 8, accent);
}

const char *const *boot_glyph_5x7(char ch)
{
    static const char *const blank[7] = {
        "00000",
        "00000",
        "00000",
        "00000",
        "00000",
        "00000",
        "00000",
    };
    static const char *const f[7] = {
        "11111",
        "10000",
        "10000",
        "11110",
        "10000",
        "10000",
        "10000",
    };
    static const char *const i[7] = {
        "11111",
        "00100",
        "00100",
        "00100",
        "00100",
        "00100",
        "11111",
    };
    static const char *const l[7] = {
        "10000",
        "10000",
        "10000",
        "10000",
        "10000",
        "10000",
        "11111",
    };
    static const char *const o[7] = {
        "01110",
        "10001",
        "10001",
        "10001",
        "10001",
        "10001",
        "01110",
    };

    switch (std::toupper(static_cast<unsigned char>(ch))) {
    case 'F':
        return f;
    case 'I':
        return i;
    case 'L':
        return l;
    case 'O':
        return o;
    default:
        return blank;
    }
}

int boot_text_width_5x7(const std::string &text, int scale, int spacing)
{
    int width = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        width += (text[i] == ' ' ? 3 : 5) * scale;
        if (i + 1 < text.size()) {
            width += spacing;
        }
    }
    return width;
}

void draw_boot_text_5x7(lv_obj_t *root, const std::string &text, int x, int y, int scale, int spacing, lv_color_t color)
{
    int cursor = x;
    for (char ch : text) {
        const char *const *glyph = boot_glyph_5x7(ch);
        const int glyph_w = ch == ' ' ? 3 : 5;
        for (int row = 0; row < 7; ++row) {
            for (int col = 0; col < glyph_w; ++col) {
                if (glyph[row][col] == '1') {
                    lv_rect(root, cursor + col * scale, y + row * scale, scale, scale, color);
                }
            }
        }
        cursor += glyph_w * scale + spacing;
    }
}

esp_err_t draw_screen_lvgl_boot(const lofi::ScreenModel &screen)
{
    if (!s_lvgl_display) {
        return ESP_ERR_INVALID_STATE;
    }

    const lv_color_t bg = lv_rgb(11, 13, 18);
    const lv_color_t accent = lv_rgb(244, 162, 79);
    const lv_color_t teal = lv_rgb(116, 199, 195);
    const lv_color_t ink = lv_rgb(245, 223, 182);
    const lv_color_t line = lv_rgb(78, 68, 82);

    lv_obj_t *root = lv_screen_active();
    lv_obj_clean(root);
    lv_obj_set_style_bg_color(root, bg, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    draw_boot_frame(root, line, accent);
    lv_obj_t *cassette = lv_image_create(root);
    lv_image_set_src(cassette, &boot_cassette_rgb565);
    lv_obj_set_pos(cassette, 78, 18);

    const std::string title_text = "LOFI";
    const int title_width = boot_text_width_5x7(title_text, 3, 3);
    draw_boot_text_5x7(root, title_text, (LCD_WIDTH - title_width) / 2 + 1, 79, 3, 3, lv_rgb(45, 37, 42));
    draw_boot_text_5x7(root, title_text, (LCD_WIDTH - title_width) / 2, 78, 3, 3, ink);

    lv_obj_t *subtitle = lv_label(root,
                                  screen.subtitle.empty() ? "Pocket Lo-Fi Player" : screen.subtitle,
                                  0,
                                  108,
                                  LCD_WIDTH,
                                  teal,
                                  lv_font_small());
    lv_obj_set_style_text_align(subtitle, LV_TEXT_ALIGN_CENTER, 0);

    lv_refr_now(s_lvgl_display);
    wait_for_lvgl_flush_idle(80);
    return ESP_OK;
}

esp_err_t draw_screen_lvgl_home(const lofi::ScreenModel &screen)
{
    if (!s_lvgl_display) {
        return ESP_ERR_INVALID_STATE;
    }

    const lv_color_t bg = lv_rgb(20, 18, 24);
    const lv_color_t chrome = lv_rgb(13, 12, 16);
    const lv_color_t accent = lv_rgb(245, 174, 94);
    const lv_color_t teal = lv_rgb(125, 205, 205);
    const lv_color_t ink = lv_rgb(248, 233, 207);
    const lv_color_t dim = lv_rgb(162, 152, 138);
    const lv_color_t line = lv_rgb(69, 60, 71);

    size_t item_count = 0;
    const HomeItem *items = home_items(item_count);
    const int count = static_cast<int>(item_count);
    const int selected = std::min(std::max(selected_home_index(screen), 0), count - 1);

    lv_obj_t *root = lv_screen_active();
    lv_obj_clean(root);
    lv_obj_set_style_bg_color(root, bg, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    draw_lvgl_header(root, "HOME", chrome, accent, ink, dim, line, accent, screen.background_task_active, screen.background_task_frame);

    constexpr int kBodyY = 19;
    constexpr int kStageHeight = 90;
    constexpr uint16_t kStageAnimMs = 300;
    constexpr uint16_t kCaptionAnimMs = 180;
    lv_obj_t *stage_viewport = lv_plain_container(root, 0, kBodyY, LCD_WIDTH, kStageHeight);
    const int previous_selected = s_home_last_selected;
    const bool animate = previous_selected >= 0 && previous_selected < count && previous_selected != selected;
    if (animate) {
        int dir = selected > previous_selected ? 1 : -1;
        if (previous_selected == 0 && selected == count - 1) {
            dir = -1;
        } else if (previous_selected == count - 1 && selected == 0) {
            dir = 1;
        }

        lv_obj_t *strip = lv_plain_container(stage_viewport,
                                             dir > 0 ? 0 : -LCD_WIDTH,
                                             0,
                                             LCD_WIDTH * 2,
                                             kStageHeight);
        lv_obj_t *old_page = lv_plain_container(strip, dir > 0 ? 0 : LCD_WIDTH, 0, LCD_WIDTH, kStageHeight);
        draw_home_stage(old_page, items, count, previous_selected, accent, teal, line);
        lv_obj_t *new_page = lv_plain_container(strip, dir > 0 ? LCD_WIDTH : 0, 0, LCD_WIDTH, kStageHeight);
        draw_home_stage(new_page, items, count, selected, accent, teal, line);

        start_home_anim(strip, anim_set_x, dir > 0 ? 0 : -LCD_WIDTH, dir > 0 ? -LCD_WIDTH : 0, kStageAnimMs);

        lv_obj_t *old_caption = lv_plain_container(root, 0, kBodyY, LCD_WIDTH, LCD_HEIGHT - kBodyY);
        draw_home_caption(old_caption, items, count, previous_selected, 0, accent, ink, line);
        start_home_anim(old_caption, anim_set_y, kBodyY, kBodyY - 3, kCaptionAnimMs);
        start_home_anim(old_caption, anim_set_opa, LV_OPA_COVER, LV_OPA_TRANSP, kCaptionAnimMs);

        lv_obj_t *new_caption = lv_plain_container(root, 0, kBodyY + 4, LCD_WIDTH, LCD_HEIGHT - kBodyY);
        lv_obj_set_style_opa(new_caption, LV_OPA_TRANSP, 0);
        draw_home_caption(new_caption, items, count, selected, 0, accent, ink, line);
        start_home_anim(new_caption, anim_set_y, kBodyY + 4, kBodyY, kCaptionAnimMs, 70);
        start_home_anim(new_caption, anim_set_opa, LV_OPA_TRANSP, LV_OPA_COVER, kCaptionAnimMs, 70);
    } else {
        lv_obj_t *page = lv_plain_container(root, 0, kBodyY, LCD_WIDTH, LCD_HEIGHT - kBodyY);
        draw_home_page(page, items, count, selected, accent, teal, ink, line);
    }
    s_home_last_selected = selected;

    lv_refr_now(s_lvgl_display);
    return ESP_OK;
}

void draw_lvgl_footer_button(lv_obj_t *root,
                             int x,
                             int y,
                             int w,
                             const std::string &label,
                             bool active,
                             lv_color_t panel,
                             lv_color_t active_fill,
                             lv_color_t ink,
                             lv_color_t line)
{
    lv_rect(root, x + 2, y, w - 4, 1, active ? active_fill : panel, 0, line, 1);
    lv_rect(root, x, y + 1, w, 12, active ? active_fill : panel, 1, line, 1);
    lv_rect(root, x + 2, y + 13, w - 4, 1, active ? active_fill : panel, 0, line, 1);
    lv_obj_t *text = lv_label(root, label, x + 2, y + 1, w - 4, ink, lv_font_normal());
    lv_obj_set_style_text_align(text, LV_TEXT_ALIGN_CENTER, 0);
}

[[maybe_unused]] void draw_lvgl_soft_footer(lv_obj_t *root,
                                            const std::string &left,
                                            const std::string &center,
                                            const std::string &right,
                                            bool center_active,
                                            lv_color_t chrome,
                                            lv_color_t panel,
                                            lv_color_t active_fill,
                                            lv_color_t ink,
                                            lv_color_t line)
{
    lv_rect(root, 0, 117, LCD_WIDTH, 18, chrome);
    lv_rect(root, 0, 117, LCD_WIDTH, 1, line);
    draw_lvgl_footer_button(root, 6, 120, 68, left, false, panel, active_fill, ink, line);
    draw_lvgl_footer_button(root, 86, 120, 68, center, center_active, panel, active_fill, ink, line);
    draw_lvgl_footer_button(root, 166, 120, 68, right, false, panel, active_fill, ink, line);
}

const char *const *volume_icon_rows_for_percent(int volume_percent)
{
    volume_percent = std::max(0, std::min(100, volume_percent));
    if (volume_percent == 0) {
        return volume_icons::kMuteRows;
    }
    if (volume_percent <= 30) {
        return volume_icons::kLowRows;
    }
    if (volume_percent < 60) {
        return volume_icons::kMediumRows;
    }
    return volume_icons::kHighRows;
}

void draw_lvgl_bitmap_rows(lv_obj_t *root,
                           const char *const *rows,
                           int width,
                           int height,
                           int x,
                           int y,
                           lv_color_t color,
                           lv_color_t background)
{
    for (int row = 0; row < height; ++row) {
        for (int col = 0; col < width; ++col) {
            const char level = rows[row][col];
            if (level != '.') {
                uint8_t opacity = 255;
                if (level == ':') {
                    opacity = 88;
                } else if (level == '+') {
                    opacity = 168;
                }
                lv_rect(root, x + col, y + row, 1, 1, lv_color_mix(color, background, opacity));
            }
        }
    }
}

void draw_lvgl_volume_status_icon(lv_obj_t *root,
                                  int x,
                                  int y,
                                  int volume_percent,
                                  lv_color_t color,
                                  lv_color_t background)
{
    draw_lvgl_bitmap_rows(root,
                          volume_icon_rows_for_percent(volume_percent),
                          volume_icons::kWidth,
                          volume_icons::kHeight,
                          x,
                          y,
                          color,
                          background);
}

const char *repeat_badge(lofi::RepeatMode repeat)
{
    switch (repeat) {
    case lofi::RepeatMode::One:
        return "1";
    case lofi::RepeatMode::List:
        return "";
    case lofi::RepeatMode::Off:
    default:
        return "";
    }
}

void draw_lvgl_repeat_status_icon(lv_obj_t *root,
                                  int x,
                                  int y,
                                  lofi::RepeatMode repeat,
                                  lv_color_t active,
                                  lv_color_t inactive,
                                  lv_color_t chrome)
{
    const bool enabled = repeat != lofi::RepeatMode::Off;
    lv_obj_t *icon = lv_label(root, LV_SYMBOL_LOOP, x, y, 22, enabled ? active : inactive, lv_font_symbol_small());
    lv_obj_set_style_text_align(icon, LV_TEXT_ALIGN_CENTER, 0);
    const char *badge = repeat_badge(repeat);
    if (badge[0] != '\0') {
        lv_obj_t *label = lv_label(root, badge, x + 14, y + 3, 8, active, lv_font_tiny());
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_rect(root, x + 15, y + 9, 4, 1, chrome);
    }
}

void draw_lvgl_now_playing_footer(lv_obj_t *root,
                                  int volume_percent,
                                  lofi::RepeatMode repeat,
                                  bool shuffle_enabled,
                                  bool lofi_active,
                                  lv_color_t chrome,
                                  lv_color_t accent,
                                  lv_color_t teal,
                                  lv_color_t ink,
                                  lv_color_t dim,
                                  lv_color_t line)
{
    lv_rect(root, 0, 121, LCD_WIDTH, 14, chrome);
    lv_rect(root, 0, 121, LCD_WIDTH, 1, line);

    lv_obj_t *note = lv_label(root, LV_SYMBOL_AUDIO, 8, 121, 22, teal, lv_font_symbol_small());
    lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, 0);
    lv_label(root, "MENU", 36, 123, 42, ink, lv_font_now_chrome());

    lv_rect(root, 98, 123, 1, 9, line);
    draw_lvgl_volume_status_icon(root, 106, 122, volume_percent, teal, chrome);

    lv_rect(root, 131, 123, 1, 9, line);
    draw_lvgl_repeat_status_icon(root, 138, 121, repeat, accent, dim, chrome);

    lv_rect(root, 162, 123, 1, 9, line);
    lv_obj_t *shuffle = lv_label(root,
                                 LV_SYMBOL_SHUFFLE,
                                 169,
                                 121,
                                 22,
                                 shuffle_enabled ? accent : dim,
                                 lv_font_symbol_small());
    lv_obj_set_style_text_align(shuffle, LV_TEXT_ALIGN_CENTER, 0);

    lv_rect(root, 193, 123, 1, 9, line);
    lv_obj_t *lofi = lv_label(root, "LOFI", 199, 123, 34, lofi_active ? teal : dim, lv_font_now_chrome());
    lv_obj_set_style_text_align(lofi, LV_TEXT_ALIGN_CENTER, 0);
}

void draw_lvgl_volume_overlay(lv_obj_t *root,
                              int volume_percent,
                              lv_color_t bg,
                              lv_color_t panel,
                              lv_color_t accent,
                              lv_color_t teal,
                              lv_color_t ink,
                              lv_color_t dim,
                              lv_color_t line)
{
    volume_percent = std::max(0, std::min(100, volume_percent));
    constexpr int w = 142;
    constexpr int h = 52;
    constexpr int x = (240 - w) / 2;
    constexpr int y = (135 - h) / 2;
    lv_obj_t *box = lv_rect(root, x, y, w, h, panel, 3, accent, 1);
    lv_rect(box, 3, 3, w - 6, h - 6, bg, 2, line, 1);

    draw_lvgl_volume_status_icon(box, 11, 8, volume_percent, teal, bg);
    lv_label(box, "VOLUME", 36, 6, 55, dim, lv_font_small());
    lv_obj_t *value = lv_label(box, std::to_string(volume_percent) + "%", 82, 4, 46, ink, lv_font_now_title());
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);

    lv_rect(box, 10, 34, 118, 7, lv_rgb(31, 28, 37), 0, line, 1);
    const int fill = std::max(0, std::min(116, 116 * volume_percent / 100));
    if (fill > 0) {
        lv_rect(box, 11, 36, fill, 3, volume_percent > 80 ? accent : teal);
    }
    for (int i = 0; i <= 4; ++i) {
        const int tx = 11 + i * 29;
        lv_rect(box, tx, 42, 1, 2, i * 25 <= volume_percent ? accent : line);
    }
}

void draw_lvgl_volume_warning_banner(lv_obj_t *root,
                                     lv_color_t preview,
                                     lv_color_t accent,
                                     const lv_font_t *font)
{
    lv_rect(root, 22, 23, 196, 15, preview, 2, accent, 1);
    lv_clipped_label(root, "LIMIT >80 - PRESS AGAIN", 29, 24, 184, 13, -2, accent, font);
}

void draw_lvgl_mode_overlay(lv_obj_t *root,
                            const lofi::ScreenModel &screen,
                            lv_color_t bg,
                            lv_color_t panel,
                            lv_color_t accent,
                            lv_color_t teal,
                            lv_color_t ink,
                            lv_color_t dim,
                            lv_color_t line)
{
    constexpr int w = 150;
    constexpr int h = 48;
    constexpr int x = (240 - w) / 2;
    constexpr int y = (135 - h) / 2;
    lv_obj_t *box = lv_rect(root, x, y, w, h, panel, 3, accent, 1);
    lv_rect(box, 3, 3, w - 6, h - 6, bg, 2, line, 1);

    const bool shuffle = screen.mode_overlay_kind == "shuffle";
    const char *symbol = shuffle ? LV_SYMBOL_SHUFFLE : LV_SYMBOL_LOOP;
    lv_obj_t *icon = lv_label(box, symbol, 13, 13, 30, shuffle ? teal : accent, lv_font_symbol_title());
    lv_obj_set_style_text_align(icon, LV_TEXT_ALIGN_CENTER, 0);

    lv_label(box, screen.mode_overlay_title.empty() ? "MODE" : screen.mode_overlay_title, 49, 8, 78, dim, lv_font_small());
    lv_obj_t *value = lv_label(box, screen.mode_overlay_value, 49, 23, 84, ink, lv_font_normal());
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);
}

bool load_album_art_cache(const std::string &path)
{
    if (path.empty()) {
        release_album_art_cache();
        return false;
    }
    if (s_album_art_valid && s_album_art_cache_path == path) {
        return true;
    }
    if (s_album_art_failed_path == path) {
        return false;
    }

    FILE *file = std::fopen(path.c_str(), "rb");
    if (!file) {
        release_album_art_cache();
        s_album_art_failed_path = path;
        ESP_LOGW(TAG, "COVER_DRAW open failed path=%s", path.c_str());
        return false;
    }
    if (!s_album_art_pixels) {
        s_album_art_pixels = static_cast<uint16_t *>(std::malloc(kAlbumArtWidth * kAlbumArtHeight * sizeof(uint16_t)));
        if (!s_album_art_pixels) {
            std::fclose(file);
            s_album_art_valid = false;
            s_album_art_failed_path = path;
            ESP_LOGW(TAG, "COVER_DRAW allocation failed path=%s", path.c_str());
            return false;
        }
    }
    const size_t expected = kAlbumArtWidth * kAlbumArtHeight;
    const size_t read = std::fread(s_album_art_pixels, sizeof(uint16_t), expected, file);
    const bool ok = std::fclose(file) == 0 && read == expected;
    if (!ok) {
        release_album_art_cache();
        s_album_art_failed_path = path;
        ESP_LOGW(TAG, "COVER_DRAW read failed path=%s pixels=%u", path.c_str(), static_cast<unsigned>(read));
        return false;
    }

    s_album_art_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_album_art_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_album_art_dsc.header.flags = 0;
    s_album_art_dsc.header.w = kAlbumArtWidth;
    s_album_art_dsc.header.h = kAlbumArtHeight;
    s_album_art_dsc.header.stride = kAlbumArtWidth * sizeof(uint16_t);
    s_album_art_dsc.data_size = expected * sizeof(uint16_t);
    s_album_art_dsc.data = reinterpret_cast<const uint8_t *>(s_album_art_pixels);
    s_album_art_cache_path = path;
    s_album_art_failed_path.clear();
    s_album_art_valid = true;
    ESP_LOGI(TAG,
             "COVER_DRAW loaded path=%s bytes=%u",
             path.c_str(),
             static_cast<unsigned>(expected * sizeof(uint16_t)));
    return true;
}

void draw_lvgl_album_art(lv_obj_t *root,
                         lv_color_t panel,
                         lv_color_t bg,
                         lv_color_t accent,
                         lv_color_t teal,
                         lv_color_t dim,
                         lv_color_t line,
                         const std::string &cache_path)
{
    constexpr int art_area_w = 72;
    constexpr int art_area_h = 72;
    constexpr int art_inset_x = 5;
    constexpr int art_inset_y = 5;
    lv_obj_t *album = lv_rect(root, 7, 21, 82, 82, panel, 1, teal, 1);
    lv_rect(album, art_inset_x, art_inset_y, art_area_w, art_area_h, bg);
    if (load_album_art_cache(cache_path)) {
        lv_obj_t *image = lv_image_create(album);
        lv_image_set_src(image, &s_album_art_dsc);
        lv_obj_set_pos(image,
                       art_inset_x + (art_area_w - kAlbumArtWidth) / 2 - 1,
                       art_inset_y + (art_area_h - kAlbumArtHeight) / 2);
        return;
    }

    lv_rect(album, 50, 12, 4, 2, accent);
    lv_rect(album, 53, 14, 3, 3, accent);
    lv_rect(album, 55, 17, 2, 4, accent);
    lv_rect(album, 53, 21, 3, 3, accent);
    lv_rect(album, 49, 24, 5, 2, accent);

    lv_rect(album, 15, 20, 1, 1, dim);
    lv_rect(album, 36, 14, 1, 1, dim);
    lv_rect(album, 47, 25, 1, 1, dim);
    lv_rect(album, 27, 32, 1, 1, dim);
    lv_rect(album, 61, 18, 1, 1, dim);

    lv_rect(album, 3, 50, 15, 12, lv_rgb(24, 23, 34));
    lv_rect(album, 18, 54, 18, 12, lv_rgb(30, 28, 42));
    lv_rect(album, 36, 58, 17, 10, lv_rgb(28, 26, 39));
    lv_rect(album, 52, 51, 21, 14, lv_rgb(24, 23, 34));

    lv_rect(album, 5, 63, 4, 8, lv_color_make(0, 0, 0));
    lv_rect(album, 14, 61, 4, 10, lv_color_make(0, 0, 0));
    lv_rect(album, 25, 64, 5, 7, lv_color_make(0, 0, 0));
    lv_rect(album, 36, 60, 6, 11, lv_color_make(0, 0, 0));
    lv_rect(album, 55, 64, 4, 7, lv_color_make(0, 0, 0));

    lv_rect(album, 7, 67, 1, 1, accent);
    lv_rect(album, 15, 66, 1, 1, accent);
    lv_rect(album, 27, 68, 1, 1, accent);
    lv_rect(album, 39, 66, 1, 1, accent);
    lv_rect(album, 57, 67, 1, 1, accent);

    lv_rect(album, 3, 72, 70, 1, line);
    lv_rect(album, 15, 73, 4, 1, teal);
    lv_rect(album, 23, 74, 7, 1, teal);
    lv_rect(album, 36, 73, 4, 1, teal);
    lv_rect(album, 48, 74, 7, 1, teal);
}

esp_err_t draw_screen_lvgl_now_playing(const lofi::ScreenModel &screen)
{
    if (!s_lvgl_display) {
        return ESP_ERR_INVALID_STATE;
    }

    const lv_color_t bg = lv_rgb(20, 18, 24);
    const lv_color_t chrome = lv_rgb(13, 12, 16);
    const lv_color_t panel = lv_rgb(44, 40, 53);
    const lv_color_t accent = lv_rgb(245, 174, 94);
    const lv_color_t teal = lv_rgb(125, 205, 205);
    const lv_color_t ink = lv_rgb(248, 233, 207);
    const lv_color_t dim = lv_rgb(162, 152, 138);
    const lv_color_t line = lv_rgb(69, 60, 71);
    const lv_color_t preview = lv_rgb(28, 25, 35);
    const lv_color_t progress = lv_rgb(51, 45, 55);

    lv_obj_t *root = lv_screen_active();
    lv_obj_clean(root);
    lv_obj_set_style_bg_color(root, bg, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    draw_lvgl_now_header(root, "NOW", chrome, accent, ink, dim, line, accent, screen.background_task_active, screen.background_task_frame);
    draw_lvgl_album_art(root, panel, bg, accent, teal, dim, line, screen.album_art_cache_path);

    const std::string title = !screen.rows.empty() ? screen.rows[0].left : "No track selected";
    const std::string artist = screen.rows.size() > 1 ? screen.rows[1].left : "Open Library to play";
    const bool playing = !screen.rows.empty() && screen.rows[0].right == ">";
    const int pos = screen.position_seconds > 0 ? screen.position_seconds : parse_position_seconds(screen);
    const int duration = std::max(0, screen.duration_seconds);
    const MarqueeTiming title_timing{2200, 1600, 0, 18, 780};
    const MarqueeTiming artist_timing{2600, 1600, 400, 15, 900};
    lv_marquee_label(root, title, 95, 27, 138, ink, lv_font_now_title(), s_title_marquee, title_timing, 25, -2);
    lv_marquee_label(root, artist, 96, 52, 126, dim, lv_font_cjk(), s_artist_marquee, artist_timing, 20, -2);

    lv_obj_t *playback = lv_label(root, playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY, 96, 78, 28, accent, lv_font_symbol_title());
    lv_obj_set_style_text_align(playback, LV_TEXT_ALIGN_CENTER, 0);
    lv_label(root, format_mmss(pos), 130, 84, 46, ink, lv_font_normal());
    lv_label(root, "/", 179, 84, 8, dim, lv_font_small());
    lv_label(root, duration > 0 ? format_mmss(duration) : "--:--", 190, 84, 42, dim, lv_font_small());

    const int progress_width = 130;
    const int progress_max = std::max(1, duration);
    const int progress_value = duration > 0 ? std::min(progress_width, std::max(0, pos) * progress_width / progress_max) : 0;
    lv_rect(root, 96, 109, progress_width, 6, progress, 0, line, 1);
    if (progress_value > 3) {
        lv_rect(root, 98, 111, progress_value - 3, 2, accent);
    }

    draw_lvgl_now_playing_footer(root,
                                 screen.volume_percent,
                                 screen.repeat_mode,
                                 screen.shuffle_enabled,
                                 screen.lofi_active,
                                 chrome,
                                 accent,
                                 teal,
                                 ink,
                                 dim,
                                 line);
    const bool volume_warning_active =
        screen.volume_overlay_active && screen.status.find("Distortion risk") != std::string::npos;
    if (volume_warning_active) {
        draw_lvgl_volume_warning_banner(root, preview, accent, lv_font_small());
    }
    if (screen.volume_overlay_active) {
        draw_lvgl_volume_overlay(root, screen.volume_overlay_percent, bg, panel, accent, teal, ink, dim, line);
    }
    if (screen.mode_overlay_active) {
        draw_lvgl_mode_overlay(root, screen, bg, panel, accent, teal, ink, dim, line);
    }
    lv_refr_now(s_lvgl_display);
    return ESP_OK;
}

esp_err_t draw_screen_lvgl_playback_menu(const lofi::ScreenModel &screen)
{
    if (!s_lvgl_display) {
        return ESP_ERR_INVALID_STATE;
    }

    const lv_color_t bg = lv_rgb(20, 18, 24);
    const lv_color_t chrome = lv_rgb(13, 12, 16);
    const lv_color_t panel = lv_rgb(35, 32, 44);
    const lv_color_t preview = lv_rgb(28, 25, 35);
    const lv_color_t accent = lv_rgb(245, 174, 94);
    const lv_color_t teal = lv_rgb(125, 205, 205);
    const lv_color_t ink = lv_rgb(248, 233, 207);
    const lv_color_t dim = lv_rgb(162, 152, 138);
    const lv_color_t line = lv_rgb(69, 60, 71);
    const lv_color_t progress = lv_rgb(51, 45, 55);

    lv_obj_t *root = lv_screen_active();
    lv_obj_clean(root);
    lv_obj_set_style_bg_color(root, bg, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    draw_lvgl_header(root, "SETTINGS", chrome, accent, ink, dim, line, accent, screen.background_task_active, screen.background_task_frame);

    std::vector<PlaybackSettingItem> items;
    items.reserve(screen.rows.size());
    for (const auto &row : screen.rows) {
        items.push_back(setting_item_from_row(row));
    }
    if (items.empty()) {
        items.push_back({"Volume", "0%", LV_SYMBOL_VOLUME_MAX, true, 0});
    }

    const int selected = std::max(0, std::min<int>(selected_row_index(screen), static_cast<int>(items.size()) - 1));
    const bool warning_status = screen.status.find("Distortion risk") != std::string::npos;
    if (warning_status) {
        draw_lvgl_volume_warning_banner(root, preview, accent, lv_font_small());
    } else if (selected > 0) {
        draw_playback_setting_preview(root, items[selected - 1], 21, preview, ink, dim, line);
    } else if (items.size() > 1) {
        draw_playback_setting_preview(root, items[items.size() - 1], 21, preview, ink, dim, line);
    }

    draw_playback_setting_card(root, items[selected], bg, panel, accent, teal, ink, dim, line, progress);

    if (selected + 1 < static_cast<int>(items.size())) {
        draw_playback_setting_preview(root, items[selected + 1], 113, preview, ink, dim, line);
    } else if (items.size() > 1) {
        draw_playback_setting_preview(root, items[0], 113, preview, ink, dim, line);
    }

    draw_playback_setting_dots(root, selected, static_cast<int>(items.size()), accent, line);
    lv_refr_now(s_lvgl_display);
    return ESP_OK;
}

esp_err_t draw_screen_lvgl_queue(const lofi::ScreenModel &screen)
{
    if (!s_lvgl_display) {
        return ESP_ERR_INVALID_STATE;
    }

    const lv_color_t bg = lv_rgb(20, 18, 24);
    const lv_color_t chrome = lv_rgb(13, 12, 16);
    const lv_color_t panel = lv_rgb(35, 32, 44);
    const lv_color_t accent = lv_rgb(245, 174, 94);
    const lv_color_t teal = lv_rgb(125, 205, 205);
    const lv_color_t ink = lv_rgb(248, 233, 207);
    const lv_color_t dim = lv_rgb(162, 152, 138);
    const lv_color_t line = lv_rgb(69, 60, 71);

    lv_obj_t *root = lv_screen_active();
    lv_obj_clean(root);
    lv_obj_set_style_bg_color(root, bg, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    draw_lvgl_header(root, "QUEUE", chrome, accent, ink, dim, line, accent, screen.background_task_active, screen.background_task_frame);

    const QueuePageInfo info = parse_queue_status(screen.status);
    if (info.total <= 0 || screen.rows.empty()) {
        lv_label(root, LV_SYMBOL_LIST, 101, 42, 38, teal, lv_font_large_symbol());
        lv_obj_t *empty = lv_label(root, "QUEUE EMPTY", 0, 75, LCD_WIDTH, ink, lv_font_normal());
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_refr_now(s_lvgl_display);
        return ESP_OK;
    }

    char range[24] = {};
    std::snprintf(range, sizeof(range), "%03d-%03d / %03d", info.first, info.last, info.total);
    lv_label(root, range, 10, 20, 116, dim, lv_font_tiny());

    lv_obj_t *jump_icon = lv_label(root, LV_SYMBOL_RIGHT, 182, 20, 13, accent, lv_font_symbol_small());
    lv_obj_set_style_text_align(jump_icon, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_t *jump = lv_label(root, "+6", 198, 20, 31, accent, lv_font_tiny());
    lv_obj_set_style_text_align(jump, LV_TEXT_ALIGN_RIGHT, 0);

    lv_rect(root, 10, 33, 220, 1, line);
    constexpr int kRowY = 35;
    constexpr int kRowStep = 17;
    constexpr int kListH = 102;
    for (size_t i = 0; i < screen.rows.size() && i < 6; ++i) {
        const bool selected = !screen.rows[i].left.empty() && screen.rows[i].left[0] == '>';
        draw_lvgl_queue_row(root,
                            screen.rows[i],
                            kRowY + static_cast<int>(i) * kRowStep,
                            selected,
                            bg,
                            panel,
                            accent,
                            teal,
                            ink,
                            dim,
                            line);
    }

    const int total = std::max(1, info.total);
    const int first = std::max(1, info.first);
    const int last = std::max(first, info.last);
    const int thumb_y = kRowY + (first - 1) * kListH / total;
    const int thumb_h = std::max(6, (last - first + 1) * kListH / total);
    lv_rect(root, 234, kRowY, 2, kListH, line);
    lv_rect(root, 233, thumb_y, 4, std::min(kListH - (thumb_y - kRowY), thumb_h), accent);

    lv_refr_now(s_lvgl_display);
    return ESP_OK;
}

esp_err_t draw_screen_lvgl_lofi(const lofi::ScreenModel &screen)
{
    if (!s_lvgl_display) {
        return ESP_ERR_INVALID_STATE;
    }

    const lv_color_t bg = lv_rgb(20, 18, 24);
    const lv_color_t chrome = lv_rgb(13, 12, 16);
    const lv_color_t panel = lv_rgb(35, 32, 44);
    const lv_color_t accent = lv_rgb(245, 174, 94);
    const lv_color_t teal = lv_rgb(125, 205, 205);
    const lv_color_t ink = lv_rgb(248, 233, 207);
    const lv_color_t dim = lv_rgb(162, 152, 138);
    const lv_color_t line = lv_rgb(69, 60, 71);

    lv_obj_t *root = lv_screen_active();
    lv_obj_clean(root);
    lv_obj_set_style_bg_color(root, bg, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    draw_lvgl_header(root, "LO-FI", chrome, accent, ink, dim, line, accent, screen.background_task_active, screen.background_task_frame);
    const bool edit_mode = screen.title == "Lo-Fi Edit";
    lv_label(root, edit_mode ? "EDIT" : "LO-FI", 10, 21, 86, ink, edit_mode ? lv_font_header() : lv_font_title());
    lv_obj_t *summary = lv_label(root, lofi_strength_summary(screen.status, screen.meta), 124, 24, 106, accent, lv_font_small());
    lv_obj_set_style_text_align(summary, LV_TEXT_ALIGN_RIGHT, 0);

    const int row_y = edit_mode ? 38 : 40;
    const int row_h = edit_mode ? 18 : 21;
    const int row_step = edit_mode ? 19 : 22;
    const size_t max_rows = edit_mode ? 5 : 4;
    for (size_t i = 0; i < screen.rows.size() && i < max_rows; ++i) {
        std::string left = strip_selection_marker(screen.rows[i].left);
        const bool selected = !screen.rows[i].left.empty() && screen.rows[i].left[0] == '>';
        const int y = row_y + static_cast<int>(i) * row_step;
        if (selected) {
            lv_rect(root, 8, y, 224, row_h, panel, 2, line, 1);
            lv_rect(root, 11, y + 2, 3, row_h - 4, teal);
            lv_rect(root, 16, y + 2, 213, row_h - 4, bg, 1);
        } else {
            lv_rect(root, 12, y + row_h, 216, 1, line);
        }

        const lv_font_t *row_font = contains_non_ascii(left) ? lv_font_library_cjk_large() : lv_font_normal();
        const int text_y = edit_mode ? y : y + 2;
        const int label_w = edit_mode ? 88 : 142;
        lv_clipped_label(root, left, 28, text_y, label_w, row_h, -1, selected ? ink : dim, row_font);
        if (!screen.rows[i].right.empty()) {
            const lv_font_t *value_font = edit_mode ? lv_font_header() : lv_font_normal();
            lv_obj_t *value = lv_label(root,
                                       screen.rows[i].right,
                                       edit_mode ? 180 : 177,
                                       text_y,
                                       edit_mode ? 42 : 47,
                                       selected ? accent : dim,
                                       value_font);
            lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);
            if (edit_mode) {
                const int amount = std::max(0, std::min(100, std::atoi(screen.rows[i].right.c_str())));
                constexpr int bar_x = 118;
                constexpr int bar_w = 54;
                constexpr int bar_h = 4;
                const int bar_y = y + row_h - 6;
                lv_rect(root, bar_x, bar_y, bar_w, bar_h, line, 0);
                lv_rect(root, bar_x, bar_y, std::max(1, bar_w * amount / 100), bar_h, selected ? accent : teal, 0);
            }
        }
    }

    lv_refr_now(s_lvgl_display);
    return ESP_OK;
}

void draw_help_keyboard_icon(lv_obj_t *root, int x, int y, lv_color_t teal, lv_color_t accent, lv_color_t line)
{
    lv_rect(root, x, y + 2, 23, 13, lv_color_make(20, 18, 24), 2, teal, 1);
    for (int row = 0; row < 2; ++row) {
        for (int col = 0; col < 4; ++col) {
            const lv_color_t color = (row == 0 && col == 0) ? accent : line;
            lv_rect(root, x + 4 + col * 4, y + 5 + row * 4, 2, 2, color);
        }
    }
    lv_rect(root, x + 8, y + 13, 9, 1, teal);
}

esp_err_t draw_screen_lvgl_help(const lofi::ScreenModel &screen)
{
    if (!s_lvgl_display) {
        return ESP_ERR_INVALID_STATE;
    }

    const lv_color_t bg = lv_rgb(20, 18, 24);
    const lv_color_t chrome = lv_rgb(13, 12, 16);
    const lv_color_t accent = lv_rgb(245, 174, 94);
    const lv_color_t teal = lv_rgb(125, 205, 205);
    const lv_color_t ink = lv_rgb(248, 233, 207);
    const lv_color_t dim = lv_rgb(162, 152, 138);
    const lv_color_t line = lv_rgb(69, 60, 71);

    lv_obj_t *root = lv_screen_active();
    lv_obj_clean(root);
    lv_obj_set_style_bg_color(root, bg, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    draw_lvgl_header(root,
                     "HELP",
                     chrome,
                     accent,
                     ink,
                     dim,
                     line,
                     accent,
                     screen.background_task_active,
                     screen.background_task_frame);

    draw_help_keyboard_icon(root, 10, 23, teal, accent, line);
    lv_clipped_label(root, screen.subtitle, 40, 22, 132, 19, -1, ink, lv_font_normal());
    lv_obj_t *page = lv_label(root, screen.meta, 196, 24, 34, dim, lv_font_small());
    lv_obj_set_style_text_align(page, LV_TEXT_ALIGN_RIGHT, 0);

    constexpr int row_x = 9;
    constexpr int row_y = 43;
    constexpr int row_h = 15;
    constexpr int row_step = 15;
    for (size_t i = 0; i < screen.rows.size() && i < 5; ++i) {
        const int y = row_y + static_cast<int>(i) * row_step;
        lv_rect(root, row_x, y + row_h - 1, 222, 1, line);
        lv_rect(root, row_x, y, 48, 13, bg, 2, accent, 1);
        lv_obj_t *key = lv_label(root, screen.rows[i].left, row_x + 2, y - 3, 44, accent, lv_font_small());
        lv_obj_set_style_text_align(key, LV_TEXT_ALIGN_CENTER, 0);
        lv_clipped_label(root, screen.rows[i].right, 64, y - 1, 158, 15, -1, ink, lv_font_small());
    }

    lv_rect(root, 0, 119, LCD_WIDTH, 1, line);
    lv_label(root, "H CLOSE", 9, 121, 58, teal, lv_font_tiny());
    lv_obj_t *scroll = lv_label(root, screen.soft_center, 75, 121, 90, dim, lv_font_tiny());
    lv_obj_set_style_text_align(scroll, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *index = lv_label(root, screen.meta, 194, 121, 36, accent, lv_font_tiny());
    lv_obj_set_style_text_align(index, LV_TEXT_ALIGN_RIGHT, 0);

    lv_refr_now(s_lvgl_display);
    return ESP_OK;
}

esp_err_t draw_screen_lvgl_generic(const lofi::ScreenModel &screen)
{
    if (!s_lvgl_display) {
        return ESP_ERR_INVALID_STATE;
    }

    const lv_color_t bg = lv_rgb(20, 18, 24);
    const lv_color_t chrome = lv_rgb(13, 12, 16);
    const lv_color_t panel = lv_rgb(35, 32, 44);
    const lv_color_t accent = lv_rgb(245, 174, 94);
    const lv_color_t teal = lv_rgb(125, 205, 205);
    const lv_color_t ink = lv_rgb(248, 233, 207);
    const lv_color_t dim = lv_rgb(162, 152, 138);
    const lv_color_t line = lv_rgb(69, 60, 71);

    lv_obj_t *root = lv_screen_active();
    lv_obj_clean(root);
    lv_obj_set_style_bg_color(root, bg, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    draw_lvgl_header(root,
                     library_header_title(screen.title),
                     chrome,
                     accent,
                     ink,
                     dim,
                     line,
                     accent,
                     screen.background_task_active,
                     screen.background_task_frame);
    lv_clipped_label(root, library_header_title(screen.title), 10, 23, 156, 21, -1, ink, lv_font_title());
    if (!screen.status.empty()) {
        lv_clipped_label(root,
                         screen.status,
                         11,
                         48,
                         218,
                         17,
                         -1,
                         dim,
                         contains_non_ascii(screen.status) ? lv_font_library_cjk() : lv_font_small());
    }

    constexpr int kRowY = 70;
    constexpr int kRowH = 17;
    constexpr int kRowStep = 18;
    for (size_t i = 0; i < screen.rows.size() && i < 3; ++i) {
        std::string left = strip_selection_marker(screen.rows[i].left);
        const bool selected = !screen.rows[i].left.empty() && screen.rows[i].left[0] == '>';
        const int y = kRowY + static_cast<int>(i) * kRowStep;
        if (selected) {
            lv_rect(root, 8, y, 224, kRowH, panel, 2, line, 1);
            lv_rect(root, 11, y + 2, 3, kRowH - 4, teal);
            lv_rect(root, 16, y + 2, 213, kRowH - 4, bg, 1);
        } else {
            lv_rect(root, 12, y + kRowH, 216, 1, line);
        }
        lv_clipped_label(root,
                         left,
                         28,
                         y,
                         143,
                         kRowH,
                         -1,
                         selected ? ink : dim,
                         contains_non_ascii(left) ? lv_font_library_cjk() : lv_font_small());
        if (!screen.rows[i].right.empty()) {
            lv_obj_t *value = lv_label(root, localize_right_label(screen.rows[i].right), 178, y, 47, selected ? accent : dim, lv_font_small());
            lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);
        }
    }

    lv_refr_now(s_lvgl_display);
    return ESP_OK;
}

} // namespace

void release_album_art_cache(void)
{
    std::free(s_album_art_pixels);
    s_album_art_pixels = nullptr;
    s_album_art_dsc = {};
    s_album_art_cache_path.clear();
    s_album_art_valid = false;
}

#if LOFI_DEBUG_AUTOMATION_ENABLED
void set_framebuffer_capture_enabled(bool enabled)
{
    if (!enabled) {
        std::free(s_shadow_framebuffer);
        s_shadow_framebuffer = nullptr;
        s_shadow_framebuffer_valid = false;
        return;
    }
    if (!s_shadow_framebuffer) {
        s_shadow_framebuffer = static_cast<uint16_t *>(std::malloc(LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t)));
        if (!s_shadow_framebuffer) {
            ESP_LOGW(TAG, "FB shadow allocation failed bytes=%u",
                     static_cast<unsigned>(LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t)));
            s_shadow_framebuffer_valid = false;
            return;
        }
        s_shadow_framebuffer_valid = false;
    }
}
#endif

esp_err_t init_display(void)
{
    if (s_lcd_panel) {
        return ESP_OK;
    }

    spi_bus_config_t buscfg = {};
    buscfg.sclk_io_num = PIN_LCD_SCLK;
    buscfg.mosi_io_num = PIN_LCD_MOSI;
    buscfg.miso_io_num = GPIO_NUM_NC;
    buscfg.quadwp_io_num = GPIO_NUM_NC;
    buscfg.quadhd_io_num = GPIO_NUM_NC;
    buscfg.max_transfer_sz = LCD_WIDTH * 32 * sizeof(uint16_t);
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO), TAG, "lcd spi init");

    esp_lcd_panel_io_handle_t io_handle = nullptr;
    esp_lcd_panel_io_spi_config_t io_config = {};
    io_config.dc_gpio_num = PIN_LCD_DC;
    io_config.cs_gpio_num = PIN_LCD_CS;
    io_config.pclk_hz = 40 * 1000 * 1000;
    io_config.lcd_cmd_bits = 8;
    io_config.lcd_param_bits = 8;
    io_config.spi_mode = 0;
    io_config.trans_queue_depth = 10;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_config, &io_handle), TAG, "lcd panel io");
    s_lcd_panel_io = io_handle;

    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = PIN_LCD_RST;
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_config.bits_per_pixel = 16;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(io_handle, &panel_config, &s_lcd_panel), TAG, "st7789 panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_lcd_panel), TAG, "lcd reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_lcd_panel), TAG, "lcd init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_lcd_panel, true), TAG, "lcd invert");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(s_lcd_panel, LCD_GAP_X, LCD_GAP_Y), TAG, "lcd gap");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(s_lcd_panel, LCD_SWAP_XY), TAG, "lcd swap xy");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_lcd_panel, LCD_MIRROR_X, LCD_MIRROR_Y), TAG, "lcd mirror");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_lcd_panel, true), TAG, "lcd on");

    ledc_timer_config_t bl_timer = {};
    bl_timer.speed_mode = kBacklightLedcMode;
    bl_timer.duty_resolution = kBacklightLedcResolution;
    bl_timer.timer_num = kBacklightLedcTimer;
    bl_timer.freq_hz = 5000;
    bl_timer.clk_cfg = LEDC_AUTO_CLK;
    ESP_RETURN_ON_ERROR(ledc_timer_config(&bl_timer), TAG, "lcd bl timer");

    ledc_channel_config_t bl_channel = {};
    bl_channel.gpio_num = PIN_LCD_BL;
    bl_channel.speed_mode = kBacklightLedcMode;
    bl_channel.channel = kBacklightLedcChannel;
    bl_channel.intr_type = LEDC_INTR_DISABLE;
    bl_channel.timer_sel = kBacklightLedcTimer;
    bl_channel.duty = 0;
    bl_channel.hpoint = 0;
    ESP_RETURN_ON_ERROR(ledc_channel_config(&bl_channel), TAG, "lcd bl ledc");
    s_backlight_pwm_ready = true;
    s_screen_awake = false;

    ESP_RETURN_ON_ERROR(lcd_draw_solid(lcd_color565(8, 12, 18)), TAG, "lcd clear");
    ESP_RETURN_ON_ERROR(init_lvgl_display(), TAG, "lvgl init");
    return ESP_OK;
}

void set_screen_awake(bool awake)
{
    s_screen_awake = awake;
    apply_backlight();
}

bool screen_awake(void)
{
    return s_screen_awake;
}

void set_screen_brightness_percent(int percent)
{
    s_screen_brightness_percent = normalize_backlight_percent(percent);
    ESP_LOGI(TAG,
             "BACKLIGHT brightness=%d duty=%u/%u",
             s_screen_brightness_percent,
             static_cast<unsigned>(backlight_duty_for_state()),
             static_cast<unsigned>(kBacklightLedcMaxDuty));
    apply_backlight();
}

esp_err_t draw_screen(const lofi::ScreenModel &screen)
{
    if (!s_lcd_panel) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint16_t lvgl_bg = lcd_color565(20, 18, 24);
    if (screen.title == "Boot" && s_lvgl_display) {
        ESP_RETURN_ON_ERROR(preclear_panel_on_page_change(screen.title, lcd_color565(11, 13, 18)), TAG, "boot preclear");
        return draw_screen_lvgl_boot(screen);
    }
    if (screen.title == "Help" && s_lvgl_display) {
        ESP_RETURN_ON_ERROR(preclear_panel_on_page_change(screen.title, lvgl_bg), TAG, "help preclear");
        return draw_screen_lvgl_help(screen);
    }
    if (screen.title == "Now Playing" && s_lvgl_display) {
        ESP_RETURN_ON_ERROR(preclear_panel_on_page_change(screen.title, lvgl_bg), TAG, "now preclear");
        return draw_screen_lvgl_now_playing(screen);
    }
    if (screen.title == "Playback Menu" && s_lvgl_display) {
        ESP_RETURN_ON_ERROR(preclear_panel_on_page_change(screen.title, lvgl_bg), TAG, "playback menu preclear");
        return draw_screen_lvgl_playback_menu(screen);
    }
    if (screen.title == "Library" && s_lvgl_display) {
        ESP_RETURN_ON_ERROR(preclear_panel_on_page_change(screen.title, lvgl_bg), TAG, "home preclear");
        return draw_screen_lvgl_home(screen);
    }
    if (screen.title == "Queue" && s_lvgl_display) {
        ESP_RETURN_ON_ERROR(preclear_panel_on_page_change(screen.title, lvgl_bg), TAG, "queue preclear");
        return draw_screen_lvgl_queue(screen);
    }
    if ((screen.title == "Lo-Fi" || screen.title == "Lo-Fi Edit") && s_lvgl_display) {
        ESP_RETURN_ON_ERROR(preclear_panel_on_page_change(screen.title, lvgl_bg), TAG, "lofi preclear");
        return draw_screen_lvgl_lofi(screen);
    }
    if ((screen.title == "Library Root" || screen.title == "Songs" || screen.title == "Artists" ||
         screen.title == "Artist" || screen.title == "Albums" || screen.title == "Album" ||
         screen.title == "Folders" || screen.title == "Folder" || screen.title == "Playlists" ||
         screen.title == "Playlist" || screen.title == "Library Action") &&
        s_lvgl_display) {
        ESP_RETURN_ON_ERROR(preclear_panel_on_page_change(screen.title, lvgl_bg), TAG, "library preclear");
        return draw_screen_lvgl_library_list(screen);
    }
    if ((screen.title == "Scan" || screen.title == "Scan Music") && s_lvgl_display) {
        ESP_RETURN_ON_ERROR(preclear_panel_on_page_change(screen.title, lvgl_bg), TAG, "generic preclear");
        return draw_screen_lvgl_generic(screen);
    }
    if (s_lvgl_display) {
        ESP_RETURN_ON_ERROR(preclear_panel_on_page_change(screen.title, lvgl_bg), TAG, "generic fallback preclear");
        return draw_screen_lvgl_generic(screen);
    }
    ESP_LOGE(TAG, "LVGL display unavailable; cannot draw screen %s", screen.title.c_str());
    return ESP_ERR_INVALID_STATE;
}

void tick_display(void)
{
    if (s_lvgl_display) {
        lv_timer_handler();
    }
}

#if LOFI_DEBUG_AUTOMATION_ENABLED
void dump_framebuffer_to_serial(void)
{
    if (!s_shadow_framebuffer || !s_shadow_framebuffer_valid) {
        std::printf("FB_DUMP_ERROR reason=no_framebuffer\n");
        std::fflush(stdout);
        return;
    }

    ++s_framebuffer_dump_seq;
    const uint32_t hash = framebuffer_hash();
    std::printf("FB_DUMP_BEGIN seq=%lu width=%d height=%d format=rgb565_lcd_word bytes=%u hash=0x%08lx\n",
                static_cast<unsigned long>(s_framebuffer_dump_seq),
                LCD_WIDTH,
                LCD_HEIGHT,
                static_cast<unsigned>(LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t)),
                static_cast<unsigned long>(hash));

    constexpr int kChunkPixels = 32;
    char hex[kChunkPixels * 4 + 1] = {};
    for (int y = 0; y < LCD_HEIGHT; ++y) {
        for (int x = 0; x < LCD_WIDTH; x += kChunkPixels) {
            const int count = std::min(kChunkPixels, LCD_WIDTH - x);
            for (int i = 0; i < count; ++i) {
                const uint16_t word = s_shadow_framebuffer[y * LCD_WIDTH + x + i];
                std::snprintf(&hex[i * 4], sizeof(hex) - i * 4, "%04x", word);
            }
            hex[count * 4] = '\0';
            std::printf("FB_DUMP_DATA seq=%lu y=%d x=%d n=%d hex=%s\n",
                        static_cast<unsigned long>(s_framebuffer_dump_seq),
                        y,
                        x,
                        count,
                        hex);
        }
    }
    std::printf("FB_DUMP_END seq=%lu hash=0x%08lx\n",
                static_cast<unsigned long>(s_framebuffer_dump_seq),
                static_cast<unsigned long>(hash));
    std::fflush(stdout);
}
#endif

esp_err_t init_keyboard(void)
{
    s_keyboard_diag.stage = "bus";
    s_keyboard_diag.ready = false;
    s_keyboard_diag.init_err = ESP_ERR_INVALID_STATE;
    s_keyboard_diag.probe_err = ESP_ERR_INVALID_STATE;

    esp_err_t err = board_i2c_init();
    if (err != ESP_OK) {
        s_keyboard_diag.init_err = err;
        return err;
    }

    s_keyboard_diag.stage = "probe";
    err = i2c_probe(I2C_ADDR_TCA8418);
    s_keyboard_diag.probe_err = err;
    if (err != ESP_OK) {
        s_keyboard_diag.init_err = err;
        return err;
    }

    gpio_config_t int_cfg = {};
    int_cfg.pin_bit_mask = 1ULL << PIN_KBD_INT;
    int_cfg.mode = GPIO_MODE_INPUT;
    int_cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    int_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    int_cfg.intr_type = GPIO_INTR_DISABLE;
    s_keyboard_diag.stage = "gpio";
    err = gpio_config(&int_cfg);
    if (err != ESP_OK) {
        s_keyboard_diag.init_err = err;
        return err;
    }

#define KBD_WRITE_OR_RETURN(stage_name, reg, value)        \
    do {                                                   \
        s_keyboard_diag.stage = stage_name;                \
        err = i2c_write_reg(I2C_ADDR_TCA8418, reg, value); \
        if (err != ESP_OK) {                               \
            s_keyboard_diag.init_err = err;                \
            return err;                                    \
        }                                                  \
    } while (0)

    KBD_WRITE_OR_RETURN("dir1", TCA8418_REG_GPIO_DIR_1, 0x00);
    KBD_WRITE_OR_RETURN("dir2", TCA8418_REG_GPIO_DIR_2, 0x00);
    KBD_WRITE_OR_RETURN("dir3", TCA8418_REG_GPIO_DIR_3, 0x00);
    KBD_WRITE_OR_RETURN("gpi-em1", TCA8418_REG_GPI_EM_1, 0xFF);
    KBD_WRITE_OR_RETURN("gpi-em2", TCA8418_REG_GPI_EM_2, 0xFF);
    KBD_WRITE_OR_RETURN("gpi-em3", TCA8418_REG_GPI_EM_3, 0xFF);
    KBD_WRITE_OR_RETURN("int-lvl1", TCA8418_REG_GPIO_INT_LVL_1, 0x00);
    KBD_WRITE_OR_RETURN("int-lvl2", TCA8418_REG_GPIO_INT_LVL_2, 0x00);
    KBD_WRITE_OR_RETURN("int-lvl3", TCA8418_REG_GPIO_INT_LVL_3, 0x00);
    KBD_WRITE_OR_RETURN("int-en1", TCA8418_REG_GPIO_INT_EN_1, 0xFF);
    KBD_WRITE_OR_RETURN("int-en2", TCA8418_REG_GPIO_INT_EN_2, 0xFF);
    KBD_WRITE_OR_RETURN("int-en3", TCA8418_REG_GPIO_INT_EN_3, 0xFF);
    KBD_WRITE_OR_RETURN("rows", TCA8418_REG_KP_GPIO_1, 0x7F);
    KBD_WRITE_OR_RETURN("cols1", TCA8418_REG_KP_GPIO_2, 0xFF);
    KBD_WRITE_OR_RETURN("cols2", TCA8418_REG_KP_GPIO_3, 0x00);
    KBD_WRITE_OR_RETURN("debounce1", TCA8418_REG_DEBOUNCE_DIS_1, 0x00);
    KBD_WRITE_OR_RETURN("debounce2", TCA8418_REG_DEBOUNCE_DIS_2, 0x00);
    KBD_WRITE_OR_RETURN("debounce3", TCA8418_REG_DEBOUNCE_DIS_3, 0x00);

    s_keyboard_diag.stage = "flush";
    err = tca8418_flush();
    if (err != ESP_OK) {
        s_keyboard_diag.init_err = err;
        return err;
    }

    KBD_WRITE_OR_RETURN("cfg", TCA8418_REG_CFG, TCA8418_REG_CFG_GPI_IEN | TCA8418_REG_CFG_KE_IEN);

#undef KBD_WRITE_OR_RETURN

    s_keyboard_ready = true;
    s_keyboard_diag.ready = true;
    s_keyboard_diag.stage = "task";
    err = start_keyboard_input_task();
    if (err != ESP_OK) {
        s_keyboard_ready = false;
        s_keyboard_diag.ready = false;
        s_keyboard_diag.init_err = err;
        return err;
    }
    err = enable_keyboard_interrupt();
    if (err != ESP_OK) {
        s_keyboard_ready = false;
        s_keyboard_diag.ready = false;
        s_keyboard_diag.init_err = err;
        return err;
    }
    s_keyboard_diag.init_err = ESP_OK;
    s_keyboard_diag.stage = "ready";
    return ESP_OK;
}

bool poll_action(lofi::Action &action, const char **key_name)
{
    if (!s_keyboard_event_queue) {
        return false;
    }
    KeyboardEvent event;
    if (xQueueReceive(s_keyboard_event_queue, &event, 0) != pdTRUE) {
        return false;
    }
    action = event.action;
    const uint32_t age_ms = static_cast<uint32_t>((xTaskGetTickCount() - event.tick) * portTICK_PERIOD_MS);
    s_keyboard_last_event_age_ms = age_ms;
    s_keyboard_max_event_age_ms = std::max(s_keyboard_max_event_age_ms, age_ms);
    std::strncpy(s_polled_key_name, event.key_name, sizeof(s_polled_key_name) - 1);
    s_polled_key_name[sizeof(s_polled_key_name) - 1] = '\0';
    if (key_name) {
        *key_name = s_polled_key_name;
    }
    ++s_keyboard_consumed_events;
    return true;
}

#if LOFI_DEBUG_AUTOMATION_ENABLED
KeyboardDiagnostics keyboard_diagnostics(void)
{
    s_keyboard_diag.ready = s_keyboard_ready;
    s_keyboard_diag.input_task_started = s_keyboard_task != nullptr;
    s_keyboard_diag.queue_depth =
        s_keyboard_event_queue ? static_cast<uint32_t>(uxQueueMessagesWaiting(s_keyboard_event_queue)) : 0;
    s_keyboard_diag.queued_events = s_keyboard_queued_events;
    s_keyboard_diag.consumed_events = s_keyboard_consumed_events;
    s_keyboard_diag.dropped_repeats = s_keyboard_dropped_repeats;
    s_keyboard_diag.dropped_events = s_keyboard_dropped_events;
    s_keyboard_diag.last_event_age_ms = s_keyboard_last_event_age_ms;
    s_keyboard_diag.max_event_age_ms = s_keyboard_max_event_age_ms;
    return s_keyboard_diag;
}
#endif

esp_err_t probe_i2c_device(uint8_t addr)
{
    ESP_RETURN_ON_ERROR(board_i2c_init(), TAG, "i2c bus");
    return i2c_probe(addr);
}

esp_err_t read_i2c_reg(uint8_t addr, uint8_t reg, uint8_t *data, size_t len)
{
    ESP_RETURN_ON_ERROR(board_i2c_init(), TAG, "i2c bus");
    return i2c_read_reg(addr, reg, data, len);
}

esp_err_t write_i2c_reg(uint8_t addr, uint8_t reg, uint8_t value)
{
    ESP_RETURN_ON_ERROR(board_i2c_init(), TAG, "i2c bus");
    return i2c_write_reg(addr, reg, value);
}

} // namespace lofi_board
