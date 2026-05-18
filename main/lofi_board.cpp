#include "lofi_board.hpp"

#include "board_pins.h"
#include "lofi_cjk_font.hpp"

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
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"

namespace lofi_board {
namespace {

const char *TAG = "lofi_board";

esp_lcd_panel_handle_t s_lcd_panel = nullptr;
esp_lcd_panel_io_handle_t s_lcd_panel_io = nullptr;
i2c_master_bus_handle_t s_i2c_bus = nullptr;
SemaphoreHandle_t s_i2c_mutex = nullptr;
bool s_keyboard_ready = false;
bool s_fn_down = false;
char s_last_key_name[12] = "-";
KeyboardDiagnostics s_keyboard_diag;
uint16_t s_shadow_framebuffer[LCD_WIDTH * LCD_HEIGHT] = {};
bool s_shadow_framebuffer_valid = false;
uint32_t s_framebuffer_dump_seq = 0;
lv_display_t *s_lvgl_display = nullptr;
esp_timer_handle_t s_lvgl_tick_timer = nullptr;
alignas(4) uint16_t s_lvgl_draw_buffer[LCD_WIDTH * 24] = {};
volatile bool s_lvgl_flush_pending = false;
lv_font_t s_lvgl_title_font;
lv_font_t s_lvgl_normal_font;
lv_font_t s_lvgl_small_font;
bool s_lvgl_font_fallbacks_ready = false;

uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return ((uint16_t)(r & 0xF8) << 8) | ((uint16_t)(g & 0xFC) << 3) | (uint16_t)(b >> 3);
}

uint16_t lcd_color565(uint8_t r, uint8_t g, uint8_t b)
{
    uint16_t color = rgb565(r, g, b);
    return (uint16_t)((color << 8) | (color >> 8));
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
        std::fill_n(&s_shadow_framebuffer[y * LCD_WIDTH], LCD_WIDTH, color);
    }
    s_shadow_framebuffer_valid = true;
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

esp_err_t lcd_fill_rect(int x0, int y0, int x1, int y1, uint16_t color)
{
    if (!s_lcd_panel) {
        return ESP_ERR_INVALID_STATE;
    }
    x0 = std::max(0, std::min(LCD_WIDTH, x0));
    x1 = std::max(0, std::min(LCD_WIDTH, x1));
    y0 = std::max(0, std::min(LCD_HEIGHT, y0));
    y1 = std::max(0, std::min(LCD_HEIGHT, y1));
    if (x0 >= x1 || y0 >= y1) {
        return ESP_OK;
    }

    uint16_t line[LCD_WIDTH];
    for (int x = x0; x < x1; ++x) {
        line[x - x0] = color;
    }
    for (int y = y0; y < y1; ++y) {
        ESP_RETURN_ON_ERROR(esp_lcd_panel_draw_bitmap(s_lcd_panel, x0, y, x1, y + 1, line), TAG, "fill rect");
        std::fill_n(&s_shadow_framebuffer[y * LCD_WIDTH + x0], x1 - x0, color);
    }
    s_shadow_framebuffer_valid = true;
    return ESP_OK;
}

uint32_t framebuffer_hash(void)
{
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < LCD_WIDTH * LCD_HEIGHT; ++i) {
        const uint16_t word = s_shadow_framebuffer[i];
        hash ^= static_cast<uint8_t>(word & 0xff);
        hash *= 16777619u;
        hash ^= static_cast<uint8_t>(word >> 8);
        hash *= 16777619u;
    }
    return hash;
}

uint8_t glyph3x5_row(char ch, int row)
{
    switch (ch) {
    case '0': {
        static const uint8_t r[5] = {7, 5, 5, 5, 7};
        return r[row];
    }
    case '1': {
        static const uint8_t r[5] = {2, 6, 2, 2, 7};
        return r[row];
    }
    case '2': {
        static const uint8_t r[5] = {7, 1, 7, 4, 7};
        return r[row];
    }
    case '3': {
        static const uint8_t r[5] = {7, 1, 7, 1, 7};
        return r[row];
    }
    case '4': {
        static const uint8_t r[5] = {5, 5, 7, 1, 1};
        return r[row];
    }
    case '5': {
        static const uint8_t r[5] = {7, 4, 7, 1, 7};
        return r[row];
    }
    case '6': {
        static const uint8_t r[5] = {7, 4, 7, 5, 7};
        return r[row];
    }
    case '7': {
        static const uint8_t r[5] = {7, 1, 1, 2, 2};
        return r[row];
    }
    case '8': {
        static const uint8_t r[5] = {7, 5, 7, 5, 7};
        return r[row];
    }
    case '9': {
        static const uint8_t r[5] = {7, 5, 7, 1, 7};
        return r[row];
    }
    case 'A': {
        static const uint8_t r[5] = {2, 5, 7, 5, 5};
        return r[row];
    }
    case 'B': {
        static const uint8_t r[5] = {6, 5, 6, 5, 6};
        return r[row];
    }
    case 'C': {
        static const uint8_t r[5] = {7, 4, 4, 4, 7};
        return r[row];
    }
    case 'D': {
        static const uint8_t r[5] = {6, 5, 5, 5, 6};
        return r[row];
    }
    case 'E': {
        static const uint8_t r[5] = {7, 4, 6, 4, 7};
        return r[row];
    }
    case 'F': {
        static const uint8_t r[5] = {7, 4, 6, 4, 4};
        return r[row];
    }
    case 'G': {
        static const uint8_t r[5] = {7, 4, 5, 5, 7};
        return r[row];
    }
    case 'H': {
        static const uint8_t r[5] = {5, 5, 7, 5, 5};
        return r[row];
    }
    case 'I': {
        static const uint8_t r[5] = {7, 2, 2, 2, 7};
        return r[row];
    }
    case 'J': {
        static const uint8_t r[5] = {1, 1, 1, 5, 7};
        return r[row];
    }
    case 'K': {
        static const uint8_t r[5] = {5, 5, 6, 5, 5};
        return r[row];
    }
    case 'L': {
        static const uint8_t r[5] = {4, 4, 4, 4, 7};
        return r[row];
    }
    case 'M': {
        static const uint8_t r[5] = {5, 7, 7, 5, 5};
        return r[row];
    }
    case 'N': {
        static const uint8_t r[5] = {5, 7, 7, 7, 5};
        return r[row];
    }
    case 'O': {
        static const uint8_t r[5] = {7, 5, 5, 5, 7};
        return r[row];
    }
    case 'P': {
        static const uint8_t r[5] = {7, 5, 7, 4, 4};
        return r[row];
    }
    case 'Q': {
        static const uint8_t r[5] = {7, 5, 5, 7, 1};
        return r[row];
    }
    case 'R': {
        static const uint8_t r[5] = {6, 5, 6, 5, 5};
        return r[row];
    }
    case 'S': {
        static const uint8_t r[5] = {7, 4, 7, 1, 7};
        return r[row];
    }
    case 'T': {
        static const uint8_t r[5] = {7, 2, 2, 2, 2};
        return r[row];
    }
    case 'U': {
        static const uint8_t r[5] = {5, 5, 5, 5, 7};
        return r[row];
    }
    case 'V': {
        static const uint8_t r[5] = {5, 5, 5, 5, 2};
        return r[row];
    }
    case 'W': {
        static const uint8_t r[5] = {5, 5, 7, 7, 5};
        return r[row];
    }
    case 'X': {
        static const uint8_t r[5] = {5, 5, 2, 5, 5};
        return r[row];
    }
    case 'Y': {
        static const uint8_t r[5] = {5, 5, 2, 2, 2};
        return r[row];
    }
    case 'Z': {
        static const uint8_t r[5] = {7, 1, 2, 4, 7};
        return r[row];
    }
    case ':': {
        static const uint8_t r[5] = {0, 2, 0, 2, 0};
        return r[row];
    }
    case '-': {
        static const uint8_t r[5] = {0, 0, 7, 0, 0};
        return r[row];
    }
    case '_': {
        static const uint8_t r[5] = {0, 0, 0, 0, 7};
        return r[row];
    }
    case '~': {
        static const uint8_t r[5] = {0, 0, 3, 6, 0};
        return r[row];
    }
    case '/': {
        static const uint8_t r[5] = {1, 1, 2, 4, 4};
        return r[row];
    }
    case '%': {
        static const uint8_t r[5] = {5, 1, 2, 4, 5};
        return r[row];
    }
    case '.': {
        static const uint8_t r[5] = {0, 0, 0, 0, 2};
        return r[row];
    }
    case '(': {
        static const uint8_t r[5] = {1, 2, 2, 2, 1};
        return r[row];
    }
    case ')': {
        static const uint8_t r[5] = {4, 2, 2, 2, 4};
        return r[row];
    }
    case '*': {
        static const uint8_t r[5] = {0, 5, 2, 5, 0};
        return r[row];
    }
    case '>': {
        static const uint8_t r[5] = {4, 2, 1, 2, 4};
        return r[row];
    }
    case '?': {
        static const uint8_t r[5] = {7, 1, 3, 0, 2};
        return r[row];
    }
    default:
        return 0;
    }
}

