#pragma once

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <span>
#include <string>
#include <vector>

namespace zenith::display {
struct NamedValue { std::uint64_t value; std::string name; };
struct Property {
    std::uint32_t id{}, flags{};
    std::string name;
    std::uint64_t value{};
    std::vector<std::uint64_t> values;
    std::vector<NamedValue> enums;
};
struct FormatModifier {
    std::uint32_t format{};
    std::uint64_t modifier{};
};
struct Crtc {
    std::uint32_t id{}, index{};
    std::vector<Property> properties;
};
struct Encoder {
    std::uint32_t id{}, crtc_id{}, possible_crtcs{};
};
struct Mode {
    std::string name;
    std::uint32_t width{}, height{}, refresh_hz{};
    bool preferred{};
};
struct Connector {
    std::uint32_t id{}, encoder_id{}, possible_crtcs{};
    std::string name, connection;
    bool writeback{};
    std::vector<std::uint32_t> encoders, writeback_formats;
    std::vector<Mode> modes;
    std::vector<Property> properties;
};
struct Plane {
    std::uint32_t id{}, crtc_id{}, fb_id{}, possible_crtcs{};
    std::string type;
    std::vector<std::uint32_t> formats;
    bool has_in_formats{};
    std::vector<FormatModifier> format_modifiers;
    std::vector<Property> properties;
};
struct Capability { std::string name; bool available{}; std::uint64_t value{}; };
struct DeviceSnapshot {
    std::string path, driver;
    bool atomic{}, universal_planes{}, writeback_connectors{};
    std::vector<Capability> capabilities;
    std::vector<Crtc> crtcs;
    std::vector<Encoder> encoders;
    std::vector<Connector> connectors;
    std::vector<Plane> planes;
    std::vector<std::string> warnings;
};

// Snapshots own their data; no DRM pointers or borrowed file descriptors escape.
std::vector<std::string> card_paths();
std::string driver_name(const std::string& path);
DeviceSnapshot probe_device(const std::string& path);
void print_report(std::ostream& out, const DeviceSnapshot& device);
std::string fourcc_name(std::uint32_t value);
// Separate, bounds-checked parsers allow deterministic tests without a DRM device.
std::vector<FormatModifier> parse_format_modifiers(std::span<const std::byte> blob);
std::vector<std::uint32_t> parse_writeback_formats(std::span<const std::byte> blob);
} // namespace zenith::display
