#include <zenith/infantry_hud.hpp>
#include "canvas.hpp"

#include <algorithm>
#include <array>
#include <string>

namespace zenith::ui {
namespace {
using core::Stage;

struct Box {
    float x, y, w, h;
    bool contains(float px, float py) const { return x <= px && px < x+w && y <= py && py < y+h; }
};
constexpr Box health{364, 267, 266, 60}, power{650, 267, 266, 60};
constexpr Box cooling{364, 362, 266, 60}, burst{650, 362, 266, 60};
constexpr Box start{604, 448, 312, 54}, supply{1032, 642, 210, 44};
constexpr Box close_supply{1090, 545, 142, 42};

struct Color { Uint8 r, g, b; };
constexpr Color panel{9, 18, 27}, raised{23, 39, 51}, border{64, 90, 105};
constexpr Color ink{235, 244, 248}, muted{166, 189, 202}, mint{108, 247, 204};
constexpr Color red{255, 132, 129}, blue{136, 195, 255}, amber{255, 207, 108};

struct Painter {
    detail::Canvas& canvas;
    void fill(Box b, Color c, Uint8 alpha = 255) {
        canvas.rect(b.x, b.y, b.w, b.h, c.r, c.g, c.b, alpha);
    }
    void panel_box(Box b, Color accent = border, Uint8 alpha = 241) {
        fill(b, panel, alpha);
        fill({b.x,b.y,b.w,2}, accent);
    }
    void text(float x, float y, const std::string& s, float scale = 1.5f, Color c = ink) {
        canvas.text(x, y, s, c.r, c.g, c.b, scale);
    }
    void centered(Box b, float y, const std::string& s, float scale, Color c = ink) {
        text(b.x + (b.w - s.size() * 8 * scale) / 2, y, s, scale, c);
    }
};

std::string clock_text(int seconds) {
    auto two = [](int n) { return (n < 10 ? "0" : "") + std::to_string(n); };
    return two(seconds / 60) + ":" + two(seconds % 60);
}
const char* stage_name(Stage stage) {
    switch (stage) {
    case Stage::preparation: return "PREPARATION";
    case Stage::countdown: return "COUNTDOWN";
    case Stage::live: return "MATCH LIVE";
    case Stage::settlement: return "FINAL";
    }
    return "";
}
} // namespace

void InfantryHud::move(float x, float y) { x_ = x; y_ = y; }
void InfantryHud::leave() { move(-1, -1); }
void InfantryHud::cancel() {
    leave();
    pressed_ = Target::none;
    tab_ = false;
}
bool InfantryHud::stats_visible(Stage stage) const { return stage == Stage::live && tab_; }
bool InfantryHud::supply_visible(Stage stage) const { return stage == Stage::live && supply_; }
bool InfantryHud::escape(Stage stage) {
    const bool was_visible = supply_visible(stage);
    supply_ = false;
    pressed_ = Target::none;
    return was_visible;
}
InfantryHud::Target InfantryHud::hit(Stage stage) const {
    if (stage == Stage::preparation) {
        if (health.contains(x_, y_)) return Target::health;
        if (power.contains(x_, y_)) return Target::power;
        if (cooling.contains(x_, y_)) return Target::cooling;
        if (burst.contains(x_, y_)) return Target::burst;
        if (start.contains(x_, y_)) return Target::start;
    }
    if (stage == Stage::live) {
        if (supply.contains(x_, y_)) return Target::supply;
        if (supply_ && close_supply.contains(x_, y_)) return Target::close_supply;
    }
    return Target::none;
}
void InfantryHud::mouse(bool down, core::InfantryPractice& round, core::InfantryPractice::Time now) {
    const auto target = hit(round.snapshot(now).stage);
    if (down) {
        pressed_ = target;
        return;
    }
    if (target == pressed_) {
        switch (target) {
        case Target::health: round.select(core::Chassis::health); break;
        case Target::power: round.select(core::Chassis::power); break;
        case Target::cooling: round.select(core::Launcher::cooling); break;
        case Target::burst: round.select(core::Launcher::burst); break;
        case Target::start: round.start(now); break;
        case Target::supply: supply_ = !supply_; break;
        case Target::close_supply: supply_ = false; break;
        case Target::none: break;
        }
    }
    pressed_ = Target::none;
}

void InfantryHud::draw(void* pixels, int width, int height, int pitch, const core::InfantrySnapshot& s,
                       bool video_frozen) const {
    detail::Canvas canvas{pixels, width, height, pitch};
    Painter p{canvas};
    const auto hovered = hit(s.stage);
    auto button = [&](Box b, Target target, const char* name, bool selected = false, const char* note = nullptr) {
        const bool is_hovered = target == hovered;
        const bool held = is_hovered && target == pressed_;
        p.fill(b, held ? mint : is_hovered ? Color{39, 68, 76} : raised);
        p.fill({b.x, b.y, 3, b.h}, selected || is_hovered ? mint : border);
        p.text(b.x+12, b.y+(note ? 10 : (b.h-14)/2), name, 1.75f, held ? panel : selected ? mint : ink);
        if (note) p.text(b.x+12, b.y+37, note, 1.5f, held ? panel : muted);
    };

    constexpr Box top{24,20,1232,78}, center{520,20,240,78};
    p.panel_box(top);
    p.text(40, 32, "ZENITH / RMUL 27", 1.5f, muted);
    p.text(40, 57, "03 INFANTRY", 2, ink);
    p.text(1004, 32, "PRACTICE / 10X", 1.5f, muted);
    p.text(1004, 59, video_frozen ? "VIDEO FROZEN" : "VIDEO LIVE", 1.75f, video_frozen ? amber : mint);

    constexpr Box red_score{294,20,226,78}, blue_score{760,20,226,78};
    p.panel_box(red_score, red);
    p.panel_box(blue_score, blue);
    p.centered(red_score, 32, "RED / YOUR CONTROL", 1.5f, red);
    p.centered(blue_score, 32, "BLUE / CONTROL", 1.5f, blue);
    p.centered(red_score, 55, std::to_string(s.red_control), 3.5f);
    p.centered(blue_score, 55, std::to_string(s.blue_control), 3.5f);
    p.centered(center, 29, stage_name(s.stage), 1.5f, mint);
    p.centered(center, 50, s.stage == Stage::preparation ? "--:--" : clock_text(s.seconds_left), 3.75f);

    // The video's center stays clear while driving.
    if (s.stage == Stage::live) {
        p.fill({615,359,16,2}, mint, 200);
        p.fill({649,359,16,2}, mint, 200);
        p.fill({639,335,2,16}, mint, 200);
        p.fill({639,369,2,16}, mint, 200);
        p.fill({639,359,2,2}, ink, 230);
    }

    p.panel_box({24,632,1232,64});
    auto meter = [&](float x, const char* label, int value, int max, Color accent) {
        p.text(x, 642, label, 1.5f, muted);
        auto number = std::to_string(value);
        if (max != 0) number += "/" + std::to_string(max);
        p.text(x, 662, number, 2);
        if (max != 0) {
            p.fill({x,686,144,3}, border);
            p.fill({x,686,144.f * std::clamp(static_cast<float>(value)/max, 0.f, 1.f),3}, accent);
        }
    };
    meter(40, "HEALTH", s.health, s.max_health, mint);
    meter(238, "HEAT", s.heat, s.max_heat, amber);
    meter(436, "ENERGY / J", s.energy, s.max_energy, blue);
    meter(634, "17 MM ALLOWED", s.ammo, 0, ink);
    if (s.stage == Stage::live) {
        p.text(810, 656, "HOLD TAB", 1.75f, stats_visible(s.stage) ? mint : ink);
        p.text(810, 676, "TEAM HEALTH", 1.25f, muted);
        button(supply, Target::supply, s.supply_available ? "SUPPLY READY" : "SUPPLY", supply_visible(s.stage));
    } else {
        p.text(810, 656, s.chassis == core::Chassis::health ? "HEALTH PRIORITY" : "POWER PRIORITY", 1.5f, muted);
        p.text(810, 675, s.launcher == core::Launcher::cooling ? "COOLING PRIORITY" : "BURST PRIORITY", 1.5f, muted);
    }

    if (s.stage == Stage::preparation) {
        p.panel_box({340,174,600,367}, mint, 249);
        p.text(364, 195, "SET YOUR LOADOUT", 2.75f);
        p.text(364, 246, "01 / CHASSIS", 1.5f, muted);
        button(health, Target::health, "HEALTH PRIORITY", s.chassis == core::Chassis::health, "350 HP  /  75 W");
        button(power, Target::power, "POWER PRIORITY", s.chassis == core::Chassis::power, "300 HP  /  90 W");
        p.text(364, 341, "02 / LAUNCHER", 1.5f, muted);
        button(cooling, Target::cooling, "COOLING PRIORITY", s.launcher == core::Launcher::cooling, "88 HEAT  /  24 PER S");
        button(burst, Target::burst, "BURST PRIORITY", s.launcher == core::Launcher::burst, "230 HEAT  / 12 PER S");
        p.text(364, 471, "LOCAL PRACTICE", 1.5f, muted);
        button(start, Target::start, "CONFIRM & START", true);
        p.text(364, 519, "LOADOUT LOCKS AT START", 1.25f, muted);
    } else if (s.stage == Stage::countdown) {
        constexpr Box card{476,250,328,156};
        p.panel_box(card, mint);
        p.centered(card, 270, "MATCH STARTS IN", 1.75f, muted);
        p.centered(card, 305, std::to_string(s.seconds_left), 8, mint);
        p.centered(card, 382, "LOADOUT LOCKED", 1.5f, muted);
    } else if (s.stage == Stage::settlement) {
        constexpr Box card{392,235,496,210};
        const bool red_win = s.red_control > s.blue_control, blue_win = s.blue_control > s.red_control;
        p.panel_box(card, red_win ? red : blue_win ? blue : mint, 249);
        p.centered(card, 259, "PRACTICE RESULT", 1.75f, muted);
        p.centered(card, 297, red_win ? "RED WINS" : blue_win ? "BLUE WINS" : "DRAW", 4, ink);
        p.centered(card, 358, std::to_string(s.red_control) + "  /  " + std::to_string(s.blue_control) + " CP", 2.5f);
        p.centered(card, 407, "ESC TO EXIT", 1.5f, muted);
    }

    if (stats_visible(s.stage)) {
        p.panel_box({24,120,344,204}, mint);
        p.text(40, 135, "TEAM HEALTH", 2);
        p.text(207, 164, "RED", 1.5f, red);
        p.text(285, 164, "BLUE", 1.5f, blue);
        const char* roles[]{"1 HEAVY", "3 INFANTRY", "7 SENTRY"};
        for (int i = 0; i < 3; ++i) {
            const float y = static_cast<float>(185 + 32*i);
            if (i == 1) p.fill({35,y,322,28}, raised);
            p.text(40, y+7, roles[i]);
            p.text(207, y+7, std::to_string(s.red_health[i]), 1.75f);
            p.text(285, y+7, std::to_string(s.blue_health[i]), 1.75f);
        }
        p.text(40, 298, "RELEASE TAB TO HIDE", 1.25f, muted);
    }
    if (supply_visible(s.stage)) {
        p.panel_box({860,383,396,223}, mint);
        p.text(884, 400, "SUPPLY / 17 MM", 2);
        p.text(884, 436, std::to_string(s.gold) + " TEAM GOLD", 2.25f);
        p.text(884, 469, "10 GOLD / 10 ROUNDS", 1.75f, muted);
        p.text(884, 499, s.supply_available ? "IN SUPPLY ZONE" : "SUPPLY ZONE REQUIRED", 1.75f,
               s.supply_available ? mint : amber);
        p.text(884, 563, "VIEW ONLY", 1.5f, muted);
        button(close_supply, Target::close_supply, "CLOSE");
    }
    if (video_frozen) {
        constexpr Box warning{446,565,388,42};
        p.panel_box(warning, amber, 255);
        p.centered(warning, 578, "VIDEO FROZEN / LAST FRAME", 1.75f, amber);
    }
    canvas.finish();
}
} // namespace zenith::ui