const cjk_font::Glyph *find_cjk_glyph(uint32_t codepoint)
{
    for (size_t i = 0; i < cjk_font::kGlyphCount; ++i) {
        if (cjk_font::kGlyphs[i].codepoint == codepoint) {
            return &cjk_font::kGlyphs[i];
        }
    }
    return nullptr;
}

const cjk_font::TinyGlyph *find_tiny_cjk_glyph(uint32_t codepoint)
{
    for (size_t i = 0; i < cjk_font::kTinyGlyphCount; ++i) {
        if (cjk_font::kTinyGlyphs[i].codepoint == codepoint) {
            return &cjk_font::kTinyGlyphs[i];
        }
    }
    return nullptr;
}

bool decode_utf8(const std::string &text, size_t &offset, uint32_t &codepoint)
{
    if (offset >= text.size()) {
        return false;
    }
    const unsigned char c0 = static_cast<unsigned char>(text[offset]);
    if (c0 < 0x80) {
        codepoint = c0;
        ++offset;
        return true;
    }
    if ((c0 & 0xE0) == 0xC0 && offset + 1 < text.size()) {
        const unsigned char c1 = static_cast<unsigned char>(text[offset + 1]);
        if ((c1 & 0xC0) == 0x80) {
            codepoint = ((c0 & 0x1F) << 6) | (c1 & 0x3F);
            offset += 2;
            return true;
        }
    } else if ((c0 & 0xF0) == 0xE0 && offset + 2 < text.size()) {
        const unsigned char c1 = static_cast<unsigned char>(text[offset + 1]);
        const unsigned char c2 = static_cast<unsigned char>(text[offset + 2]);
        if ((c1 & 0xC0) == 0x80 && (c2 & 0xC0) == 0x80) {
            codepoint = ((c0 & 0x0F) << 12) | ((c1 & 0x3F) << 6) | (c2 & 0x3F);
            offset += 3;
            return true;
        }
    }
    codepoint = '?';
    ++offset;
    return true;
}

int text_cell_width(uint32_t codepoint)
{
    if (codepoint < 0x80) {
        return 1;
    }
    return find_cjk_glyph(codepoint) ? 2 : 1;
}

void draw_ascii_char(int x, int y, char ch, int scale, uint16_t color)
{
    ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    for (int row = 0; row < 5; ++row) {
        const uint8_t bits = glyph3x5_row(ch, row);
        for (int col = 0; col < 3; ++col) {
            if (bits & (1 << (2 - col))) {
                const int px = x + col * scale;
                const int py = y + row * scale;
                lcd_fill_rect(px, py, px + scale, py + scale, color);
            }
        }
    }
}

int draw_cjk_char(int x, int y, const cjk_font::Glyph &glyph, int scale, uint16_t color)
{
    scale = std::max(1, scale);
    for (int row = 0; row < cjk_font::kGlyphHeight; ++row) {
        const uint16_t bits = glyph.rows[row];
        for (int col = 0; col < glyph.width && col < 12; ++col) {
            if (bits & (1 << (11 - col))) {
                const int px = x + col * scale;
                const int py = y + row * scale;
                lcd_fill_rect(px, py, px + scale, py + scale, color);
            }
        }
    }
    return (glyph.width + 1) * scale;
}

int draw_tiny_cjk_char(int x, int y, const cjk_font::TinyGlyph &glyph, uint16_t color)
{
    for (int row = 0; row < cjk_font::kTinyGlyphHeight; ++row) {
        const uint16_t bits = glyph.rows[row];
        for (int col = 0; col < glyph.width && col < 8; ++col) {
            if (bits & (1 << (7 - col))) {
                lcd_fill_rect(x + col, y + row, x + col + 1, y + row + 1, color);
            }
        }
    }
    return glyph.width + 1;
}

void draw_text(int x, int y, const std::string &text, int scale, uint16_t color)
{
    if (scale <= 0) {
        return;
    }
    size_t offset = 0;
    int cursor = x;
    size_t drawn = 0;
    while (offset < text.size() && drawn < 40) {
        uint32_t codepoint = 0;
        if (!decode_utf8(text, offset, codepoint) || codepoint < 0x20) {
            continue;
        }
        const cjk_font::Glyph *glyph = codepoint >= 0x80 ? find_cjk_glyph(codepoint) : nullptr;
        if (glyph) {
            cursor += draw_cjk_char(cursor, y - (scale > 1 ? 1 : 0), *glyph, scale, color);
        } else {
            const char ch = codepoint < 0x80 ? static_cast<char>(codepoint) : '?';
            draw_ascii_char(cursor, y, ch, scale, color);
            cursor += 4 * scale;
        }
        ++drawn;
    }
}

void draw_text_tiny(int x, int y, const std::string &text, uint16_t color)
{
    size_t offset = 0;
    int cursor = x;
    size_t drawn = 0;
    while (offset < text.size() && drawn < 48) {
        uint32_t codepoint = 0;
        if (!decode_utf8(text, offset, codepoint) || codepoint < 0x20) {
            continue;
        }
        const cjk_font::TinyGlyph *glyph = codepoint >= 0x80 ? find_tiny_cjk_glyph(codepoint) : nullptr;
        if (glyph) {
            cursor += draw_tiny_cjk_char(cursor, y, *glyph, color);
        } else {
            const char ch = codepoint < 0x80 ? static_cast<char>(codepoint) : '?';
            draw_ascii_char(cursor, y + 1, ch, 1, color);
            cursor += 4;
        }
        ++drawn;
    }
}

std::string strip_selection_marker(const std::string &value)
{
    size_t pos = 0;
    while (pos < value.size() && (value[pos] == '>' || value[pos] == '*' || value[pos] == ' ')) {
        ++pos;
    }
    return value.substr(pos);
}

std::string fit_label(const std::string &value, size_t max_chars)
{
    size_t offset = 0;
    size_t cells = 0;
    size_t last_good = 0;
    while (offset < value.size()) {
        uint32_t codepoint = 0;
        if (!decode_utf8(value, offset, codepoint)) {
            break;
        }
        const size_t width = static_cast<size_t>(text_cell_width(codepoint));
        if (cells + width > max_chars) {
            break;
        }
        cells += width;
        last_good = offset;
    }
    if (last_good >= value.size()) {
        return value;
    }
    if (max_chars <= 1) {
        return value.substr(0, last_good);
    }
    return value.substr(0, last_good) + "~";
}

