#include "lofi_audio.hpp"

#include "lofi_board.hpp"
#include "lofi_dsp.hpp"
#include "lofi_wav.hpp"

#include "board_pins.h"

#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <strings.h>

#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_audio_dec_default.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_heap_caps.h"
#include "impl/esp_aac_dec.h"
#include "impl/esp_m4a_dec.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "soc/soc_caps.h"

namespace lofi_audio {
namespace {

const char *TAG = "lofi_audio";
constexpr uint32_t kAudioTaskStackBytes = 32768;
constexpr UBaseType_t kAudioTaskPriority = 8;
constexpr size_t kSimpleDecoderInputBytes = 8 * 1024;
constexpr size_t kSimpleDecoderOutputBytes = 8 * 1024;

enum class CommandType {
    Play,
    Stop,
    Pause,
    Volume,
    Profile,
    Seek,
};

struct Command {
    CommandType type = CommandType::Stop;
    char path[192] = {};
    int volume = 70;
    int seconds = 0;
    lofi::LofiProfile profile;
};

QueueHandle_t s_queue = nullptr;
TaskHandle_t s_task = nullptr;
portMUX_TYPE s_status_mux = portMUX_INITIALIZER_UNLOCKED;
Snapshot s_snapshot;
bool s_simple_decoders_registered = false;
bool s_codec_ready = false;
int s_codec_sample_rate = 0;
int s_requested_volume = 70;
int s_codec_volume_reg = -1;
bool s_codec_muted = false;

struct Es8311ClockCoeff {
    uint32_t rate;
    uint32_t mclk;
    uint8_t pre_div;
    uint8_t pre_multi;
    uint8_t adc_div;
    uint8_t dac_div;
    uint8_t fs_mode;
    uint8_t lrck_h;
    uint8_t lrck_l;
    uint8_t bclk_div;
    uint8_t adc_osr;
    uint8_t dac_osr;
};

constexpr Es8311ClockCoeff kEs8311BclkCoeffs[] = {
    {22050, 705600, 0x01, 0x03, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {32000, 1024000, 0x01, 0x03, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {44100, 1411200, 0x01, 0x03, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {48000, 1536000, 0x01, 0x03, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
};

constexpr uint8_t ES8311_RESET_REG00 = 0x00;
constexpr uint8_t ES8311_CLK_MANAGER_REG01 = 0x01;
constexpr uint8_t ES8311_CLK_MANAGER_REG02 = 0x02;
constexpr uint8_t ES8311_CLK_MANAGER_REG03 = 0x03;
constexpr uint8_t ES8311_CLK_MANAGER_REG04 = 0x04;
constexpr uint8_t ES8311_CLK_MANAGER_REG05 = 0x05;
constexpr uint8_t ES8311_CLK_MANAGER_REG06 = 0x06;
constexpr uint8_t ES8311_CLK_MANAGER_REG07 = 0x07;
constexpr uint8_t ES8311_CLK_MANAGER_REG08 = 0x08;
constexpr uint8_t ES8311_SDPIN_REG09 = 0x09;
constexpr uint8_t ES8311_SDPOUT_REG0A = 0x0a;
constexpr uint8_t ES8311_SYSTEM_REG0D = 0x0d;
constexpr uint8_t ES8311_SYSTEM_REG0E = 0x0e;
constexpr uint8_t ES8311_SYSTEM_REG12 = 0x12;
constexpr uint8_t ES8311_SYSTEM_REG13 = 0x13;
constexpr uint8_t ES8311_SYSTEM_REG14 = 0x14;
constexpr uint8_t ES8311_ADC_REG17 = 0x17;
constexpr uint8_t ES8311_ADC_REG1C = 0x1c;
constexpr uint8_t ES8311_DAC_REG31 = 0x31;
constexpr uint8_t ES8311_DAC_REG32 = 0x32;
constexpr uint8_t ES8311_DAC_REG37 = 0x37;
constexpr uint8_t ES8311_CHD1_REGFD = 0xfd;
constexpr uint8_t ES8311_CHD2_REGFE = 0xfe;
constexpr uint8_t ES8311_CHVER_REGFF = 0xff;

void set_snapshot(Status status, int position_seconds, const char *message)
{
    portENTER_CRITICAL(&s_status_mux);
    s_snapshot.status = status;
    s_snapshot.position_seconds = position_seconds;
    std::snprintf(s_snapshot.message, sizeof(s_snapshot.message), "%s", message ? message : "-");
    portEXIT_CRITICAL(&s_status_mux);
}

void log_audio_heap(const char *stage, const char *format_name)
{
    ESP_LOGI(TAG,
             "AUDIO_HEAP stage=%s format=%s free8=%u largest8=%u free32=%u largest32=%u stack_free=%u",
             stage,
             format_name ? format_name : "-",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_32BIT)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_32BIT)),
             static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t)));
}

void process_samples(void *pcm, size_t bytes, int channels, int volume, const lofi::LofiProfile &profile, lofi::LofiDspState &state)
{
    if (bytes == 0) {
        return;
    }
    if (volume <= 0) {
        std::memset(pcm, 0, bytes);
        return;
    }
    lofi::process_lofi_pcm16(reinterpret_cast<int16_t *>(pcm), bytes / sizeof(int16_t), channels, volume, profile, state);
}

esp_err_t es8311_write(uint8_t reg, uint8_t value)
{
    return lofi_board::write_i2c_reg(I2C_ADDR_ES8311, reg, value);
}

void es8311_write_best_effort(uint8_t reg, uint8_t value, const char *label)
{
    esp_err_t err = es8311_write(reg, value);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ES8311 optional write %s reg=0x%02x failed: %s", label, reg, esp_err_to_name(err));
    }
}

