/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal.h"
#include "board/hal_bridge.h"
#include <mooncake_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/idf_additions.h>
#include <esp_heap_caps.h>
#include <driver/i2c_master.h>
#include <atomic>

static const std::string_view _tag = "HAL-AmbientLight";

namespace {

// How often the sensor is polled.
constexpr uint32_t kSampleIntervalMs = 1000;

/* ------------------------------- LTR-553ALS ------------------------------- */

// The CoreS3 carries an LTR-553ALS-WA ambient light and proximity sensor on the
// same ribbon cable as the camera. It shares the main I2C bus and answers at
// 0x23, next door to the camera at 0x21. Only the ambient half is used here;
// the proximity half is left in standby so its infrared LED never pulses.
constexpr uint8_t kLtrI2cAddress = 0x23;

constexpr uint8_t kRegAlsContr    = 0x80;
constexpr uint8_t kRegAlsMeasRate = 0x85;
constexpr uint8_t kRegPartId      = 0x86;
constexpr uint8_t kRegManufacId   = 0x87;
constexpr uint8_t kRegAlsData     = 0x88;  // CH1 low/high, then CH0 low/high

// Identity registers, read to prove we are talking to the part we think we are
// rather than to whatever else might answer at this address.
constexpr int kExpectedPartId    = 0x92;
constexpr int kExpectedManufacId = 0x05;

// ALS active at 8x gain. 8x spans roughly 0.125 to 8k lux: it keeps useful
// resolution down in the range that separates a dark room from a dim one, and
// only saturates in direct sunlight, where "is it dark?" is not a hard question.
constexpr uint8_t kAlsContrActive = 0x0d;

// 100 ms integration, 500 ms between conversions. Polled once a second, so
// every read returns a fresh result without having to watch the data-ready bit.
constexpr uint8_t kAlsMeasRateCfg = 0x03;

// The part needs to settle after leaving standby before the first conversion.
constexpr uint32_t kSensorWakeupMs = 100;

/* -------------------------------- Behaviour ------------------------------- */

// Visible-light counts (CH0) at or below which the room counts as dark.
//
// Raw counts rather than lux on purpose: the datasheet's lux conversion picks
// between three sets of coefficients by channel ratio and returns zero outright
// when infrared dominates, so a lit room can convert to 0 lux. Counts are
// strictly monotonic in light, which is all a dark/not-dark decision needs.
//
// PROVISIONAL: both thresholds still need measuring on hardware.
constexpr int kDarkLevelThreshold = 8;

// Counts needed to call the room lit again. The gap up from the dark threshold
// is the hysteresis that stops a reading sitting on the boundary from flapping
// the backlight: readings between the two thresholds leave the screen as it is.
constexpr int kBrightLevelThreshold = 20;

// Darkness has to persist before the screen goes off, so a passing shadow is
// ignored.
constexpr uint32_t kDarkSamplesToSleep = 4;

// Light has to come back for a moment too, otherwise noise near the threshold
// would wake the screen repeatedly.
constexpr uint32_t kBrightSamplesToWake = 2;

// Samples between diagnostic log lines, so the thresholds above can be checked
// against real readings over the serial monitor.
constexpr uint32_t kLogEverySamples = 5;

// Used if the backlight somehow reads as already off when the screen is put to
// sleep, so waking up can never leave it stuck black.
constexpr uint8_t kFallbackBrightness = 60;

std::atomic<int> _ambient_level{-1};
std::atomic<bool> _screen_off{false};
std::atomic<bool> _dark_enabled{true};
std::atomic<uint8_t> _saved_brightness{kFallbackBrightness};

// Set when the user taps a dark screen awake, and cleared again only once the
// room reads properly lit. A tap is an explicit "I want to see this", so while
// it is set the dark rule is suppressed entirely; dim rooms measure close
// enough to a dark one that otherwise the screen would blank again seconds
// after every manual wake.
std::atomic<bool> _manual_on{false};

i2c_master_dev_handle_t _sensor_dev = nullptr;

bool _sensor_open()
{
    if (_sensor_dev != nullptr) {
        return true;
    }

    auto bus = hal_bridge::board_get_i2c_bus();
    if (bus == nullptr) {
        return false;
    }

    i2c_device_config_t cfg = {};
    cfg.dev_addr_length     = I2C_ADDR_BIT_LEN_7;
    cfg.device_address      = kLtrI2cAddress;
    cfg.scl_speed_hz        = 100000;

    return i2c_master_bus_add_device(bus, &cfg, &_sensor_dev) == ESP_OK;
}

// Returns the register value, or -1 if the sensor could not be reached.
int _sensor_read(uint8_t reg)
{
    if (!_sensor_open()) {
        return -1;
    }

    uint8_t value = 0;
    if (i2c_master_transmit_receive(_sensor_dev, &reg, 1, &value, 1, 100) != ESP_OK) {
        return -1;
    }
    return value;
}

// Reads consecutive registers in a single transaction. The data registers latch
// on the first read of a block, so reading them together is what guarantees the
// low and high bytes come from the same conversion.
bool _sensor_read_block(uint8_t reg, uint8_t* out, size_t len)
{
    if (!_sensor_open()) {
        return false;
    }
    return i2c_master_transmit_receive(_sensor_dev, &reg, 1, out, len, 100) == ESP_OK;
}

bool _sensor_write(uint8_t reg, uint8_t value)
{
    if (!_sensor_open()) {
        return false;
    }

    const uint8_t payload[2] = {reg, value};
    return i2c_master_transmit(_sensor_dev, payload, sizeof(payload), 100) == ESP_OK;
}

// Takes the ambient half of the sensor in or out of standby. The proximity half
// is powered independently and is never enabled, so its infrared LED costs
// nothing.
void _sensor_apply_power(bool als_on)
{
    _sensor_write(kRegAlsContr, als_on ? kAlsContrActive : 0x00);
    if (als_on) {
        vTaskDelay(pdMS_TO_TICKS(kSensorWakeupMs));
    }
}

// Returns false if nothing recognisable is on the bus, in which case the whole
// feature stays dormant rather than acting on invented readings.
bool _sensor_init()
{
    const int part_id    = _sensor_read(kRegPartId);
    const int manufac_id = _sensor_read(kRegManufacId);
    if (part_id != kExpectedPartId || manufac_id != kExpectedManufacId) {
        mclog::tagWarn(_tag, "no LTR-553 at {:#x} (part={:#x}, manufac={:#x})", kLtrI2cAddress, part_id, manufac_id);
        return false;
    }

    _sensor_write(kRegAlsMeasRate, kAlsMeasRateCfg);

    mclog::tagInfo(_tag, "LTR-553 ready (part={:#x}, manufac={:#x})", part_id, manufac_id);
    return true;
}

// Visible-light channel (CH0) counts, or -1 if the read failed.
int _read_ambient_level()
{
    uint8_t data[4] = {};
    if (!_sensor_read_block(kRegAlsData, data, sizeof(data))) {
        return -1;
    }
    // data[0..1] is CH1 (infrared), data[2..3] is CH0 (visible plus infrared).
    // CH1 has to be read first, which reading the block in order takes care of.
    return static_cast<int>(data[2] | (data[3] << 8));
}

// Turning the backlight off has to remember the level to come back to. Reading
// it back from NVS is not an option here: this task cannot touch flash.
void _screen_sleep()
{
    uint8_t brightness = hal_bridge::board_get_backlight_brightness();
    if (brightness == 0) {
        brightness = kFallbackBrightness;
    }
    _saved_brightness.store(brightness);

    hal_bridge::board_set_backlight_brightness(0);
    _screen_off.store(true);
}

void _screen_wake()
{
    if (!_screen_off.exchange(false)) {
        return;
    }
    hal_bridge::board_set_backlight_brightness(_saved_brightness.load());
}

void _ambient_light_update_task(void* param)
{
    mclog::tagInfo(_tag, "start update task");

    if (!_sensor_init()) {
        mclog::tagWarn(_tag, "automatic screen off unavailable");
        vTaskDelete(NULL);
        return;
    }

    uint32_t dark_samples   = 0;
    uint32_t bright_samples = 0;
    uint32_t sample_count   = 0;
    bool als_on             = false;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(kSampleIntervalMs));