std::vector<std::string> utf8_units(const std::string &value)
{
    std::vector<std::string> units;
    size_t offset = 0;
    while (offset < value.size()) {
        const size_t start = offset;
        uint32_t codepoint = 0;
        if (!decode_utf8(value, offset, codepoint) || codepoint < 0x20) {
            continue;
        }
        if (offset <= value.size() && offset > start) {
            units.push_back(value.substr(start, offset - start));
        }
    }
    return units;
}

std::string marquee_label(const std::string &value, size_t max_cells, size_t phase)
{
    if (max_cells == 0) {
        return "";
    }
    const std::string fitted = fit_label(value, max_cells);
    if (fitted == value) {
        return value;
    }

    std::vector<std::string> units = utf8_units(value);
    if (units.empty()) {
        return "";
    }
    units.push_back(" ");
    units.push_back(" ");
    units.push_back(" ");

    const size_t start = phase % units.size();
    std::string out;
    size_t cells = 0;
    for (size_t step = 0; step < units.size() && cells < max_cells; ++step) {
        const std::string &unit = units[(start + step) % units.size()];
        size_t unit_offset = 0;
        uint32_t codepoint = 0;
        if (!decode_utf8(unit, unit_offset, codepoint)) {
            codepoint = '?';
        }
        const size_t width = static_cast<size_t>(text_cell_width(codepoint));
        if (cells + width > max_cells) {
            break;
        }
        out += unit;
        cells += width;
    }
    return out;
}

std::string localize_title(const std::string &value)
{
    return value;
}

std::string localize_soft_key(const std::string &value)
{
    return value;
}

std::string localize_row_left(const std::string &value)
{
    return value;
}

std::string localize_right_label(const std::string &value)
{
    return value;
}