esp_err_t es8311_read(uint8_t reg, uint8_t &value)
{
    return lofi_board::read_i2c_reg(I2C_ADDR_ES8311, reg, &value, 1);
}

uint8_t es8311_volume_reg(int volume)
{
    volume = std::max(0, std::min(100, volume));
    if (volume == 0) {
        return 0;
    }
    return static_cast<uint8_t>((volume * 256 / 100) - 1);
}

esp_err_t apply_es8311_volume(int volume)
{
    s_requested_volume = std::max(0, std::min(100, volume));
    if (!s_codec_ready) {
        return ESP_OK;
    }

    const int reg = es8311_volume_reg(s_requested_volume);
    if (reg == s_codec_volume_reg) {
        return ESP_OK;
    }

    esp_err_t err = es8311_write(ES8311_DAC_REG32, static_cast<uint8_t>(reg));
    if (err == ESP_OK) {
        s_codec_volume_reg = reg;
        ESP_LOGI(TAG, "AUDIO_VOLUME app=%d dac=0x%02x", s_requested_volume, reg);
    } else {
        ESP_LOGW(TAG, "AUDIO_VOLUME failed app=%d dac=0x%02x err=%s",
                 s_requested_volume,
                 reg,
                 esp_err_to_name(err));
    }
    return err;
}

esp_err_t apply_es8311_mute(bool muted)
{
    if (!s_codec_ready) {
        s_codec_muted = muted;
        return ESP_OK;
    }
    if (muted == s_codec_muted) {
        return ESP_OK;
    }

    uint8_t reg = 0;
    esp_err_t err = es8311_read(ES8311_DAC_REG31, reg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "AUDIO_MUTE read failed muted=%d err=%s", muted ? 1 : 0, esp_err_to_name(err));
        return err;
    }
    if (muted) {
        reg |= 0x60;
    } else {
        reg &= static_cast<uint8_t>(~0x60);
    }
    err = es8311_write(ES8311_DAC_REG31, reg);
    if (err == ESP_OK) {
        s_codec_muted = muted;
        ESP_LOGI(TAG, "AUDIO_MUTE muted=%d", muted ? 1 : 0);
    } else {
        ESP_LOGW(TAG, "AUDIO_MUTE write failed muted=%d err=%s", muted ? 1 : 0, esp_err_to_name(err));
    }
    return err;
}

void set_i2s_paused(i2s_chan_handle_t tx, bool paused)
{
    if (!tx) {
        return;
    }
    const esp_err_t err = paused ? i2s_channel_disable(tx) : i2s_channel_enable(tx);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "i2s %s failed: %s", paused ? "pause" : "resume", esp_err_to_name(err));
    }
}

const Es8311ClockCoeff *find_es8311_coeff(uint32_t sample_rate)
{
    const uint32_t bclk_hz = sample_rate * 16 * 2;
    for (const auto &coeff : kEs8311BclkCoeffs) {
        if (coeff.rate == sample_rate && coeff.mclk == bclk_hz) {
            return &coeff;
        }
    }
    return nullptr;
}

esp_err_t configure_es8311_clock(const Es8311ClockCoeff &coeff)
{
    ESP_RETURN_ON_ERROR(es8311_write(ES8311_CLK_MANAGER_REG01, 0xbf), TAG, "es8311 clock source");

    uint8_t reg = 0;
    ESP_RETURN_ON_ERROR(es8311_read(ES8311_CLK_MANAGER_REG02, reg), TAG, "es8311 reg02 read");
    reg &= 0x07;
    reg |= static_cast<uint8_t>((coeff.pre_div - 1) << 5);
    reg |= static_cast<uint8_t>(coeff.pre_multi << 3);
    ESP_RETURN_ON_ERROR(es8311_write(ES8311_CLK_MANAGER_REG02, reg), TAG, "es8311 reg02");
    ESP_RETURN_ON_ERROR(es8311_write(ES8311_CLK_MANAGER_REG03, static_cast<uint8_t>((coeff.fs_mode << 6) | coeff.adc_osr)), TAG, "es8311 reg03");
    ESP_RETURN_ON_ERROR(es8311_write(ES8311_CLK_MANAGER_REG04, coeff.dac_osr), TAG, "es8311 reg04");
    ESP_RETURN_ON_ERROR(es8311_write(ES8311_CLK_MANAGER_REG05, static_cast<uint8_t>(((coeff.adc_div - 1) << 4) | (coeff.dac_div - 1))), TAG, "es8311 reg05");

    ESP_RETURN_ON_ERROR(es8311_read(ES8311_CLK_MANAGER_REG06, reg), TAG, "es8311 reg06 read");
    reg &= 0xe0;
    reg |= coeff.bclk_div < 19 ? static_cast<uint8_t>(coeff.bclk_div - 1) : coeff.bclk_div;
    ESP_RETURN_ON_ERROR(es8311_write(ES8311_CLK_MANAGER_REG06, reg), TAG, "es8311 reg06");

    ESP_RETURN_ON_ERROR(es8311_read(ES8311_CLK_MANAGER_REG07, reg), TAG, "es8311 reg07 read");
    reg &= 0xc0;
    reg |= coeff.lrck_h;
    ESP_RETURN_ON_ERROR(es8311_write(ES8311_CLK_MANAGER_REG07, reg), TAG, "es8311 reg07");
    ESP_RETURN_ON_ERROR(es8311_write(ES8311_CLK_MANAGER_REG08, coeff.lrck_l), TAG, "es8311 reg08");
    return ESP_OK;
}

