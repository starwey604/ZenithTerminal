#pragma once

#include <zenith/infantry.hpp>

namespace zenith::ui {
class InfantryHud {
public:
    void move(float x, float y);
    void leave();
    void mouse(bool down, core::InfantryPractice& round, core::InfantryPractice::Time now);
    void tab(bool down) { tab_ = down; }
    void cancel();
    bool escape(core::Stage stage); // True when a visible menu was closed.
    bool stats_visible(core::Stage stage) const;
    bool supply_visible(core::Stage stage) const;

    void draw(void* pixels, int width, int height, int pitch, const core::InfantrySnapshot& state,
              bool video_frozen) const;

private:
    enum class Target { none, health, power, cooling, burst, start, supply, close_supply };
    Target hit(core::Stage stage) const;
    float x_ = -1, y_ = -1;
    Target pressed_{Target::none};
    bool tab_ = false, supply_ = false;
};
} // namespace zenith::ui