std::string row_hint(const std::string &page, const std::string &label, const std::string &right)
{
    if (page == "Library") {
        if (label == "Now Playing") {
            return "Default entry";
        }
        if (label == "Albums") {
            return "Play by album";
        }
        if (label == "Artists") {
            return "Browse by artist";
        }
        if (label == "Shuffle All") {
            return "Shuffle Music";
        }
        if (label == "Folder") {
            return "Loose files";
        }
        if (label == "Lo-Fi") {
            return "Tape color presets";
        }
    }
    if (page == "Lo-Fi") {
        if (label == "Off") {
            return "Clean playback";
        }
        if (label == "Warm Tape") {
            return "Warm tape wobble";
        }
        if (label == "Vinyl Cafe") {
            return "Vinyl noise";
        }
        if (label == "Rainy Window") {
            return "Soft rainy space";
        }
        if (label == "Tiny Radio") {
            return "Narrow radio";
        }
        if (label == "Late Night") {
            return "Low night tone";
        }
        if (label == "Custom") {
            return "Intensity custom";
        }
    }
    if (page == "Folder") {
        if (!right.empty()) {
            return fit_label(right, 18);
        }
        return "LOOSE MUSIC";
    }
    if ((page == "Albums" || page == "Artists" || page == "Queue") && !right.empty()) {
        return fit_label(right, 18);
    }
    return "";
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

int parse_volume_toast(const std::string &status)
{
    const std::string marker = "Volume ";
    if (status.rfind(marker, 0) != 0) {
        return -1;
    }
    return std::max(0, std::min(100, std::atoi(status.c_str() + marker.size())));
}

std::string format_mmss(int seconds)
{
    seconds = std::max(0, seconds);
    char buf[16] = {};
    std::snprintf(buf, sizeof(buf), "%02d:%02d", (seconds / 60) % 100, seconds % 60);
    return std::string(buf);
}

std::string lofi_strength_summary(const std::string &status)
{
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

void draw_list_icon(int x, int y, const std::string &page, const std::string &label, size_t row, uint16_t color)
{
    if (page == "Artists" || page == "Artist") {
        draw_text(x + 1, y, "A", 1, color);
        return;
    }
    if (page == "Albums" || page == "Album" || label.find("Albums") != std::string::npos) {
        lcd_fill_rect(x, y + 1, x + 8, y + 9, color);
        lcd_fill_rect(x + 2, y + 3, x + 6, y + 7, lcd_color565(20, 18, 24));
        return;
    }
    if (page == "Folder" || label.find("Folder") != std::string::npos) {
        lcd_fill_rect(x, y + 3, x + 10, y + 9, color);
        lcd_fill_rect(x + 1, y + 1, x + 5, y + 3, color);
        return;
    }
    if (label.find("Shuffle") != std::string::npos) {
        lcd_fill_rect(x, y + 2, x + 9, y + 3, color);
        lcd_fill_rect(x, y + 8, x + 9, y + 9, color);
        lcd_fill_rect(x + 7, y + 1, x + 10, y + 4, color);
        lcd_fill_rect(x + 7, y + 7, x + 10, y + 10, color);
        return;
    }
    if (label.find("Lo-Fi") != std::string::npos) {
        lcd_fill_rect(x + 1, y + 2, x + 9, y + 3, color);
        lcd_fill_rect(x + 1, y + 8, x + 9, y + 9, color);
        lcd_fill_rect(x + 1, y + 2, x + 2, y + 9, color);
        lcd_fill_rect(x + 8, y + 2, x + 9, y + 9, color);
        return;
    }
    if (row == 0 || label.find("Now Playing") != std::string::npos) {
        lcd_fill_rect(x + 2, y + 1, x + 3, y + 9, color);
        lcd_fill_rect(x + 3, y + 1, x + 8, y + 2, color);
        lcd_fill_rect(x + 7, y + 2, x + 8, y + 7, color);
        lcd_fill_rect(x, y + 7, x + 3, y + 10, color);
        return;
    }
    draw_text(x + 1, y, ">", 1, color);
}

void draw_radio_mark(int x, int y, bool selected, uint16_t color, uint16_t bg)
{
    lcd_fill_rect(x + 2, y, x + 6, y + 1, color);
    lcd_fill_rect(x + 2, y + 7, x + 6, y + 8, color);
    lcd_fill_rect(x, y + 2, x + 1, y + 6, color);
    lcd_fill_rect(x + 7, y + 2, x + 8, y + 6, color);
    lcd_fill_rect(x + 3, y + 3, x + 5, y + 6, selected ? color : bg);
}

void draw_soft_button(int x, int y, int w, const std::string &label, uint16_t fill, uint16_t ink)
{
    const std::string visible = localize_soft_key(label);
    lcd_fill_rect(x + 2, y, x + w - 2, y + 1, fill);
    lcd_fill_rect(x, y + 1, x + w, y + 11, fill);
    lcd_fill_rect(x + 2, y + 12, x + w - 2, y + 13, fill);
    draw_text(x + 9, y + 2, visible, 1, ink);
}

void draw_selected_row_frame(int x0, int y0, int x1, int y1, uint16_t fill, uint16_t accent)
{
    lcd_fill_rect(x0, y0, x1, y1, fill);
    lcd_fill_rect(x0 + 1, y0, x1 - 1, y0 + 1, accent);
    lcd_fill_rect(x0 + 1, y1 - 1, x1 - 1, y1, accent);
    lcd_fill_rect(x0, y0 + 1, x0 + 1, y1 - 1, accent);
    lcd_fill_rect(x1 - 1, y0 + 1, x1, y1 - 1, accent);
}

void draw_progress_bar(int x, int y, int w, int value, int max_value, uint16_t track, uint16_t fill)
{
    lcd_fill_rect(x, y, x + w, y + 5, track);
    int filled = 0;
    if (max_value > 0) {
        filled = std::max(0, std::min(w, value * w / max_value));
    }
    if (filled > 0) {
        lcd_fill_rect(x, y, x + filled, y + 5, fill);
    }
}

void draw_slider(int x, int y, int w, int value, uint16_t track, uint16_t fill)
{
    value = std::max(0, std::min(10, value));
    draw_progress_bar(x, y, w, value, 10, track, fill);
    const int knob = x + std::max(0, std::min(w - 3, value * w / 10));
    lcd_fill_rect(knob, y - 2, knob + 3, y + 7, fill);
}

int parse_row_value(const std::string &value)
{
    return std::max(0, std::min(100, std::atoi(value.c_str())));
}

void draw_album_thumb(int x, int y, int w, int h, uint16_t panel, uint16_t accent, uint16_t teal)
{
    lcd_fill_rect(x, y, x + w, y + h, panel);
    lcd_fill_rect(x + 1, y + 1, x + w - 1, y + 2, teal);
    lcd_fill_rect(x + 1, y + h - 2, x + w - 1, y + h - 1, teal);
    lcd_fill_rect(x + 1, y + 1, x + 2, y + h - 1, teal);
    lcd_fill_rect(x + w - 2, y + 1, x + w - 1, y + h - 1, teal);
    const int cx = x + w / 2;
    const int cy = y + h / 2;
    lcd_fill_rect(cx - 5, cy - 5, cx + 5, cy + 5, accent);
    lcd_fill_rect(cx - 13, cy - 8, cx - 11, cy + 9, teal);
    lcd_fill_rect(cx + 11, cy - 8, cx + 13, cy + 9, teal);
    lcd_fill_rect(cx - 14, cy - 9, cx + 14, cy - 7, teal);
    lcd_fill_rect(cx - 14, cy + 7, cx + 14, cy + 9, teal);
}

void draw_playback_glyph(int x, int y, bool playing, uint16_t accent, uint16_t bg)
{
    if (playing) {
        lcd_fill_rect(x, y, x + 8, y + 23, accent);
        lcd_fill_rect(x + 15, y, x + 23, y + 23, accent);
        return;
    }
    lcd_fill_rect(x, y, x + 4, y + 23, accent);
    lcd_fill_rect(x + 4, y + 3, x + 8, y + 20, accent);
    lcd_fill_rect(x + 8, y + 6, x + 12, y + 17, accent);
    lcd_fill_rect(x + 12, y + 9, x + 16, y + 14, accent);
    lcd_fill_rect(x + 16, y + 11, x + 20, y + 12, accent);
    lcd_fill_rect(x, y, x + 1, y + 23, bg);
}

void draw_music_note_icon(int x, int y, uint16_t color)
{
    lcd_fill_rect(x + 4, y, x + 6, y + 11, color);
    lcd_fill_rect(x + 6, y, x + 12, y + 2, color);
    lcd_fill_rect(x + 10, y + 2, x + 12, y + 10, color);
    lcd_fill_rect(x, y + 9, x + 6, y + 14, color);
    lcd_fill_rect(x + 7, y + 8, x + 13, y + 13, color);
}

void draw_footer_separator(int x, uint16_t color)
{
    lcd_fill_rect(x, 120, x + 1, 132, color);
}

void draw_speaker_icon(int x, int y, uint16_t color)
{
    lcd_fill_rect(x, y + 4, x + 4, y + 10, color);
    lcd_fill_rect(x + 4, y + 2, x + 8, y + 12, color);
    lcd_fill_rect(x + 10, y + 4, x + 11, y + 10, color);
    lcd_fill_rect(x + 12, y + 2, x + 13, y + 12, color);
}

void draw_repeat_icon(int x, int y, uint16_t color)
{
    lcd_fill_rect(x, y + 3, x + 12, y + 4, color);
    lcd_fill_rect(x + 10, y + 1, x + 14, y + 6, color);
    lcd_fill_rect(x + 2, y + 9, x + 14, y + 10, color);
    lcd_fill_rect(x, y + 7, x + 4, y + 12, color);
}

void draw_queue_icon(int x, int y, uint16_t color)
{
    lcd_fill_rect(x, y + 2, x + 12, y + 4, color);
    lcd_fill_rect(x, y + 6, x + 12, y + 8, color);
    lcd_fill_rect(x, y + 10, x + 12, y + 12, color);
    draw_text(x + 15, y + 3, ">", 1, color);
}

void draw_toast_overlay(const lofi::ScreenModel &screen,
                        uint16_t panel,
                        uint16_t panel2,
                        uint16_t accent,
                        uint16_t teal,
                        uint16_t ink,
                        uint16_t dim,
                        uint16_t progress)
{
    if (screen.status.find("SD ") != std::string::npos) {
        return;
    }

    const int x = 36;
    const int y = 51;
    const int w = 168;
    const int h = 43;
    lcd_fill_rect(x + 3, y + 3, x + w + 3, y + h + 3, panel2);
    lcd_fill_rect(x + 4, y, x + w - 4, y + 1, accent);
    lcd_fill_rect(x + 1, y + 1, x + w - 1, y + 3, accent);
    lcd_fill_rect(x, y + 4, x + w, y + h - 4, panel);
    lcd_fill_rect(x + 1, y + h - 3, x + w - 1, y + h - 1, accent);
    lcd_fill_rect(x + 4, y + h - 1, x + w - 4, y + h, accent);

    const int volume = parse_volume_toast(screen.status);
    if (volume >= 0) {
        draw_text(x + 22, y + 13, "VOLUME", 1, ink);
        draw_text(x + 113, y + 10, std::to_string(volume) + "%", 2, accent);
        draw_progress_bar(x + 52, y + 29, 96, volume, 100, progress, teal);
        return;
    }

    draw_text(x + 18, y + 14, fit_label(screen.status, 31), 1, ink);
    if (screen.status.rfind("Lo-Fi", 0) == 0 || screen.status.rfind("Repeat", 0) == 0 ||
        screen.status.rfind("Shuffle", 0) == 0) {
        draw_text(x + 18, y + 27, "SETTING UPDATED", 1, dim);
    } else {
        draw_text(x + 18, y + 27, "OK", 1, dim);
    }
}

void draw_volume_focus_overlay(int volume,
                               uint16_t panel,
                               uint16_t panel2,
                               uint16_t accent,
                               uint16_t teal,
                               uint16_t ink,
                               uint16_t progress)
{
    const int x = 35;
    const int y = 56;
    const int w = 170;
    const int h = 42;
    lcd_fill_rect(x + 3, y + 3, x + w + 3, y + h + 3, panel2);
    lcd_fill_rect(x + 4, y, x + w - 4, y + 1, accent);
    lcd_fill_rect(x + 1, y + 1, x + w - 1, y + 3, accent);
    lcd_fill_rect(x, y + 4, x + w, y + h - 4, panel);
    lcd_fill_rect(x + 1, y + h - 3, x + w - 1, y + h - 1, accent);
    lcd_fill_rect(x + 4, y + h - 1, x + w - 4, y + h, accent);
    draw_text(x + 20, y + 13, "VOLUME", 1, ink);
    draw_text(x + 112, y + 9, std::to_string(volume) + "%", 2, accent);
    draw_progress_bar(x + 52, y + 29, 96, volume, 100, progress, teal);
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

bool keyboard_poll(lofi::Action &action, const char **key_name)
{
    if (!s_keyboard_ready) {
        return false;
    }

    uint8_t count = 0;
    if (i2c_read_reg(I2C_ADDR_TCA8418, TCA8418_REG_KEY_LCK_EC, &count, 1) != ESP_OK || (count & 0x0F) == 0) {
        return false;
    }

    uint8_t raw = 0;
    if (i2c_read_reg(I2C_ADDR_TCA8418, TCA8418_REG_KEY_EVENT_A, &raw, 1) != ESP_OK || raw == 0) {
        return false;
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
    ESP_LOGI(TAG, "KBD raw=0x%02x key=%s pressed=%d row=%u col=%u action=%d",
             raw,
             s_last_key_name,
             pressed ? 1 : 0,
             static_cast<unsigned>(row),
             static_cast<unsigned>(col),
             static_cast<int>(mapped_action));
    if (!pressed) {
        return false;
    }

    action = mapped_action;
    if (action == lofi::Action::None) {
        if (std::strcmp(s_last_key_name, "C") == 0) {
            return true;
        }
        return false;
    }
    return true;
}

lv_color_t lv_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return lv_color_make(r, g, b);
}

const lv_font_t *lv_font_small(void)
{
#if LV_FONT_MONTSERRAT_12
    return &lv_font_montserrat_12;
#else
    return LV_FONT_DEFAULT;
#endif
}

const lv_font_t *lv_font_header(void)
{
#if LV_FONT_MONTSERRAT_10
    return &lv_font_montserrat_10;
#else
    return lv_font_small();
#endif
}

const lv_font_t *lv_font_normal(void)
{
#if LV_FONT_MONTSERRAT_14
    return &lv_font_montserrat_14;
#else
    return LV_FONT_DEFAULT;
#endif
}

const lv_font_t *lv_font_title(void)
{
#if LV_FONT_MONTSERRAT_20
    return &lv_font_montserrat_20;
#elif LV_FONT_MONTSERRAT_18
    return &lv_font_montserrat_18;
#else
    return LV_FONT_DEFAULT;
#endif
}

bool text_contains_non_ascii(const std::string &text)
{
    return std::any_of(text.begin(), text.end(), [](unsigned char ch) {
        return ch >= 0x80;
    });
}

const lv_font_t *lv_font_cjk(void)
{
#if LV_FONT_SOURCE_HAN_SANS_SC_16_CJK
    return &lv_font_source_han_sans_sc_16_cjk;
#elif LV_FONT_SOURCE_HAN_SANS_SC_14_CJK
    return &lv_font_source_han_sans_sc_14_cjk;
#else
    return LV_FONT_DEFAULT;
#endif
}

void init_lvgl_font_fallbacks(void)
{
    if (s_lvgl_font_fallbacks_ready) {
        return;
    }
    const lv_font_t *cjk = lv_font_cjk();
    s_lvgl_title_font = *lv_font_title();
    s_lvgl_title_font.fallback = cjk;
    s_lvgl_normal_font = *lv_font_normal();
    s_lvgl_normal_font.fallback = cjk;
    s_lvgl_small_font = *lv_font_small();
    s_lvgl_small_font.fallback = cjk;
    s_lvgl_font_fallbacks_ready = true;
}

const lv_font_t *lv_font_with_cjk_fallback(const lv_font_t *latin_font)
{
    init_lvgl_font_fallbacks();
    if (latin_font == lv_font_title()) {
        return &s_lvgl_title_font;
    }
    if (latin_font == lv_font_normal()) {
        return &s_lvgl_normal_font;
    }
    if (latin_font == lv_font_small()) {
        return &s_lvgl_small_font;
    }
    return latin_font;
}

const lv_font_t *lv_font_for_text(const std::string &text, const lv_font_t *latin_font)
{
    return text_contains_non_ascii(text) ? lv_font_with_cjk_fallback(latin_font) : latin_font;
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

void clear_lvgl_scene(void)
{
    if (!s_lvgl_display) {
        return;
    }
    lv_obj_t *root = lv_screen_active();
    lv_obj_clean(root);
    lv_obj_set_style_bg_color(root, lv_rgb(20, 18, 24), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_refr_now(s_lvgl_display);
}

void draw_lvgl_battery(lv_obj_t *root, lv_color_t ink, lv_color_t dim, lv_color_t fill)
{
    (void)ink;
    (void)dim;
    lv_obj_t *battery = lv_label(root, LV_SYMBOL_BATTERY_3, 219, 0, 20, fill, lv_font_normal());
    lv_obj_set_style_text_align(battery, LV_TEXT_ALIGN_CENTER, 0);
}

void draw_lvgl_header(lv_obj_t *root,
                      const std::string &title,
                      lv_color_t chrome,
                      lv_color_t accent,
                      lv_color_t ink,
                      lv_color_t dim,
                      lv_color_t line,
                      lv_color_t battery)
{
    lv_rect(root, 0, 0, LCD_WIDTH, 14, chrome);
    lv_rect(root, 0, 14, LCD_WIDTH, 1, line);
    lv_label(root, "LOFI", 4, 2, 42, accent, lv_font_header());
    lv_label(root, title, 152, 2, 68, dim, lv_font_header());
    draw_lvgl_battery(root, ink, dim, battery);
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

void draw_lvgl_soft_footer(lv_obj_t *root,
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

void draw_lvgl_now_playing_footer(lv_obj_t *root,
                                  lv_color_t chrome,
                                  lv_color_t accent,
                                  lv_color_t teal,
                                  lv_color_t ink,
                                  lv_color_t dim,
                                  lv_color_t line)
{
    lv_rect(root, 0, 119, LCD_WIDTH, 16, chrome);
    lv_rect(root, 0, 119, LCD_WIDTH, 1, line);

    lv_obj_t *note = lv_label(root, LV_SYMBOL_AUDIO, 8, 121, 24, teal, lv_font_normal());
    lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, 0);
    lv_label(root, "MENU", 36, 121, 44, ink, lv_font_normal());

    lv_rect(root, 104, 121, 1, 11, line);
    lv_obj_t *volume = lv_label(root, LV_SYMBOL_VOLUME_MID, 111, 121, 20, teal, lv_font_normal());
    lv_obj_set_style_text_align(volume, LV_TEXT_ALIGN_CENTER, 0);

    lv_rect(root, 138, 121, 1, 11, line);
    lv_obj_t *repeat = lv_label(root, LV_SYMBOL_LOOP, 144, 121, 22, accent, lv_font_normal());
    lv_obj_set_style_text_align(repeat, LV_TEXT_ALIGN_CENTER, 0);

    lv_rect(root, 172, 121, 1, 11, line);
    lv_obj_t *lofi = lv_label(root, "LOFI", 178, 121, 25, teal, lv_font_small());
    lv_obj_set_style_text_align(lofi, LV_TEXT_ALIGN_CENTER, 0);

    lv_rect(root, 209, 121, 1, 11, line);
    lv_obj_t *queue = lv_label(root, LV_SYMBOL_LIST, 216, 121, 16, ink, lv_font_normal());
    lv_obj_set_style_text_align(queue, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *queue_plus = lv_label(root, LV_SYMBOL_PLUS, 230, 124, 8, ink, lv_font_small());
    lv_obj_set_style_text_align(queue_plus, LV_TEXT_ALIGN_CENTER, 0);
}

void draw_lvgl_album_art(lv_obj_t *root,
                         lv_color_t panel,
                         lv_color_t bg,
                         lv_color_t accent,
                         lv_color_t teal,
                         lv_color_t dim,
                         lv_color_t line)
{
    lv_obj_t *album = lv_rect(root, 10, 23, 76, 78, panel, 1, teal, 1);
    lv_rect(album, 3, 3, 70, 72, bg);
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
    const lv_color_t progress = lv_rgb(51, 45, 55);

    lv_obj_t *root = lv_screen_active();
    lv_obj_clean(root);
    lv_obj_set_style_bg_color(root, bg, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    draw_lvgl_header(root, "RECORDED", chrome, accent, ink, dim, line, accent);
    draw_lvgl_album_art(root, panel, bg, accent, teal, dim, line);

    const std::string title = !screen.rows.empty() ? screen.rows[0].left : "No track selected";
    const std::string artist = screen.rows.size() > 1 ? screen.rows[1].left : "Open Library to play";
    const bool playing = !screen.rows.empty() && screen.rows[0].right == ">";
    const int pos = screen.position_seconds > 0 ? screen.position_seconds : parse_position_seconds(screen);
    const int duration = std::max(0, screen.duration_seconds);
    lv_label(root, title, 96, 30, 136, ink, lv_font_for_text(title, lv_font_title()), LV_LABEL_LONG_SCROLL);
    lv_label(root, artist, 97, 58, 110, dim, lv_font_for_text(artist, lv_font_normal()), LV_LABEL_LONG_SCROLL);

    lv_obj_t *playback = lv_label(root, playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY, 96, 76, 28, accent, lv_font_title());
    lv_obj_set_style_text_align(playback, LV_TEXT_ALIGN_CENTER, 0);
    lv_label(root, format_mmss(pos), 130, 82, 46, ink, lv_font_normal());
    lv_label(root, "/", 179, 82, 8, dim, lv_font_small());
    lv_label(root, duration > 0 ? format_mmss(duration) : "--:--", 190, 82, 42, dim, lv_font_small());

    const int progress_width = 130;
    const int progress_max = std::max(1, duration);
    const int progress_value = duration > 0 ? std::min(progress_width, std::max(0, pos) * progress_width / progress_max) : 0;
    lv_rect(root, 96, 105, progress_width, 6, progress, 0, line, 1);
    if (progress_value > 3) {
        lv_rect(root, 98, 107, progress_value - 3, 2, accent);
    }

    draw_lvgl_now_playing_footer(root, chrome, accent, teal, ink, dim, line);
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
    const lv_color_t panel = lv_rgb(36, 32, 43);
    const lv_color_t selected = lv_rgb(69, 48, 34);
    const lv_color_t accent = lv_rgb(245, 174, 94);
    const lv_color_t ink = lv_rgb(248, 233, 207);
    const lv_color_t dim = lv_rgb(162, 152, 138);
    const lv_color_t line = lv_rgb(69, 60, 71);
    const lv_color_t progress = lv_rgb(51, 45, 55);

    lv_obj_t *root = lv_screen_active();
    lv_obj_clean(root);
    lv_obj_set_style_bg_color(root, bg, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    draw_lvgl_header(root, "RECORDED", chrome, accent, ink, dim, line, accent);
    lv_rect(root, 14, 26, 212, 86, panel, 3, line, 1);
    lv_label(root, "Playback Menu", 24, 32, 130, ink, lv_font_normal());

    for (size_t i = 0; i < screen.rows.size() && i < 4; ++i) {
        const std::string left = strip_selection_marker(screen.rows[i].left);
        const bool row_selected = screen.rows[i].left.find('>') != std::string::npos;
        const int y = 54 + static_cast<int>(i) * 14;
        if (row_selected) {
            lv_rect(root, 22, y - 2, 196, 13, selected, 2);
        }
        lv_label(root, left, 28, y - 3, 88, row_selected ? ink : dim, lv_font_small());
        if (left == "Volume") {
            const int volume = std::max(0, std::min(100, std::atoi(screen.rows[i].right.c_str())));
            lv_rect(root, 104, y + 2, 70, 4, progress);
            lv_rect(root, 104, y + 2, 70 * volume / 100, 4, accent);
        }
        lv_label(root, screen.rows[i].right, 178, y - 3, 38, row_selected ? ink : dim, lv_font_small());
    }

    const lv_color_t footer_active = lv_rgb(92, 60, 37);
    draw_lvgl_soft_footer(root,
                          screen.soft_left,
                          screen.soft_center,
                          screen.soft_right,
                          true,
                          chrome,
                          panel,
                          footer_active,
                          ink,
                          line);
    lv_refr_now(s_lvgl_display);
    return ESP_OK;
}

} // namespace

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

    gpio_config_t bl_cfg = {};
    bl_cfg.pin_bit_mask = 1ULL << PIN_LCD_BL;
    bl_cfg.mode = GPIO_MODE_OUTPUT;
    ESP_RETURN_ON_ERROR(gpio_config(&bl_cfg), TAG, "lcd bl gpio");
    gpio_set_level(static_cast<gpio_num_t>(PIN_LCD_BL), 1);

    ESP_RETURN_ON_ERROR(lcd_draw_solid(lcd_color565(8, 12, 18)), TAG, "lcd clear");
    ESP_RETURN_ON_ERROR(init_lvgl_display(), TAG, "lvgl init");
    return ESP_OK;
}

esp_err_t draw_screen(const lofi::ScreenModel &screen)
{
    if (!s_lcd_panel) {
        return ESP_ERR_INVALID_STATE;
    }
    if (screen.title == "Now Playing" && s_lvgl_display) {
        return draw_screen_lvgl_now_playing(screen);
    }
    if (screen.title == "Playback Menu" && s_lvgl_display) {
        return draw_screen_lvgl_playback_menu(screen);
    }
    clear_lvgl_scene();

    const uint16_t bg = lcd_color565(20, 18, 24);
    const uint16_t chrome = lcd_color565(13, 12, 16);
    const uint16_t panel = lcd_color565(44, 40, 53);
    const uint16_t panel2 = lcd_color565(44, 40, 53);
    const uint16_t accent = lcd_color565(245, 174, 94);
    const uint16_t accent2 = lcd_color565(69, 51, 44);
    const uint16_t teal = lcd_color565(125, 205, 205);
    const uint16_t ink = lcd_color565(248, 233, 207);
    const uint16_t dim = lcd_color565(162, 152, 138);
    const uint16_t line = lcd_color565(69, 60, 71);
    const uint16_t progress = lcd_color565(51, 45, 55);

    ESP_RETURN_ON_ERROR(lcd_draw_solid(bg), TAG, "clear screen");
    ESP_RETURN_ON_ERROR(lcd_fill_rect(0, 0, LCD_WIDTH, 14, chrome), TAG, "top chrome");
    ESP_RETURN_ON_ERROR(lcd_fill_rect(0, 14, LCD_WIDTH, 15, line), TAG, "top line");
    draw_text(4, 3, "LOFI", 1, accent);
    draw_text(132, 3, fit_label(localize_title(screen.title), 20), 1, dim);

    bool suppress_toast = false;
    if (screen.title == "Now Playing") {
        const bool playing = !screen.rows.empty() && screen.rows[0].right == ">";
        const std::string title = screen.rows.empty() ? "No Track" : strip_selection_marker(screen.rows[0].left);
        const std::string artist_album = screen.rows.size() > 1 ? screen.rows[1].left : "Open Library";
        const int pos = screen.position_seconds > 0 ? screen.position_seconds : parse_position_seconds(screen);
        const int duration = std::max(0, screen.duration_seconds);
        const int volume_toast = parse_volume_toast(screen.status);
        const size_t scroll_phase = static_cast<size_t>(xTaskGetTickCount() / pdMS_TO_TICKS(600));

        if (volume_toast >= 0) {
            draw_text(82, 30, marquee_label(title, 18, scroll_phase), 2, ink);
            draw_text(82, 55, marquee_label(artist_album, 30, scroll_phase / 2), 1, teal);
            draw_volume_focus_overlay(volume_toast, panel, panel2, accent, teal, ink, progress);
            suppress_toast = true;
        } else {

            draw_album_thumb(6, 31, 66, 66, panel, accent, teal);
            draw_text(82, 30, marquee_label(title, 18, scroll_phase), 2, ink);
            draw_text(82, 55, marquee_label(artist_album, 30, scroll_phase / 2), 1, teal);
            draw_playback_glyph(82, 75, playing, accent, bg);

            const int progress_pos = duration > 0 ? std::min(duration, std::max(0, pos)) : 0;
            draw_text(124, 78, format_mmss(pos), 2, ink);
            draw_text(184, 82, duration > 0 ? format_mmss(duration) : "--:--", 1, dim);
            draw_progress_bar(82, 104, 150, progress_pos, std::max(1, duration), progress, accent);
        }
    } else if (screen.title == "Lo-Fi Edit") {
        draw_text(8, 22, fit_label(localize_title(screen.title), 18), 2, ink);
        int y = 44;
        for (size_t i = 0; i < screen.rows.size() && i < 4; ++i) {
            const bool selected = !screen.rows[i].left.empty() && screen.rows[i].left[0] == '>';
            const std::string left = localize_row_left(strip_selection_marker(screen.rows[i].left));
            const int value = parse_row_value(screen.rows[i].right) / 10;
            draw_text(12, y - 6, fit_label(left, 13), 1, selected ? ink : dim);
            draw_text(94, y - 5, std::to_string(value), 1, selected ? accent : dim);
            draw_slider(126, y - 2, 96, value, progress, selected ? teal : dim);
            y += 20;
        }
        draw_text_tiny(8, 107, "LEFT/RIGHT ADJUST ENTER SAVE", dim);
    } else if (screen.title == "Album" || screen.title == "Album Detail") {
        const std::string album = screen.rows.empty() ? "Album" : screen.rows[0].left;
        const std::string count = screen.rows.empty() ? "" : screen.rows[0].right;
        draw_text(8, 21, localize_title(screen.title), 2, ink);
        draw_album_thumb(9, 39, 42, 42, panel, accent, teal);
        draw_text(60, 36, fit_label(album, 20), 2, ink);
        draw_text(61, 57, fit_label(count.empty() ? "PLAY ALBUM" : count + " tracks", 20), 1, teal);
        draw_text_tiny(61, 69, "ENTER PLAYS ALBUM", dim);
        int y = 82;
        for (size_t i = 1; i < screen.rows.size() && i < 4; ++i) {
            const bool selected = !screen.rows[i].left.empty() && screen.rows[i].left[0] == '>';
            if (selected) {
                draw_selected_row_frame(7, y - 3, LCD_WIDTH - 7, y + 14, accent2, accent);
            } else {
                ESP_RETURN_ON_ERROR(lcd_fill_rect(8, y + 13, LCD_WIDTH - 8, y + 14, line), TAG, "album row separator");
            }
            draw_text(13, y, fit_label(strip_selection_marker(screen.rows[i].left), 27), 1, selected ? ink : dim);
            y += 17;
        }
    } else if (screen.title == "Folder") {
        draw_text(8, 22, "Loose Files", 2, ink);
        draw_text(9, 42, "/Music / Inbox", 1, dim);
        const size_t max_rows = 3;
        size_t start = 0;
        if (screen.rows.size() > max_rows) {
            for (size_t i = 0; i < screen.rows.size(); ++i) {
                if (!screen.rows[i].left.empty() && screen.rows[i].left[0] == '>') {
                    if (i >= max_rows) {
                        start = i - max_rows + 1;
                    }
                    break;
                }
            }
        }
        int y = 55;
        for (size_t i = start; i < screen.rows.size() && i < start + max_rows; ++i) {
            const bool selected = !screen.rows[i].left.empty() && screen.rows[i].left[0] == '>';
            const std::string title = strip_selection_marker(screen.rows[i].left);
            if (selected) {
                draw_selected_row_frame(6, y - 4, LCD_WIDTH - 6, y + 18, accent2, accent);
            } else {
                ESP_RETURN_ON_ERROR(lcd_fill_rect(7, y + 17, LCD_WIDTH - 7, y + 18, line), TAG, "folder row separator");
            }
            draw_list_icon(14, y + 1, screen.title, "Folder", i - start, selected ? teal : dim);
            draw_text(31, y - 1, fit_label(title, 21), 1, selected ? ink : dim);
            draw_text_tiny(31, y + 13, selected ? "ENTER PLAYS FILE" : fit_label(screen.rows[i].right, 18), dim);
            if (!screen.rows[i].right.empty()) {
                lcd_fill_rect(196, y, 233, y + 13, selected ? panel : panel2);
                draw_text(201, y + 2, fit_label(screen.rows[i].right, 7), 1, selected ? teal : dim);
            }
            y += 23;
        }
    } else if (screen.title == "Scan") {
        int tracks = 0;
        int albums = 0;
        if (!screen.rows.empty()) {
            tracks = parse_row_value(screen.rows[0].right);
        }
        if (screen.rows.size() > 1) {
            albums = parse_row_value(screen.rows[1].right);
        }
        draw_text(8, 22, "Scan Music", 2, ink);
        draw_text(8, 43, "/Music", 1, dim);
        const int progress_max = std::max(1, tracks + std::max(48, albums * 2));
        draw_progress_bar(8, 62, 224, tracks, progress_max, progress, teal);
        draw_text(8, 81, std::to_string(albums) + " albums / " + std::to_string(tracks) + " tracks", 1, ink);
        draw_text_tiny(8, 100, "READING /Music", dim);
        if (screen.rows.size() > 3 && !screen.rows[3].left.empty()) {
            draw_text_tiny(8, 110, fit_label(screen.rows[3].left, 30), dim);
        }
    } else if (screen.title == "Queue") {
        draw_text(8, 22, "Queue", 2, ink);
        std::string current = "Current queue";
        size_t total_rows = screen.rows.size();
        for (const lofi::ScreenLine &row : screen.rows) {
            if (row.right == "NOW" || row.left.find('*') != std::string::npos) {
                current = "Now " + fit_label(strip_selection_marker(row.left), 18);
                break;
            }
        }
        for (const lofi::ScreenLine &row : screen.rows) {
            const int value = parse_row_value(row.right);
            if (value > 0) {
                total_rows = std::max(total_rows, static_cast<size_t>(value));
            }
        }
        draw_text_tiny(8, 40, current, dim);
        draw_text_tiny(171, 40, std::to_string(total_rows) + " tracks", dim);

        const size_t max_rows = 3;
        int y = 50;
        for (size_t i = 0; i < screen.rows.size() && i < max_rows; ++i) {
            const bool selected = !screen.rows[i].left.empty() && screen.rows[i].left[0] == '>';
            const bool now = screen.rows[i].right == "NOW" || screen.rows[i].left.find('*') != std::string::npos;
            const std::string title = strip_selection_marker(screen.rows[i].left);
            if (selected) {
                draw_selected_row_frame(6, y - 4, LCD_WIDTH - 6, y + 18, accent2, accent);
            } else {
                ESP_RETURN_ON_ERROR(lcd_fill_rect(7, y + 17, LCD_WIDTH - 7, y + 18, line), TAG, "queue row separator");
            }
            draw_text(14, y, now ? ">" : screen.rows[i].right, 1, now ? teal : dim);
            draw_text(31, y - 1, fit_label(title, 20), 1, selected ? ink : dim);
            draw_text_tiny(31, y + 13, now ? "NOW PLAYING" : "NEXT", dim);
            if (!screen.rows[i].right.empty()) {
                lcd_fill_rect(196, y, 233, y + 13, selected ? panel : panel2);
                draw_text(201, y + 2, fit_label(localize_right_label(screen.rows[i].right), 7), 1, now ? teal : (selected ? accent : dim));
            }
            y += 23;
        }
        draw_text_tiny(8, 106, "UP/DOWN MOVE ENTER PLAY", dim);
    } else if (screen.title == "Lo-Fi") {
        draw_text(8, 22, "Lo-Fi", 2, ink);
        draw_text_tiny(156, 31, lofi_strength_summary(screen.status), accent);
        int y = 43;
        for (size_t i = 0; i < screen.rows.size() && i < 3; ++i) {
            const bool selected = !screen.rows[i].left.empty() && screen.rows[i].left[0] == '>';
            const std::string left = localize_row_left(strip_selection_marker(screen.rows[i].left));
            if (selected) {
                draw_selected_row_frame(6, y - 4, LCD_WIDTH - 6, y + 18, accent2, accent);
            } else {
                ESP_RETURN_ON_ERROR(lcd_fill_rect(7, y + 17, LCD_WIDTH - 7, y + 18, line), TAG, "lofi row separator");
            }
            draw_radio_mark(12, y + 1, selected, selected ? accent : dim, bg);
            draw_text(27, y - 1, fit_label(left, 17), 1, selected ? ink : dim);
            const std::string hint = row_hint(screen.title, left, screen.rows[i].right);
            if (!hint.empty()) {
                draw_text_tiny(27, y + 13, fit_label(hint, 25), dim);
            }
            if (!screen.rows[i].right.empty()) {
                lcd_fill_rect(196, y, 233, y + 13, selected ? panel : panel2);
                draw_text(201, y + 2, fit_label(screen.rows[i].right, 7), 1, selected ? teal : dim);
            }
            y += 23;
        }
        draw_text_tiny(8, 106, "BACK ENTER APPLY MENU EDIT", dim);
    } else {
        draw_text(8, 22, localize_title(screen.title), 2, ink);
        const bool rich_rows = screen.title == "Library";
        int y = rich_rows ? 38 : 40;
        const int step = rich_rows ? 24 : 21;
        const size_t max_rows = rich_rows ? 3 : 4;
        size_t start = 0;
        if (rich_rows && screen.rows.size() > max_rows) {
            for (size_t i = 0; i < screen.rows.size(); ++i) {
                if (!screen.rows[i].left.empty() && screen.rows[i].left[0] == '>') {
                    if (i >= max_rows) {
                        start = i - max_rows + 1;
                    }
                    break;
                }
            }
        }
        for (size_t i = start; i < screen.rows.size() && i < start + max_rows; ++i) {
            const bool selected = !screen.rows[i].left.empty() && screen.rows[i].left[0] == '>';
            const std::string left = localize_row_left(strip_selection_marker(screen.rows[i].left));
            if (selected) {
                draw_selected_row_frame(6, y - 3, LCD_WIDTH - 6, y + (rich_rows ? 19 : 16), accent2, accent);
            } else {
                const int sep_y = y + (rich_rows ? 18 : 15);
                ESP_RETURN_ON_ERROR(lcd_fill_rect(7, sep_y, LCD_WIDTH - 7, sep_y + 1, line), TAG, "row separator");
            }
            draw_list_icon(13, y + 1, screen.title, left, i - start, selected ? teal : dim);
            if (rich_rows) {
                draw_text(31, y - 1, fit_label(left, 17), 1, selected ? ink : dim);
            } else {
                draw_text(31, y - 1, fit_label(left, 17), 1, selected ? ink : dim);
            }
            const std::string hint = row_hint(screen.title, left, screen.rows[i].right);
            if (!hint.empty()) {
                draw_text_tiny(31, y + (rich_rows ? 14 : 12), fit_label(hint, rich_rows ? 24 : 26), dim);
            }
            if (!screen.rows[i].right.empty()) {
                lcd_fill_rect(196, y, 233, y + 13, selected ? panel : panel2);
                draw_text(201, y + 2, fit_label(localize_right_label(screen.rows[i].right), 7), 1, selected ? teal : dim);
            }
            y += step;
        }
    }

    if (!suppress_toast) {
        draw_toast_overlay(screen, panel, panel2, accent, teal, ink, dim, progress);
    }

    if (screen.title == "Now Playing") {
        ESP_RETURN_ON_ERROR(lcd_fill_rect(0, 117, LCD_WIDTH, LCD_HEIGHT, chrome), TAG, "compact footer chrome");
        ESP_RETURN_ON_ERROR(lcd_fill_rect(0, 117, LCD_WIDTH, 118, line), TAG, "compact footer line");
        draw_music_note_icon(8, 121, teal);
        draw_text(27, 123, "MENU", 1, ink);
        draw_footer_separator(128, line);
        draw_speaker_icon(138, 121, teal);
        draw_footer_separator(158, line);
        draw_repeat_icon(168, 121, accent);
        draw_footer_separator(188, line);
        draw_text(196, 123, "LOFI", 1, teal);
        draw_footer_separator(217, line);
        draw_queue_icon(224, 121, ink);
    } else {
        ESP_RETURN_ON_ERROR(lcd_fill_rect(0, 115, LCD_WIDTH, LCD_HEIGHT, chrome), TAG, "footer chrome");
        ESP_RETURN_ON_ERROR(lcd_fill_rect(0, 115, LCD_WIDTH, 116, line), TAG, "footer line");
        draw_soft_button(6, 119, 63, screen.soft_left, panel2, ink);
        draw_soft_button(89, 119, 63, screen.soft_center, panel2, ink);
        draw_soft_button(171, 119, 63, screen.soft_right, panel2, ink);
    }
    return ESP_OK;
}

void tick_display(void)
{
    if (s_lvgl_display) {
        lv_timer_handler();
    }
}

esp_err_t draw_color_test(void)
{
    clear_lvgl_scene();
    ESP_RETURN_ON_ERROR(lcd_draw_solid(lcd_color565(0, 0, 0)), TAG, "color test clear");
    draw_text(6, 4, "RGB COLOR TEST", 1, lcd_color565(255, 255, 255));
    draw_text_tiny(132, 5, "C=TEST D=DUMP", lcd_color565(180, 180, 180));

    struct Swatch {
        const char *label;
        uint8_t r;
        uint8_t g;
        uint8_t b;
    };
    constexpr Swatch swatches[] = {
        {"RED", 255, 0, 0},
        {"GREEN", 0, 255, 0},
        {"BLUE", 0, 0, 255},
        {"YELLOW", 255, 255, 0},
        {"CYAN", 0, 255, 255},
        {"MAGENTA", 255, 0, 255},
        {"WHITE", 255, 255, 255},
        {"BLACK", 0, 0, 0},
    };

    for (size_t i = 0; i < sizeof(swatches) / sizeof(swatches[0]); ++i) {
        const int col = static_cast<int>(i % 4);
        const int row = static_cast<int>(i / 4);
        const int x = 8 + col * 58;
        const int y = 24 + row * 45;
        const Swatch &s = swatches[i];
        const uint16_t color = lcd_color565(s.r, s.g, s.b);
        ESP_RETURN_ON_ERROR(lcd_fill_rect(x, y, x + 32, y + 20, color), TAG, "color test swatch");
        ESP_RETURN_ON_ERROR(lcd_fill_rect(x - 1, y - 1, x + 33, y, lcd_color565(96, 96, 96)), TAG, "color test border");
        ESP_RETURN_ON_ERROR(lcd_fill_rect(x - 1, y + 20, x + 33, y + 21, lcd_color565(96, 96, 96)), TAG, "color test border");
        ESP_RETURN_ON_ERROR(lcd_fill_rect(x - 1, y - 1, x, y + 21, lcd_color565(96, 96, 96)), TAG, "color test border");
        ESP_RETURN_ON_ERROR(lcd_fill_rect(x + 32, y - 1, x + 33, y + 21, lcd_color565(96, 96, 96)), TAG, "color test border");
        draw_text_tiny(x, y + 25, s.label, lcd_color565(210, 210, 210));
    }

    draw_text_tiny(8, 116, "EXPECTED: RED GREEN BLUE YELLOW CYAN MAGENTA WHITE BLACK", lcd_color565(180, 180, 180));
    return ESP_OK;
}

void dump_framebuffer_to_serial(void)
{
    if (!s_shadow_framebuffer_valid) {
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
    s_keyboard_diag.init_err = ESP_OK;
    s_keyboard_diag.stage = "ready";
    return ESP_OK;
}

bool poll_action(lofi::Action &action, const char **key_name)
{
    return keyboard_poll(action, key_name);
}

KeyboardDiagnostics keyboard_diagnostics(void)
{
    s_keyboard_diag.ready = s_keyboard_ready;
    return s_keyboard_diag;
}

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