        const bool want_dark = _dark_enabled.load();

        if (want_dark != als_on) {
            // Switching the feature off must never leave the screen dark, since
            // nothing would be left watching for the light to come back.
            if (!want_dark) {
                _screen_wake();
            }

            _sensor_apply_power(want_dark);
            als_on = want_dark;
            _manual_on.store(false);
            dark_samples = bright_samples = 0;

            if (!als_on) {
                _ambient_level.store(-1);
            }
        }

        if (!als_on) {
            continue;
        }

        const int level = _read_ambient_level();
        if (++sample_count % kLogEverySamples == 0) {
            mclog::tagInfo(_tag, "level={}, screen_off={}, manual_on={}", level, _screen_off.load(), _manual_on.load());
        }
        if (level < 0) {
            // Bus hiccup. Leave the screen as it is and try again.
            continue;
        }
        _ambient_level.store(level);

        if (level >= kBrightLevelThreshold) {
            bright_samples++;
            dark_samples = 0;
        } else if (level <= kDarkLevelThreshold) {
            dark_samples++;
            bright_samples = 0;
        } else {
            // Between the thresholds. Neither rule applies, so the screen stays
            // as it is and both runs start over.
            dark_samples = bright_samples = 0;
            continue;
        }

        if (bright_samples >= kBrightSamplesToWake) {
            bright_samples = 0;
            // Proper light also clears a manual override: the tap was only
            // needed for as long as the room was dark.
            _manual_on.store(false);
            if (_screen_off.load()) {
                mclog::tagInfo(_tag, "screen on: level={}", level);
                _screen_wake();
            }
            continue;
        }