esp_err_t ensure_es8311_codec(uint32_t sample_rate)
{
    if (s_codec_ready && s_codec_sample_rate == static_cast<int>(sample_rate)) {
        return ESP_OK;
    }

    const Es8311ClockCoeff *coeff = find_es8311_coeff(sample_rate);
    if (!coeff) {
        ESP_LOGW(TAG, "ES8311 no BCLK clock coeff for %lu Hz", static_cast<unsigned long>(sample_rate));
        return ESP_ERR_NOT_SUPPORTED;
    }

    ESP_RETURN_ON_ERROR(lofi_board::probe_i2c_device(I2C_ADDR_ES8311), TAG, "es8311 probe");
    uint8_t id1 = 0;
    uint8_t id2 = 0;
    uint8_t ver = 0;
    es8311_read(ES8311_CHD1_REGFD, id1);
    es8311_read(ES8311_CHD2_REGFE, id2);
    es8311_read(ES8311_CHVER_REGFF, ver);

    ESP_RETURN_ON_ERROR(es8311_write(ES8311_RESET_REG00, 0x1f), TAG, "es8311 reset");
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_RETURN_ON_ERROR(es8311_write(ES8311_RESET_REG00, 0x00), TAG, "es8311 reset release");
    ESP_RETURN_ON_ERROR(es8311_write(ES8311_RESET_REG00, 0x80), TAG, "es8311 power on");
    ESP_RETURN_ON_ERROR(configure_es8311_clock(*coeff), TAG, "es8311 clock");

    uint8_t reg00 = 0;
    ESP_RETURN_ON_ERROR(es8311_read(ES8311_RESET_REG00, reg00), TAG, "es8311 fmt read");
    reg00 &= 0xbf;
    ESP_RETURN_ON_ERROR(es8311_write(ES8311_RESET_REG00, reg00), TAG, "es8311 slave fmt");
    ESP_RETURN_ON_ERROR(es8311_write(ES8311_SDPIN_REG09, 0x0c), TAG, "es8311 sdp in");
    ESP_RETURN_ON_ERROR(es8311_write(ES8311_SDPOUT_REG0A, 0x0c), TAG, "es8311 sdp out");
    ESP_RETURN_ON_ERROR(es8311_write(ES8311_SYSTEM_REG0D, 0x01), TAG, "es8311 analog power");
    ESP_RETURN_ON_ERROR(es8311_write(ES8311_SYSTEM_REG0E, 0x02), TAG, "es8311 adc power");
    ESP_RETURN_ON_ERROR(es8311_write(ES8311_SYSTEM_REG12, 0x00), TAG, "es8311 dac power");
    ESP_RETURN_ON_ERROR(es8311_write(ES8311_SYSTEM_REG13, 0x10), TAG, "es8311 hp drive");
    ESP_RETURN_ON_ERROR(es8311_write(ES8311_ADC_REG17, 0xc8), TAG, "es8311 adc gain");
    ESP_RETURN_ON_ERROR(es8311_write(ES8311_SYSTEM_REG14, 0x1a), TAG, "es8311 analog mic");
    es8311_write_best_effort(ES8311_ADC_REG1C, 0x6a, "adc_bypass");
    es8311_write_best_effort(ES8311_DAC_REG37, 0x08, "dac_bypass");

    uint8_t mute = 0;
    ESP_RETURN_ON_ERROR(es8311_read(ES8311_DAC_REG31, mute), TAG, "es8311 mute read");
    mute &= static_cast<uint8_t>(~(0x40 | 0x20));
    ESP_RETURN_ON_ERROR(es8311_write(ES8311_DAC_REG31, mute), TAG, "es8311 unmute");

    s_codec_ready = true;
    s_codec_sample_rate = static_cast<int>(sample_rate);
    s_codec_volume_reg = -1;
    s_codec_muted = false;
    ESP_RETURN_ON_ERROR(apply_es8311_volume(s_requested_volume), TAG, "es8311 volume");
    ESP_RETURN_ON_ERROR(apply_es8311_mute(false), TAG, "es8311 unmute");
    ESP_LOGI(TAG, "ES8311_INIT rate=%lu bclk=%lu id=%02x%02x ver=%02x",
             static_cast<unsigned long>(sample_rate),
             static_cast<unsigned long>(coeff->mclk),
             id1,
             id2,
             ver);
    return ESP_OK;
}

void close_i2s(i2s_chan_handle_t handle);

const char *i2s_clock_name(i2s_clock_src_t source)
{
    switch (source) {
#if SOC_I2S_SUPPORTS_APLL
    case I2S_CLK_SRC_APLL:
        return "apll";
#endif
    case I2S_CLK_SRC_XTAL:
        return "xtal";
    case I2S_CLK_SRC_DEFAULT:
        return "default";
    default:
        return "unknown";
    }
}

esp_err_t init_i2s_with_clock(i2s_chan_config_t &chan_cfg,
                              i2s_std_config_t &std_cfg,
                              i2s_clock_src_t source,
                              i2s_chan_handle_t *out)
{
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, out, nullptr), TAG, "i2s channel");
    std_cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(std_cfg.clk_cfg.sample_rate_hz);
    std_cfg.clk_cfg.clk_src = source;
    esp_err_t err = i2s_channel_init_std_mode(*out, &std_cfg);
    if (err != ESP_OK) {
        i2s_del_channel(*out);
        *out = nullptr;
    }
    return err;
}

