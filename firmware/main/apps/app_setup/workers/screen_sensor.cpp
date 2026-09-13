/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "workers.h"
#include <mooncake_log.h>
#include <hal/hal.h>
#include <hal/board/hal_bridge.h>
#include <stackchan/stackchan.h>
#include <cstdint>
#include <vector>

using namespace smooth_ui_toolkit::lvgl_cpp;
using namespace setup_workers;

static std::string _tag = "Setup-ScreenSensor";

namespace {

void style_switch(Switch& sw)
{
    sw.setSize(64, 36);
    sw.setBgColor(lv_color_hex(0xB8D3FD), LV_PART_MAIN);
    sw.setBgColor(lv_color_hex(0x615B9E), LV_PART_INDICATOR | LV_STATE_CHECKED);
    sw.setBgColor(lv_color_hex(0xFFFFFF), LV_PART_KNOB);
}

void style_panel(Container& panel, int32_t y, int32_t height)
{
    panel.setSize(296, height);
    panel.align(LV_ALIGN_TOP_MID, 0, y);
    panel.setBgColor(lv_color_hex(0xD2E3FF));
    panel.setBorderWidth(0);
    panel.setRadius(18);
    panel.setPadding(0, 0, 0, 0);
    panel.removeFlag(LV_OBJ_FLAG_SCROLLABLE);
}

void style_title(Label& label)
{
    label.setTextFont(&lv_font_montserrat_16);
    label.setTextColor(lv_color_hex(0x26206A));
    label.setWidth(260);
    label.setTextAlign(LV_TEXT_ALIGN_CENTER);
    label.align(LV_ALIGN_TOP_MID, 0, 18);
}

}  // namespace

ScreenOffWorker::ScreenOffWorker()
{
    mclog::info("ScreenOffWorker start");

    _config = GetHAL().getXiaozhiConfig();

    _panel = std::make_unique<Container>(lv_screen_active());
    _panel->setBgColor(lv_color_hex(0xEDF4FF));
    _panel->align(LV_ALIGN_CENTER, 0, 0);
    _panel->setBorderWidth(0);
    _panel->setSize(320, 240);
    _panel->setRadius(0);
    _panel->setPadding(0, 50, 24, 18);
    _panel->setScrollDir(LV_DIR_VER);
    _panel->setScrollbarMode(LV_SCROLLBAR_MODE_ACTIVE);

    _panel_dark = std::make_unique<Container>(_panel->get());
    style_panel(*_panel_dark, 20, 160);

    _label_dark_title = std::make_unique<Label>(_panel_dark->get());
    _label_dark_title->setText("Screen off in the dark\n(wakes when light returns,\nor when tapped):");
    style_title(*_label_dark_title);

    _switch_dark = std::make_unique<Switch>(_panel_dark->get());
    style_switch(*_switch_dark);
    _switch_dark->align(LV_ALIGN_TOP_MID, 0, 106);
    if (_config.autoScreenOffInDark) {
        _switch_dark->addState(LV_STATE_CHECKED);
    }

    _btn_confirm = std::make_unique<Button>(_panel->get());
    apply_button_common_style(*_btn_confirm);
    _btn_confirm->align(LV_ALIGN_TOP_MID, 0, 200);
    _btn_confirm->setSize(290, 50);
    _btn_confirm->label().setText("Confirm");
    _btn_confirm->onClick().connect([this]() { _confirm_flag = true; });
}

void ScreenOffWorker::update()
{
    if (!_confirm_flag) {
        return;
    }
    _confirm_flag = false;

    _config.autoScreenOffInDark = _switch_dark->getValue();
    GetHAL().setXiaozhiConfig(_config);

    // Applied live so the sensor powers up or down immediately, rather than
    // waiting for the next boot.
    GetHAL().setAutoScreenOffInDark(_config.autoScreenOffInDark);

    mclog::tagInfo(_tag, "screen off config updated: inDark={}", _config.autoScreenOffInDark);
    _is_done = true;
}