        if (dark_samples >= kDarkSamplesToSleep) {
            dark_samples = 0;
            if (!_screen_off.load() && !_manual_on.load()) {
                mclog::tagInfo(_tag, "screen off: level={} (tap to wake)", level);
                _screen_sleep();
            }
        }
    }
}

}  // namespace

void Hal::ambient_light_init()
{
    mclog::tagInfo(_tag, "init");

    const auto config = hal_bridge::get_xiaozhi_config();
    _dark_enabled.store(config.autoScreenOffInDark);

    // The stack must live in internal RAM, not PSRAM. Anything this task calls
    // that touches flash (NVS, partition reads) disables the cache, which makes
    // a PSRAM stack unreachable and aborts on
    // esp_task_stack_is_sane_cache_disabled().
    xTaskCreatePinnedToCore(_ambient_light_update_task, "ambientlight", 1024 * 4, nullptr, 2, NULL, 1);
}

void Hal::setAutoScreenOffInDark(bool enabled)
{
    _dark_enabled.store(enabled);
}

int Hal::getAmbientLightLevel()
{
    return _ambient_level.load();
}

bool Hal::isScreenOffByAmbientLight()
{
    return _screen_off.load();
}

bool Hal::wakeScreenOnTouch()
{
    if (!_screen_off.load()) {
        return false;
    }

    // A tap beats the dark rule until the light properly returns. Dim rooms
    // measure barely above a dark one, so without this the screen would sleep
    // again within seconds and there would be no way to keep it on short of
    // turning the feature off in settings.
    _manual_on.store(true);

    mclog::tagInfo(_tag, "screen on: touch");
    _screen_wake();
    return true;
}