esp_err_t open_i2s(uint32_t sample_rate, uint16_t channels, i2s_chan_handle_t *out)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 12;
    chan_cfg.dma_frame_num = 512;

    i2s_std_config_t std_cfg = {};
    std_cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
    std_cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                           channels == 1 ? I2S_SLOT_MODE_MONO : I2S_SLOT_MODE_STEREO);
    std_cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    std_cfg.gpio_cfg.bclk = static_cast<gpio_num_t>(PIN_I2S_BCLK);
    std_cfg.gpio_cfg.ws = static_cast<gpio_num_t>(PIN_I2S_WS);
    std_cfg.gpio_cfg.dout = static_cast<gpio_num_t>(PIN_I2S_DOUT);
    std_cfg.gpio_cfg.din = I2S_GPIO_UNUSED;
    ESP_LOGI(TAG, "I2S_PINS bclk=%d ws=%d dout=%d din=%d rate=%lu ch=%u",
             PIN_I2S_BCLK,
             PIN_I2S_WS,
             PIN_I2S_DOUT,
             -1,
             static_cast<unsigned long>(sample_rate),
             static_cast<unsigned>(channels));

    const i2s_clock_src_t clock_sources[] = {
#if SOC_I2S_SUPPORTS_APLL
        I2S_CLK_SRC_APLL,
#endif
        I2S_CLK_SRC_XTAL,
        I2S_CLK_SRC_DEFAULT,
    };

    esp_err_t err = ESP_FAIL;
    const char *selected_clock = "none";
    for (i2s_clock_src_t source : clock_sources) {
        err = init_i2s_with_clock(chan_cfg, std_cfg, source, out);
        if (err == ESP_OK) {
            selected_clock = i2s_clock_name(source);
            ESP_LOGI(TAG,
                     "I2S_CLOCK src=%s rate=%lu ch=%u dma_desc=%lu dma_frame=%lu",
                     selected_clock,
                     static_cast<unsigned long>(sample_rate),
                     static_cast<unsigned>(channels),
                     static_cast<unsigned long>(chan_cfg.dma_desc_num),
                     static_cast<unsigned long>(chan_cfg.dma_frame_num));
            break;
        }
        ESP_LOGW(TAG,
                 "I2S %s init failed for %lu Hz, trying fallback: %s",
                 i2s_clock_name(source),
                 static_cast<unsigned long>(sample_rate),
                 esp_err_to_name(err));
    }
    if (err != ESP_OK) {
        return err;
    }
    err = i2s_channel_enable(*out);
    if (err != ESP_OK) {
        i2s_del_channel(*out);
        *out = nullptr;
    }
    if (err == ESP_OK) {
        esp_err_t codec_err = ensure_es8311_codec(sample_rate);
        if (codec_err != ESP_OK) {
            close_i2s(*out);
            *out = nullptr;
            return codec_err;
        }
    }
    return err;
}

void close_i2s(i2s_chan_handle_t handle)
{
    if (handle) {
        i2s_channel_disable(handle);
        i2s_del_channel(handle);
    }
}

bool receive_immediate(Command &cmd)
{
    return s_queue && xQueueReceive(s_queue, &cmd, 0) == pdTRUE;
}

bool seek_wav_file(FILE *file, const lofi::WavInfo &info, int seconds, uint32_t &played_bytes, uint32_t &remaining)
{
    const uint32_t bytes_per_second = info.sample_rate * info.channels * (info.bits_per_sample / 8);
    const uint32_t block_align = std::max<uint32_t>(1, info.channels * (info.bits_per_sample / 8));
    uint64_t offset = static_cast<uint64_t>(std::max(0, seconds)) * bytes_per_second;
    if (offset > info.data_size) {
        offset = info.data_size;
    }
    offset -= offset % block_align;
    if (std::fseek(file, static_cast<long>(info.data_offset + offset), SEEK_SET) != 0) {
        return false;
    }
    played_bytes = static_cast<uint32_t>(offset);
    remaining = info.data_size - played_bytes;
    return true;
}

int wav_position_seconds(const lofi::WavInfo &info, uint32_t played_bytes)
{
    const uint32_t bytes_per_second = info.sample_rate * info.channels * (info.bits_per_sample / 8);
    if (bytes_per_second == 0) {
        return 0;
    }
    return static_cast<int>(played_bytes / bytes_per_second);
}

