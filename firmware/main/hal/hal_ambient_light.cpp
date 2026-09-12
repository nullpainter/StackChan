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
#include <atomic>

static const std::string_view _tag = "HAL-AmbientLight";

namespace {

// How often the camera is sampled for a light reading.
constexpr uint32_t kSampleIntervalMs = 1000;

// Mean frame luma (0-255) at or below which the screen should be off.
//
// The camera runs auto-exposure, so it never reports anything close to black:
// it raises gain and exposure to compensate for missing light. Measured on this
// hardware, an unlit room and a hand held over the lens both settle at about 28,
// while a normally lit room reads 87-148. The thresholds sit in that gap.
//
// Because the two dark cases read the same, they are not distinguished here.
// Both mean "nobody can see the screen", which is the same outcome either way.
constexpr int kDarkLumaThreshold = 45;

// Luma needed to call the room lit again. The gap from the dark threshold stops
// the screen flickering when a reading sits on the boundary.
constexpr int kBrightLumaThreshold = 65;

// Darkness has to persist before the screen goes off, so a passing shadow is
// ignored. Kept short enough that covering the camera by hand still feels
// responsive.
constexpr uint32_t kDarkSamplesToSleep = 4;

// Light has to come back for a moment too, otherwise sensor noise near the
// threshold would wake the screen repeatedly.
constexpr uint32_t kBrightSamplesToWake = 2;

// Samples between diagnostic log lines, so the thresholds above can be checked
// against real readings over the serial monitor.
constexpr uint32_t kLogEverySamples = 5;

// Used if the backlight somehow reads as already off when darkness is detected,
// so waking up can never leave the screen stuck black.
constexpr uint8_t kFallbackBrightness = 60;

std::atomic<int> _ambient_luma{-1};
std::atomic<bool> _screen_off{false};

void _ambient_light_update_task(void* param)
{
    mclog::tagInfo(_tag, "start update task");

    uint32_t dark_samples   = 0;
    uint32_t bright_samples = 0;
    uint32_t sample_count   = 0;
    uint8_t saved_brightness = kFallbackBrightness;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(kSampleIntervalMs));

        const int luma = hal_bridge::board_sample_ambient_luma();
        if (++sample_count % kLogEverySamples == 0) {
            mclog::tagInfo(_tag, "luma={}, screen_off={}", luma, _screen_off.load());
        }
        if (luma < 0) {
            // No reading this round (camera still warming up or busy taking a
            // photo). Leave the screen as it is and try again.
            continue;
        }
        _ambient_luma.store(luma);

        if (!_screen_off.load()) {
            dark_samples = luma <= kDarkLumaThreshold ? dark_samples + 1 : 0;
            if (dark_samples < kDarkSamplesToSleep) {
                continue;
            }

            // Remember the level to come back to. Reading it back from NVS is
            // not an option here: this task cannot touch flash (see below).
            saved_brightness = hal_bridge::board_get_backlight_brightness();
            if (saved_brightness == 0) {
                saved_brightness = kFallbackBrightness;
            }

            mclog::tagInfo(_tag, "screen off: luma={}", luma);
            hal_bridge::board_set_backlight_brightness(0);
            _screen_off.store(true);
            dark_samples   = 0;
            bright_samples = 0;
        } else {
            bright_samples = luma >= kBrightLumaThreshold ? bright_samples + 1 : 0;
            if (bright_samples < kBrightSamplesToWake) {
                continue;
            }

            mclog::tagInfo(_tag, "screen on: luma={}", luma);
            hal_bridge::board_set_backlight_brightness(saved_brightness);
            _screen_off.store(false);
            bright_samples = 0;
            dark_samples   = 0;
        }
    }
}

}  // namespace

void Hal::ambient_light_init()
{
    mclog::tagInfo(_tag, "init");

    // The stack must live in internal RAM, not PSRAM. Anything this task calls
    // that touches flash (NVS, partition reads) disables the cache, which makes
    // a PSRAM stack unreachable and aborts on
    // esp_task_stack_is_sane_cache_disabled().
    xTaskCreatePinnedToCore(_ambient_light_update_task, "ambientlight", 1024 * 4, nullptr, 2, NULL, 1);
}

int Hal::getAmbientLuma()
{
    return _ambient_luma.load();
}

bool Hal::isScreenOffByAmbientLight()
{
    return _screen_off.load();
}
