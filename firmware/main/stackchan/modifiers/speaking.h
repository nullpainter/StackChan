/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "../modifiable.h"
#include "../utils/random.h"
#include <smooth_ui_toolkit.hpp>
#include <hal/hal.h>
#include <cstdint>
#include <utility>

namespace stackchan {

class SpeakingModifier : public Modifier {
public:
    /**
     * @param destroyAfterMs speaking lifetime in ms (0 means persistent until removed)
     * @param mouthIntervalMs mouth open/close interval in ms
     * @param enableMotion enable subtle head motion while speaking
     * @param motionIntervalMinMs minimum head-motion interval in ms
     * @param motionIntervalMaxMs maximum head-motion interval in ms
     */
    SpeakingModifier(uint32_t destroyAfterMs = 0, uint32_t mouthIntervalMs = 180, bool enableMotion = true,
                     uint32_t motionIntervalMinMs = 1500, uint32_t motionIntervalMaxMs = 2500)
        : _mouth_interval_ms(mouthIntervalMs),
          _motion_interval_min_ms(motionIntervalMinMs),
          _motion_interval_max_ms(motionIntervalMaxMs),
          _enable_motion(enableMotion)
    {
        if (_motion_interval_max_ms < _motion_interval_min_ms) {
            std::swap(_motion_interval_min_ms, _motion_interval_max_ms);
        }

        uint32_t now = GetHAL().millis();

        // Lifetime timer
        if (destroyAfterMs > 0) {
            _destroy_at   = now + destroyAfterMs;
            _has_lifetime = true;
        }

        // Mouth animation timer
        _next_mouth_tick = now + _mouth_interval_ms;

        // Motion timer
        if (_enable_motion) {
            _next_motion_tick = now + Random::getInstance().getInt(_motion_interval_min_ms, _motion_interval_max_ms);
        }

        _need_get_prev_angles = true;
    }

    void _update(Modifiable& stackchan) override
    {
        if (!stackchan.hasAvatar()) {
            return;
        }

        uint32_t now = GetHAL().millis();

        // Check lifetime
        if (_has_lifetime && now >= _destroy_at) {
            stackchan.avatar().mouth().setWeight(0);  // close mouth
            requestDestroy();
            return;
        }

        // Mouth animation
        if (now >= _next_mouth_tick) {
            _next_mouth_tick = now + _mouth_interval_ms;
            animate_mouth(stackchan.avatar());
        }

        // Subtle speaking motion
        if (_enable_motion && now >= _next_motion_tick) {
            _next_motion_tick = now + Random::getInstance().getInt(_motion_interval_min_ms, _motion_interval_max_ms);
            perform_subtle_speaking_motion(stackchan);
        }
    }

private:
    void animate_mouth(avatar::Avatar& avatar)
    {
        _is_mouth_open = !_is_mouth_open;
        auto& random   = Random::getInstance();

        int weight = _is_mouth_open ? random.getInt(_open_min_weight, _open_max_weight)
                                    : random.getInt(_close_min_weight, _close_max_weight);

        avatar.mouth().setWeight(weight);
    }

    void perform_subtle_speaking_motion(Modifiable& stackchan)
    {
        auto& motion = stackchan.motion();
        if (motion.isMoving()) {
            return;
        }

        float target_x = 0.0f;
        float target_y = 0.0f;
        if (GetHAL().getConversationTarget(target_x, target_y)) {
            // Keep eye contact while retaining slight natural jitter.
            const float jitter_x  = static_cast<float>(Random::getInstance().getInt(-50, 50)) / 1000.0f;
            const float jitter_y  = static_cast<float>(Random::getInstance().getInt(-60, 60)) / 1000.0f;
            const float clamped_x = uitk::clamp(target_x + jitter_x, -1.0f, 1.0f);
            const float clamped_y = uitk::clamp(target_y + jitter_y, -1.0f, 1.0f);
            const int speed       = Random::getInstance().getInt(100, 200);

            motion.lookAtNormalized(clamped_x, clamped_y, speed);
            _need_get_prev_angles = true;
            return;
        }

        uitk::Vector2i current_actual_angles = motion.getCurrentAngles();

        if (_need_get_prev_angles) {
            _prev_angles          = current_actual_angles;
            _need_get_prev_angles = false;
        } else {
            // If external movement is large, resync baseline to avoid snapping back.
            const int32_t threshold = 300;
            int32_t diff_x          = std::abs(current_actual_angles.x - _prev_angles.x);
            int32_t diff_y          = std::abs(current_actual_angles.y - _prev_angles.y);

            if (diff_x > threshold || diff_y > threshold) {
                _prev_angles = current_actual_angles;
            }
        }

        int32_t target_yaw   = _prev_angles.x;
        int32_t target_pitch = _prev_angles.y;

        int action = Random::getInstance().getInt(0, 10);
        int speed  = Random::getInstance().getInt(100, 200);  // keep speaking motion gentle

        if (action < 5) {
            // Action A: subtle nod
            target_pitch += Random::getInstance().getInt(-20, 50);
        } else {
            // Action B: subtle yaw drift
            target_yaw += Random::getInstance().getInt(-40, 40);
            target_pitch += Random::getInstance().getInt(-20, 20);
        }

        motion.moveWithSpeed(target_yaw, target_pitch, speed);
    }

    // Config constants
    const int _open_min_weight  = 40;
    const int _open_max_weight  = 80;
    const int _close_min_weight = 0;
    const int _close_max_weight = 20;

    // Timer state
    uint32_t _destroy_at       = 0;
    uint32_t _next_mouth_tick  = 0;
    uint32_t _next_motion_tick = 0;
    uint32_t _mouth_interval_ms;
    uint32_t _motion_interval_min_ms;
    uint32_t _motion_interval_max_ms;

    bool _has_lifetime         = false;
    bool _enable_motion        = false;
    bool _is_mouth_open        = false;
    bool _need_get_prev_angles = true;

    uitk::Vector2i _prev_angles;
};

}  // namespace stackchan
