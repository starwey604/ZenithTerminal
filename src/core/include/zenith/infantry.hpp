#pragma once

#include <array>
#include <chrono>
#include <optional>

namespace zenith::core {
enum class Stage { preparation, countdown, live, settlement };
enum class Chassis { health, power };
enum class Launcher { cooling, burst };

struct InfantrySnapshot {
    Stage stage{Stage::preparation};
    Chassis chassis{Chassis::health};
    Launcher launcher{Launcher::cooling};
    int seconds_left{};
    int health{}, max_health{}, heat{}, max_heat{}, energy{}, max_energy{}, ammo{}, gold{};
    int red_control{}, blue_control{};
    // Heavy, infantry, sentry, in that order.
    std::array<int, 3> red_health{}, blue_health{};
    bool supply_available{};
};

// A local RMUL infantry fixture. It only advances after an explicit start,
// and has no network or display dependencies.
class InfantryPractice {
public:
    using Clock = std::chrono::steady_clock;
    using Time = Clock::time_point;

    void select(Chassis chassis);
    void select(Launcher launcher);
    void start(Time now);
    InfantrySnapshot snapshot(Time now) const;

private:
    std::optional<Time> started_;
    Chassis chassis_{Chassis::health};
    Launcher launcher_{Launcher::cooling};
};
} // namespace zenith::core