void play_wav(Command current)
{
    FILE *file = std::fopen(current.path, "rb");
    if (!file) {
        ESP_LOGW(TAG, "open failed: %s", current.path);
        set_snapshot(Status::Error, 0, "open failed");
        return;
    }

    lofi::WavInfo info;
    lofi::WavParseResult parse_result = lofi::parse_wav_file(file, info);
    if (parse_result != lofi::WavParseResult::Ok) {
        ESP_LOGW(TAG, "wav parse failed: %s (%s)", current.path, lofi::to_string(parse_result));
        std::fclose(file);
        set_snapshot(Status::Unsupported, 0, lofi::to_string(parse_result));
        return;
    }
    ESP_LOGI(TAG, "WAV_STREAM hz=%u ch=%u bits=%u bytes=%u path=%s",
             static_cast<unsigned>(info.sample_rate),
             static_cast<unsigned>(info.channels),
             static_cast<unsigned>(info.bits_per_sample),
             static_cast<unsigned>(info.data_size),
             current.path);

    esp_err_t codec_probe = lofi_board::probe_i2c_device(I2C_ADDR_ES8311);
    if (codec_probe != ESP_OK) {
        ESP_LOGW(TAG, "ES8311 probe failed: %s", esp_err_to_name(codec_probe));
    }

    i2s_chan_handle_t tx = nullptr;
    esp_err_t err = open_i2s(info.sample_rate, info.channels, &tx);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "i2s open failed: %s", esp_err_to_name(err));
        std::fclose(file);
        set_snapshot(Status::Error, 0, "i2s open failed");
        return;
    }

    uint8_t bytes[1024];
    uint32_t played_bytes = 0;
    uint32_t remaining = info.data_size;
    if (!seek_wav_file(file, info, current.seconds, played_bytes, remaining)) {
        close_i2s(tx);
        std::fclose(file);
        set_snapshot(Status::Error, 0, "seek failed");
        return;
    }
    set_snapshot(Status::Playing, wav_position_seconds(info, played_bytes), "playing wav");

    bool paused = false;
    int volume = current.volume;
    lofi::LofiDspState dsp_state;
    int last_snapshot_pos = wav_position_seconds(info, played_bytes);
    int64_t last_snapshot_us = esp_timer_get_time();

    while (remaining > 0) {
        Command next;
        while (receive_immediate(next)) {
            if (next.type == CommandType::Stop || next.type == CommandType::Play) {
                if (next.type == CommandType::Play) {
                    xQueueSendToFront(s_queue, &next, 0);
                }
                if (paused) {
                    set_i2s_paused(tx, false);
                }
                close_i2s(tx);
                std::fclose(file);
                set_snapshot(Status::Idle, wav_position_seconds(info, played_bytes), "stopped");
                return;
            }
            if (next.type == CommandType::Pause) {
                paused = next.volume != 0;
                if (paused) {
                    apply_es8311_mute(true);
                    set_i2s_paused(tx, true);
                } else {
                    set_i2s_paused(tx, false);
                    apply_es8311_volume(volume);
                    apply_es8311_mute(false);
                }
                set_snapshot(paused ? Status::Paused : Status::Playing,
                             wav_position_seconds(info, played_bytes),
                             paused ? "paused" : "playing wav");
            } else if (next.type == CommandType::Volume) {
                volume = next.volume;
                if (!paused) {
                    apply_es8311_volume(volume);
                }
            } else if (next.type == CommandType::Profile) {
                current.profile = next.profile;
                dsp_state = lofi::LofiDspState{};
                ESP_LOGI(TAG,
                         "LOFI_DSP_UPDATE preset=%s active=%d intensity=%d warmth=%d noise=%d wobble=%d space=%d softness=%d",
                         lofi::to_string(current.profile.preset),
                         lofi::lofi_dsp_active(current.profile) ? 1 : 0,
                         current.profile.intensity,
                         current.profile.warmth,
                         current.profile.noise,
                         current.profile.wobble,
                         current.profile.space,
                         current.profile.softness);
            } else if (next.type == CommandType::Seek) {
                if (!seek_wav_file(file, info, next.seconds, played_bytes, remaining)) {
                    ESP_LOGW(TAG, "wav seek failed: %s", current.path);
                    set_snapshot(Status::Error, wav_position_seconds(info, played_bytes), "seek failed");
                    close_i2s(tx);
                    std::fclose(file);
                    return;
                }
                set_snapshot(paused ? Status::Paused : Status::Playing,
                             wav_position_seconds(info, played_bytes),
                             paused ? "paused" : "playing wav");
            }
        }

        if (paused) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        const size_t want = std::min<uint32_t>(sizeof(bytes), remaining);
        const size_t got = std::fread(bytes, 1, want, file);
        if (got == 0) {
            break;
        }
        process_samples(bytes, got, info.channels, volume, current.profile, dsp_state);
        size_t written = 0;
        err = i2s_channel_write(tx, bytes, got, &written, pdMS_TO_TICKS(1000));
        if (err != ESP_OK) {
            ESP_LOGW(TAG,
                     "i2s wav write failed: %s bytes=%u written=%u",
                     esp_err_to_name(err),
                     static_cast<unsigned>(got),
                     static_cast<unsigned>(written));
            set_snapshot(Status::Error, wav_position_seconds(info, played_bytes), "i2s write failed");
            break;
        }
        remaining -= static_cast<uint32_t>(got);
        played_bytes += static_cast<uint32_t>(got);
        const int pos = wav_position_seconds(info, played_bytes);
        const int64_t now_us = esp_timer_get_time();
        if (pos != last_snapshot_pos || now_us - last_snapshot_us >= 500000) {
            set_snapshot(Status::Playing, pos, "playing wav");
            last_snapshot_pos = pos;
            last_snapshot_us = now_us;
        }
    }

    close_i2s(tx);
    std::fclose(file);
    set_snapshot(Status::Ended, wav_position_seconds(info, played_bytes), "track ended");
}

