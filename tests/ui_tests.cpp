#include <zenith/ui.hpp>
#include <zenith/infantry_hud.hpp>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>
namespace {
void check_size(int width, int height) {
    const int pitch=width*4+64;
    std::vector<std::uint8_t> pixels(pitch*height, 0xa5);
    zenith::ui::draw(pixels.data(), width, height, pitch, 42);
    const int x[]{36,76,116};
    const std::uint32_t expected[]{0x00000000,0x80808080,0xffffffff};
    for (int i=0;i<3;++i) {
        std::uint32_t p; std::memcpy(&p, pixels.data()+(height-28)*pitch+x[i]*4,4);
        if (p!=expected[i]) throw std::runtime_error("UI premultiplied alpha swatch mismatch");
    }
    for (int y=0;y<height;++y)
        for (int x=width*4;x<pitch;++x)
            if (pixels[y*pitch+x]!=0xa5) throw std::runtime_error("UI overwrote pitch padding");
}

void check_input_button() {
    zenith::ui::InputButton button;
    auto expect = [&](bool hovered, bool held, std::uint64_t activations) {
        if (button.hovered() != hovered || button.held() != held || button.activations() != activations)
            throw std::runtime_error("UI button state mismatch");
    };
    expect(false, false, 0);
    button.move(1000, 260);
    expect(true, false, 0);
    button.mouse(true);
    expect(true, true, 0);
    button.mouse(false);
    expect(true, false, 1);
    button.mouse(true);
    button.leave();
    button.mouse(false);
    expect(false, false, 1);
    button.mouse(true); // Press outside, then move in before releasing.
    button.move(1000, 260);
    button.mouse(false);
    expect(true, false, 1);
    button.space(true);
    expect(true, true, 1);
    button.space(false);
    expect(true, false, 2);
    button.space(true);
    button.mouse(true);
    button.cancel();
    button.move(1000, 260);
    button.space(false);
    button.mouse(false);
    expect(true, false, 2);
}

void check_infantry_hud() {
    using namespace zenith;
    using namespace std::chrono_literals;
    using core::Stage;
    core::InfantryPractice round;
    ui::InfantryHud hud;
    const core::InfantryPractice::Time epoch{};
    auto expect = [](bool condition, const char* reason) {
        if (!condition) throw std::runtime_error(reason);
    };
    auto click = [&](float x, float y, auto now) {
        hud.move(x, y);
        hud.mouse(true, round, now);
        hud.mouse(false, round, now);
    };
    auto before = round.snapshot(epoch);
    expect(before.stage == Stage::preparation && before.max_health == 350 && before.max_heat == 88,
           "HUD default loadout");
    hud.move(680, 290);
    hud.mouse(true, round, epoch);
    expect(round.snapshot(epoch).chassis == core::Chassis::health, "HUD must commit on release");
    hud.move(610, 290);
    hud.mouse(false, round, epoch);
    expect(round.snapshot(epoch).chassis == core::Chassis::health, "HUD release on another button");
    click(680, 290, epoch);
    click(680, 390, epoch);
    before = round.snapshot(epoch);
    expect(before.max_health == 300 && before.max_heat == 230, "HUD selected loadout");
    click(750, 475, epoch);
    round.select(core::Launcher::cooling);
    auto countdown = round.snapshot(epoch + 4999ms);
    expect(round.snapshot(epoch).seconds_left == 5 && countdown.stage == Stage::countdown &&
           countdown.seconds_left == 1 && countdown.max_heat == 230, "HUD countdown and lock");
    auto live = round.snapshot(epoch + 5s);
    expect(live.stage == Stage::live && live.seconds_left == 300 &&
           live.red_control == 600 && live.blue_control == 600 && !live.supply_available,
           "HUD initial live state");
    auto later = epoch + 14s;
    expect(round.snapshot(later).supply_available, "HUD scripted supply availability");
    hud.tab(true);
    expect(hud.stats_visible(Stage::live), "HUD Tab down");
    click(1110, 660, later);
    expect(hud.supply_visible(Stage::live) && hud.stats_visible(Stage::live), "HUD supply and read-only Tab overlap");
    hud.tab(false);
    expect(!hud.stats_visible(Stage::live) && hud.supply_visible(Stage::live), "HUD Tab up preserves supply");
    expect(hud.escape(Stage::live) && !hud.supply_visible(Stage::live), "HUD Escape closes supply first");
    hud.move(1110, 660);
    hud.mouse(true, round, later);
    hud.tab(true);
    hud.cancel();
    hud.move(1110, 660);
    hud.mouse(false, round, later);
    expect(!hud.supply_visible(Stage::live) && !hud.stats_visible(Stage::live), "HUD focus loss clears holds");
    click(1110, 660, later);
    click(1150, 565, later);
    expect(!hud.supply_visible(Stage::live), "HUD close button");
    auto final = round.snapshot(epoch + 35s);
    expect(final.stage == Stage::settlement && final.seconds_left == 0 && final.red_control > final.blue_control,
           "HUD settlement");
    expect(!hud.escape(Stage::settlement), "HUD settlement has no menu");
}
} // namespace
void ui_tests() { check_size(1280,720); check_size(1920,1080); check_input_button(); check_infantry_hud(); }