namespace {

struct NamedColor {
    const char* name;
    uint32_t rgb;
};

// Includes the two defaults so a user who changes their mind can get back to
// the original look without knowing what the values were.
const std::vector<NamedColor> _palette = {
    {"Aqua", 0x004040},    {"Blue", 0x0000FF},   {"Cyan", 0x00FFFF},   {"Teal", 0x008080}, {"Green", 0x00FF00},
    {"Lime", 0x40FF40},    {"Yellow", 0xFFFF00}, {"Orange", 0xFF6000}, {"Red", 0xFF0000},  {"Pink", 0xFF0080},
    {"Magenta", 0xFF00FF}, {"Purple", 0x8000FF}, {"White", 0xFFFFFF},  {"Warm", 0xFF8040}, {"Off", 0x000000},
};

}  // namespace

LedColorWorker::LedColorWorker(Target target) : _target(target)
{
    mclog::info("LedColorWorker start");

    _config = GetHAL().getXiaozhiConfig();

    const uint32_t current = _target == Target::Chat ? _config.chatLedColor : _config.speechLedColor;

    _panel = std::make_unique<Container>(lv_screen_active());
    _panel->setBgColor(lv_color_hex(0xEDF4FF));
    _panel->align(LV_ALIGN_CENTER, 0, 0);
    _panel->setBorderWidth(0);
    _panel->setSize(320, 240);
    _panel->setRadius(0);
    _panel->setFlexFlow(LV_FLEX_FLOW_COLUMN);
    _panel->setFlexAlign(LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    _panel->setPadding(20, 20, 20, 20);
    _panel->setPadRow(12);

    _label_title = std::make_unique<Label>(*_panel);
    _label_title->setText(_target == Target::Chat ? "Chat colour" : "Speech colour");
    _label_title->setTextFont(&lv_font_montserrat_24);
    _label_title->setTextColor(lv_color_hex(0x26206A));

    for (const auto& entry : _palette) {
        auto btn = std::make_unique<Button>(*_panel);
        apply_button_common_style(*btn);
        btn->setSize(240, 46);
        btn->label().setText(entry.name);

        // A swatch rather than only a name, because "Aqua" and "Teal" do not
        // mean much until you see them on the strips.
        btn->setBgColor(lv_color_hex(entry.rgb == 0 ? 0x808080 : entry.rgb));
        btn->setBgOpa(entry.rgb == 0 ? 80 : 255);

        // The current choice is marked, otherwise the page gives no clue which
        // of fifteen buttons is already in effect.
        if (entry.rgb == current) {
            btn->setBorderWidth(4);
            btn->setBorderColor(lv_color_hex(0x26206A));
        }

        const uint32_t rgb = entry.rgb;
        btn->onClick().connect([this, rgb]() { _pending_color = static_cast<int32_t>(rgb); });

        _buttons.push_back(std::move(btn));
    }

    auto btn_back = std::make_unique<Button>(*_panel);
    apply_button_common_style(*btn_back);
    btn_back->setSize(240, 50);
    btn_back->label().setText("Back");
    btn_back->onClick().connect([this]() { _confirm_flag = true; });
    _buttons.push_back(std::move(btn_back));
}

void LedColorWorker::update()
{
    if (_pending_color != -1) {
        const uint32_t rgb = static_cast<uint32_t>(_pending_color);
        _pending_color     = -1;

        if (_target == Target::Chat) {
            _config.chatLedColor = rgb;
        } else {
            _config.speechLedColor = rgb;
        }
        GetHAL().setXiaozhiConfig(_config);
        hal_bridge::board_reload_led_colors();

        // Preview on the strips, so the choice can be judged on the hardware
        // rather than from a label.
        const uint8_t r = (rgb >> 16) & 0xFF;
        const uint8_t g = (rgb >> 8) & 0xFF;
        const uint8_t b = rgb & 0xFF;
        GetStackChan().leftNeonLight().setColor(r, g, b);
        GetStackChan().rightNeonLight().setColor(r, g, b);

        mclog::tagInfo(_tag, "{} colour set to {:#08x}", _target == Target::Chat ? "chat" : "speech", rgb);
    }

    if (_confirm_flag) {
        _confirm_flag = false;
        _is_done      = true;
    }
}

LedColorWorker::~LedColorWorker()
{
    GetStackChan().leftNeonLight().setColor(0, 0, 0);
    GetStackChan().rightNeonLight().setColor(0, 0, 0);
}