void play_simple_file(Command current, esp_audio_simple_dec_type_t dec_type, const char *format_name)
{
    log_audio_heap("play_begin", format_name);
    FILE *file = std::fopen(current.path, "rb");
    if (!file) {
        ESP_LOGW(TAG, "open failed: %s", current.path);
        set_snapshot(Status::Error, 0, "open failed");
        return;
    }

    if (!s_simple_decoders_registered) {
        esp_audio_err_t reg = esp_audio_dec_register_default();
        if (reg == ESP_AUDIO_ERR_OK) {
            reg = esp_audio_simple_dec_register_default();
        }
        if (reg != ESP_AUDIO_ERR_OK) {
            ESP_LOGW(TAG, "simple decoder register failed: %d", reg);
            std::fclose(file);
            set_snapshot(Status::Error, 0, "decoder register failed");
            return;
        }
        s_simple_decoders_registered = true;
    }

    esp_audio_simple_dec_cfg_t dec_cfg = {};
    dec_cfg.dec_type = dec_type;
    esp_m4a_dec_cfg_t m4a_cfg = {};
    esp_aac_dec_cfg_t aac_cfg = ESP_AAC_DEC_CONFIG_DEFAULT();
    if (dec_type == ESP_AUDIO_SIMPLE_DEC_TYPE_M4A) {
        m4a_cfg.aac_plus_enable = false;
        dec_cfg.dec_cfg = &m4a_cfg;
        dec_cfg.cfg_size = sizeof(m4a_cfg);
    } else if (dec_type == ESP_AUDIO_SIMPLE_DEC_TYPE_AAC) {
        aac_cfg.aac_plus_enable = false;
        dec_cfg.dec_cfg = &aac_cfg;
        dec_cfg.cfg_size = sizeof(aac_cfg);
    }
    esp_audio_simple_dec_handle_t decoder = nullptr;
    esp_audio_err_t ret = esp_audio_simple_dec_open(&dec_cfg, &decoder);
    if (ret != ESP_AUDIO_ERR_OK) {
        ESP_LOGW(TAG, "%s decoder open failed: %d", format_name, ret);
        log_audio_heap("simple_open_failed", format_name);
        std::fclose(file);
        set_snapshot(Status::Unsupported, 0, "decoder open failed");
        return;
    }
    log_audio_heap("simple_open_ok", format_name);

    constexpr size_t kInputSize = kSimpleDecoderInputBytes;
    size_t out_capacity = kSimpleDecoderOutputBytes;
    uint8_t *input = static_cast<uint8_t *>(std::malloc(kInputSize));
    uint8_t *output = static_cast<uint8_t *>(std::malloc(out_capacity));
    if (!input || !output) {
        std::free(input);
        std::free(output);
        esp_audio_simple_dec_close(decoder);
        std::fclose(file);
        log_audio_heap("io_alloc_failed", format_name);
        set_snapshot(Status::Error, 0, "decoder no memory");
        return;
    }
    log_audio_heap("io_alloc_ok", format_name);

    esp_err_t codec_probe = lofi_board::probe_i2c_device(I2C_ADDR_ES8311);
    if (codec_probe != ESP_OK) {
        ESP_LOGW(TAG, "ES8311 probe failed: %s", esp_err_to_name(codec_probe));
    }

    i2s_chan_handle_t tx = nullptr;
    int sample_rate = 0;
    int channels = 0;
    int bits = 0;
    uint64_t played_bytes = 0;
    bool eof = false;
    bool paused = false;
    int64_t perf_audio_us = 0;
    int64_t perf_process_us = 0;
    int64_t perf_write_us = 0;
    int64_t perf_window_start_us = 0;
    int perf_log_count = 0;
    bool perf_started = false;
    auto reset_perf_window = [&]() {
        perf_audio_us = 0;
        perf_process_us = 0;
        perf_write_us = 0;
        perf_window_start_us = 0;
        perf_log_count = 0;
        perf_started = false;
    };
    int last_snapshot_pos = -1;
    int64_t last_snapshot_us = 0;
    lofi::LofiDspState dsp_state;
    char decoding_message[32] = {};
    std::snprintf(decoding_message, sizeof(decoding_message), "decoding %s", format_name);
    set_snapshot(Status::Playing, 0, decoding_message);

    while (!eof && ret == ESP_AUDIO_ERR_OK) {
        const size_t got = std::fread(input, 1, kInputSize, file);
        eof = got < kInputSize;
        esp_audio_simple_dec_raw_t raw = {};
        raw.buffer = input;
        raw.len = static_cast<uint32_t>(got);
        raw.eos = eof;

        while ((raw.len > 0 || raw.eos) && ret == ESP_AUDIO_ERR_OK) {
            Command next;
            while (receive_immediate(next)) {
                if (next.type == CommandType::Stop || next.type == CommandType::Play) {
                    if (next.type == CommandType::Play) {
                        xQueueSendToFront(s_queue, &next, 0);
                    }
                    if (paused) {
                        set_i2s_paused(tx, false);
                    }
                    close_i2s(tx);
                    std::free(input);
                    std::free(output);
                    esp_audio_simple_dec_close(decoder);
                    std::fclose(file);
                    set_snapshot(Status::Idle, sample_rate > 0 ? static_cast<int>(played_bytes / (sample_rate * std::max(1, channels) * 2)) : 0, "stopped");
                    return;
                }
                if (next.type == CommandType::Volume) {
                    current.volume = next.volume;
                    if (!paused) {
                        apply_es8311_volume(current.volume);
                    }
                } else if (next.type == CommandType::Pause) {
                    paused = next.volume != 0;
                    const int pos = sample_rate > 0 ? static_cast<int>(played_bytes / (sample_rate * std::max(1, channels) * 2)) : 0;
                    if (paused) {
                        apply_es8311_mute(true);
                        set_i2s_paused(tx, true);
                    } else {
                        set_i2s_paused(tx, false);
                        apply_es8311_volume(current.volume);
                        apply_es8311_mute(false);
                    }
                    set_snapshot(paused ? Status::Paused : Status::Playing,
                                 pos,
                                 paused ? "paused" : format_name);
                    reset_perf_window();
                } else if (next.type == CommandType::Profile) {
                    current.profile = next.profile;
                    dsp_state = lofi::LofiDspState{};
                    ESP_LOGI(TAG,
                             "LOFI_DSP_UPDATE preset=%s active=%d intensity=%d warmth=%d noise=%d wobble=%d space=%d softness=%d",
                             lofi::to_string(current.profile.preset),
                             lofi::lofi_dsp_active(current.profile) ? 1 : 0,
                             current.profile.intensity,
                             current.profile.warmth,
                             current.profile.noise,
                             current.profile.wobble,
                             current.profile.space,
                             current.profile.softness);
                }
            }
            if (paused) {
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }

            esp_audio_simple_dec_out_t out_frame = {};
            out_frame.buffer = output;
            out_frame.len = static_cast<uint32_t>(out_capacity);
            const int64_t process_start_us = esp_timer_get_time();
            ret = esp_audio_simple_dec_process(decoder, &raw, &out_frame);
            perf_process_us += esp_timer_get_time() - process_start_us;
            if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                uint8_t *new_output = static_cast<uint8_t *>(std::realloc(output, out_frame.needed_size));
                if (!new_output) {
                    ESP_LOGW(TAG, "%s output realloc failed size=%u", format_name, static_cast<unsigned>(out_frame.needed_size));
                    set_snapshot(Status::Error, 0, "decoder no memory");
                    ret = ESP_AUDIO_ERR_MEM_LACK;
                    break;
                }
                output = new_output;
                out_capacity = out_frame.needed_size;
                ret = ESP_AUDIO_ERR_OK;
                continue;
            }
            if (ret != ESP_AUDIO_ERR_OK) {
                ESP_LOGW(TAG, "%s decode failed: %d", format_name, ret);
                log_audio_heap("decode_failed", format_name);
                break;
            }
            if (raw.consumed > raw.len) {
                raw.len = 0;
            } else {
                raw.buffer += raw.consumed;
                raw.len -= raw.consumed;
            }
            if (out_frame.decoded_size == 0) {
                if (raw.consumed == 0) {
                    ESP_LOGW(TAG, "%s decode made no progress: eos=%d len=%u", format_name, raw.eos ? 1 : 0,
                             static_cast<unsigned>(raw.len));
                    ret = raw.eos ? ESP_AUDIO_ERR_DATA_LACK : ESP_AUDIO_ERR_NOT_SUPPORT;
                    break;
                }
                continue;
            }

            esp_audio_simple_dec_info_t info = {};
            if (esp_audio_simple_dec_get_info(decoder, &info) != ESP_AUDIO_ERR_OK) {
                continue;
            }
            if (info.bits_per_sample != 16 || (info.channel != 1 && info.channel != 2)) {
                ESP_LOGW(TAG, "%s unsupported PCM info: %u Hz %u ch %u bit",
                         format_name,
                         static_cast<unsigned>(info.sample_rate),
                         static_cast<unsigned>(info.channel),
                         static_cast<unsigned>(info.bits_per_sample));
                set_snapshot(Status::Unsupported, 0, "pcm format unsupported");
                ret = ESP_AUDIO_ERR_NOT_SUPPORT;
                break;
            }
            if (!tx || sample_rate != static_cast<int>(info.sample_rate) ||
                channels != static_cast<int>(info.channel) || bits != static_cast<int>(info.bits_per_sample)) {
                close_i2s(tx);
                tx = nullptr;
                sample_rate = static_cast<int>(info.sample_rate);
                channels = static_cast<int>(info.channel);
                bits = static_cast<int>(info.bits_per_sample);
                esp_err_t err = open_i2s(static_cast<uint32_t>(sample_rate), static_cast<uint16_t>(channels), &tx);
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "i2s open failed for %s %d Hz %d ch: %s",
                             format_name, sample_rate, channels, esp_err_to_name(err));
                    set_snapshot(Status::Error, 0, "i2s open failed");
                    ret = ESP_AUDIO_ERR_FAIL;
                    break;
                }
                ESP_LOGI(TAG, "%s_STREAM hz=%d ch=%d bits=%d bitrate=%u path=%s",
                         format_name,
                         sample_rate,
                         channels,
                         bits,
                         static_cast<unsigned>(info.bitrate),
                         current.path);
            }

            if (!perf_started) {
                perf_audio_us = 0;
                perf_process_us = 0;
                perf_write_us = 0;
                perf_window_start_us = esp_timer_get_time();
                perf_started = true;
            }

            process_samples(output, out_frame.decoded_size, channels, current.volume, current.profile, dsp_state);
            size_t written = 0;
            const int64_t write_start_us = esp_timer_get_time();
            esp_err_t err = i2s_channel_write(tx, output, out_frame.decoded_size, &written, pdMS_TO_TICKS(1000));
            perf_write_us += esp_timer_get_time() - write_start_us;
            if (err != ESP_OK) {
                ESP_LOGW(TAG,
                         "i2s %s write failed: %s decoded=%u written=%u",
                         format_name,
                         esp_err_to_name(err),
                         static_cast<unsigned>(out_frame.decoded_size),
                         static_cast<unsigned>(written));
                set_snapshot(Status::Error, 0, "i2s write failed");
                ret = ESP_AUDIO_ERR_FAIL;
                break;
            }
            played_bytes += written;
            if (sample_rate > 0 && channels > 0) {
                perf_audio_us += static_cast<int64_t>(written) * 1000000LL /
                                 (static_cast<int64_t>(sample_rate) * channels * 2);
            }
            const int pos = sample_rate > 0 ? static_cast<int>(played_bytes / (sample_rate * std::max(1, channels) * 2)) : 0;
            const int64_t now_us = esp_timer_get_time();
            if (pos != last_snapshot_pos || now_us - last_snapshot_us >= 500000) {
                set_snapshot(Status::Playing, pos, format_name);
                last_snapshot_pos = pos;
                last_snapshot_us = now_us;
            }
            if (perf_started && now_us - perf_window_start_us >= 5000000) {
                const double ratio = perf_audio_us > 0 ? static_cast<double>(now_us - perf_window_start_us) /
                                                             static_cast<double>(perf_audio_us)
                                                       : 0.0;
                if (perf_log_count < 3) {
                    ESP_LOGI(TAG,
                             "%s_PERF audio_ms=%lld wall_ms=%lld decode_ms=%lld write_ms=%lld ratio=%.2f",
                             format_name,
                             static_cast<long long>(perf_audio_us / 1000),
                             static_cast<long long>((now_us - perf_window_start_us) / 1000),
                             static_cast<long long>(perf_process_us / 1000),
                             static_cast<long long>(perf_write_us / 1000),
                             ratio);
                } else {
                    ESP_LOGD(TAG,
                             "%s_PERF audio_ms=%lld wall_ms=%lld decode_ms=%lld write_ms=%lld ratio=%.2f",
                             format_name,
                             static_cast<long long>(perf_audio_us / 1000),
                             static_cast<long long>((now_us - perf_window_start_us) / 1000),
                             static_cast<long long>(perf_process_us / 1000),
                             static_cast<long long>(perf_write_us / 1000),
                             ratio);
                }
                ++perf_log_count;
                perf_audio_us = 0;
                perf_process_us = 0;
                perf_write_us = 0;
                perf_window_start_us = now_us;
            }
        }
    }

    close_i2s(tx);
    std::free(input);
    std::free(output);
    esp_audio_simple_dec_close(decoder);
    std::fclose(file);
    log_audio_heap("play_cleanup", format_name);
    if (played_bytes == 0) {
        set_snapshot(Status::Unsupported, 0, "decode failed");
    } else if (ret == ESP_AUDIO_ERR_OK || ret == ESP_AUDIO_ERR_DATA_LACK) {
        const int pos = sample_rate > 0 ? static_cast<int>(played_bytes / (sample_rate * std::max(1, channels) * 2)) : 0;
        set_snapshot(Status::Ended, pos, "track ended");
    } else if (ret == ESP_AUDIO_ERR_NOT_SUPPORT) {
        const int pos = sample_rate > 0 ? static_cast<int>(played_bytes / (sample_rate * std::max(1, channels) * 2)) : 0;
        set_snapshot(Status::Unsupported, pos, "decode failed");
    } else {
        const int pos = sample_rate > 0 ? static_cast<int>(played_bytes / (sample_rate * std::max(1, channels) * 2)) : 0;
        set_snapshot(Status::Error, pos, "decode error");
    }
}

