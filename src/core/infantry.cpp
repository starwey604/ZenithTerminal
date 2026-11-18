#include <zenith/infantry.hpp>

#include <algorithm>

namespace zenith::core {
namespace {
using namespace std::chrono;
constexpr auto countdown = 5s;
constexpr int round_seconds = 300;
constexpr int practice_speed = 10;

struct Sample {
    int at, red_control, blue_control;
    int health_loss, heat, energy, ammo, gold;
    int red_heavy, red_sentry;
    std::array<int, 3> blue_health;
    bool supply_available;
};
constexpr Sample samples[]{
    {  0, 600, 600,   0,  0, 60,  0, 200, 400, 400, {400, 350, 400}, false},
    { 40, 580, 566,  78,  0, 34,  0, 200, 352, 400, {386, 252, 393}, false},
    { 80, 554, 533,  18, 19, 58,  0, 400, 352, 372, {338, 237, 321}, true},
    {160, 518, 471,   0,  0, 60,100, 500, 323, 338, {295, 217, 280}, true},
    {210, 495, 421,  51, 73, 29, 60, 800, 280, 338, {257, 179, 226}, false},
    {299, 438, 329, 111, 38, 45, 21,1100, 244, 338, {194, 138, 157}, false},
};
} // namespace

void InfantryPractice::select(Chassis chassis) {
    if (!started_) chassis_ = chassis;
}
void InfantryPractice::select(Launcher launcher) {
    if (!started_) launcher_ = launcher;
}
void InfantryPractice::start(Time now) {
    if (!started_) started_ = now;
}

InfantrySnapshot InfantryPractice::snapshot(Time now) const {
    InfantrySnapshot s;
    s.chassis = chassis_;
    s.launcher = launcher_;
    s.max_health = chassis_ == Chassis::health ? 350 : 300;
    s.max_heat = launcher_ == Launcher::cooling ? 88 : 230;
    s.max_energy = 60;
    int game_elapsed = 0;
    if (started_) {
        const auto elapsed = std::max(now - *started_, Clock::duration::zero());
        if (elapsed < countdown) {
            s.stage = Stage::countdown;
            s.seconds_left = static_cast<int>(ceil<seconds>(countdown - elapsed).count());
        } else {
            const auto game_time = (elapsed - countdown) * practice_speed;
            game_elapsed = static_cast<int>(duration_cast<seconds>(std::min(
                game_time, duration_cast<Clock::duration>(seconds(round_seconds)))).count());
            s.stage = game_elapsed == round_seconds ? Stage::settlement : Stage::live;
            s.seconds_left = round_seconds - game_elapsed;
        }
    }

    const auto* sample = &samples[0];
    for (const auto& next : samples)
        if (next.at <= game_elapsed) sample = &next;
    s.health = s.max_health - sample->health_loss;
    s.heat = sample->heat;
    s.energy = sample->energy;
    s.ammo = sample->ammo;
    s.gold = sample->gold;
    s.red_control = sample->red_control;
    s.blue_control = sample->blue_control;
    s.red_health = {sample->red_heavy, s.health, sample->red_sentry};
    s.blue_health = sample->blue_health;
    s.supply_available = s.stage == Stage::live && sample->supply_available;
    return s;
}
} // namespace zenith::core
