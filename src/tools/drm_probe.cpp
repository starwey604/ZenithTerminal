#include <zenith/display/probe.hpp>
#include <exception>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    std::string device, driver;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help") {
            std::cout << "Usage: zenith_drm_probe [--device /dev/dri/cardN | --driver vkms]\n"
                         "Default: enumerate all card nodes. Query only; never modesets or requests master.\n"
                         "Exit: 0 complete query, 1 query error, 2 invalid arguments, 3 no matching device.\n";
            return 0;
        }
        if ((arg == "--device" || arg == "--driver") && i + 1 < argc && device.empty() && driver.empty()) {
            (arg == "--device" ? device : driver) = argv[++i];
            const auto& value = arg == "--device" ? device : driver;
            if (!value.empty() && !value.starts_with("-")) continue;
        }
        std::cerr << "Invalid arguments; use --help.\n";
        return 2;
    }
    try {
        const auto paths = device.empty() ? zenith::display::card_paths() : std::vector<std::string>{device};
        bool matched = false, failed = false;
        for (const auto& path : paths) {
            try {
                if (!driver.empty() && zenith::display::driver_name(path) != driver) continue;
                matched = true;
                zenith::display::print_report(std::cout, zenith::display::probe_device(path));
            } catch (const std::exception& error) {
                failed = true;
                std::cerr << path << ": " << error.what() << '\n';
            }
        }
        if (failed) return 1;
        if (!matched) {
            std::cerr << "No matching DRM card" << (driver.empty() ? "" : " for driver " + driver)
                << ". Check /dev/dri, module loading and device access.\n";
            return 3;
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