void audio_task(void *)
{
    Command cmd;
    while (true) {
        if (xQueueReceive(s_queue, &cmd, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (cmd.type == CommandType::Play) {
            const char *ext = std::strrchr(cmd.path, '.');
            if (ext && strcasecmp(ext, ".m4a") == 0) {
                play_simple_file(cmd, ESP_AUDIO_SIMPLE_DEC_TYPE_M4A, "M4A");
            } else if (ext && strcasecmp(ext, ".aac") == 0) {
                play_simple_file(cmd, ESP_AUDIO_SIMPLE_DEC_TYPE_AAC, "AAC");
            } else if (ext && strcasecmp(ext, ".mp3") == 0) {
                play_simple_file(cmd, ESP_AUDIO_SIMPLE_DEC_TYPE_MP3, "MP3");
            } else {
                play_wav(cmd);
            }
        } else if (cmd.type == CommandType::Stop) {
            set_snapshot(Status::Idle, 0, "stopped");
        } else if (cmd.type == CommandType::Pause) {
            set_snapshot(cmd.volume != 0 ? Status::Paused : Status::Idle, s_snapshot.position_seconds,
                         cmd.volume != 0 ? "paused" : "idle");
        }
    }
}

} // namespace

esp_err_t init(void)
{
    if (!s_queue) {
        s_queue = xQueueCreate(4, sizeof(Command));
        if (!s_queue) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (!s_task) {
        if (xTaskCreatePinnedToCore(audio_task, "lofi_audio", kAudioTaskStackBytes, nullptr, kAudioTaskPriority, &s_task, 1) != pdPASS) {
            return ESP_ERR_NO_MEM;
        }
    }
    set_snapshot(Status::Idle, 0, "ready");
    return ESP_OK;
}

esp_err_t play_track(const lofi::Track &track, int volume, const lofi::LofiProfile &profile, int start_seconds)
{
    if (track.format != "wav" && track.format != "mp3" && track.format != "m4a" && track.format != "aac") {
        set_snapshot(Status::Unsupported, 0, "unsupported format");
        return ESP_ERR_NOT_SUPPORTED;
    }

    apply_es8311_volume(volume);
    apply_es8311_mute(false);
    ESP_LOGI(TAG, "AUDIO_PLAY format=%s path=%s", track.format.c_str(), track.path.c_str());
    ESP_LOGI(TAG,
             "LOFI_DSP preset=%s active=%d intensity=%d warmth=%d noise=%d wobble=%d space=%d softness=%d",
             lofi::to_string(profile.preset),
             lofi::lofi_dsp_active(profile) ? 1 : 0,
             profile.intensity,
             profile.warmth,
             profile.noise,
             profile.wobble,
             profile.space,
             profile.softness);
    Command cmd;
    cmd.type = CommandType::Play;
    std::snprintf(cmd.path, sizeof(cmd.path), "%s", track.path.c_str());
    cmd.volume = volume;
    cmd.seconds = std::max(0, start_seconds);
    cmd.profile = profile;
    if (xQueueSend(s_queue, &cmd, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    set_snapshot(Status::Playing, cmd.seconds, "opening");
    return ESP_OK;
}

void set_paused(bool paused)
{
    Command cmd;
    cmd.type = CommandType::Pause;
    cmd.volume = paused ? 1 : 0;
    if (s_queue) {
        xQueueSend(s_queue, &cmd, 0);
    }
}

void stop(void)
{
    Command cmd;
    cmd.type = CommandType::Stop;
    if (s_queue) {
        xQueueSend(s_queue, &cmd, 0);
    }
}

void set_volume(int volume)
{
    volume = std::max(0, std::min(100, volume));
    if (volume == s_requested_volume) {
        return;
    }
    apply_es8311_volume(volume);
    Command cmd;
    cmd.type = CommandType::Volume;
    cmd.volume = volume;
    if (s_queue) {
        xQueueSend(s_queue, &cmd, 0);
    }
}

void set_profile(const lofi::LofiProfile &profile)
{
    Command cmd;
    cmd.type = CommandType::Profile;
    cmd.profile = profile;
    if (s_queue) {
        xQueueSend(s_queue, &cmd, 0);
    }
}

void seek(int seconds)
{
    Command cmd;
    cmd.type = CommandType::Seek;
    cmd.seconds = std::max(0, seconds);
    if (s_queue) {
        xQueueSend(s_queue, &cmd, 0);
    }
}

Snapshot snapshot(void)
{
    Snapshot copy;
    portENTER_CRITICAL(&s_status_mux);
    copy = s_snapshot;
    portEXIT_CRITICAL(&s_status_mux);
    return copy;
}

} // namespace lofi_audio
