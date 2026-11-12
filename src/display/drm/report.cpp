#include <zenith/display/probe.hpp>
#include <drm_fourcc.h>
#include <xf86drmMode.h>
#include <algorithm>
#include <bit>
#include <iomanip>
#include <ostream>

namespace zenith::display {
namespace {
void print_properties(std::ostream& out, const std::vector<Property>& properties) {
    for (const auto& p : properties) {
        out << "    property " << p.name << " (#" << p.id << ") = ";
        if ((p.flags & DRM_MODE_PROP_EXTENDED_TYPE) == DRM_MODE_PROP_SIGNED_RANGE) out << std::bit_cast<std::int64_t>(p.value);
        else out << p.value;
        if (p.flags & DRM_MODE_PROP_IMMUTABLE) out << " immutable";
        if (p.flags & DRM_MODE_PROP_BLOB) out << " blob";
        if ((p.flags & DRM_MODE_PROP_EXTENDED_TYPE) == DRM_MODE_PROP_OBJECT) out << " object";
        if ((p.flags & DRM_MODE_PROP_RANGE) ||
            (p.flags & DRM_MODE_PROP_EXTENDED_TYPE) == DRM_MODE_PROP_SIGNED_RANGE) {
            out << " range=[";
            for (std::size_t i = 0; i < p.values.size(); ++i) {
                if (i) out << ", ";
                if ((p.flags & DRM_MODE_PROP_EXTENDED_TYPE) == DRM_MODE_PROP_SIGNED_RANGE) out << std::bit_cast<std::int64_t>(p.values[i]);
                else out << p.values[i];
            }
            out << ']';
        }
        if (!p.enums.empty()) {
            out << ((p.flags & DRM_MODE_PROP_BITMASK) ? " bit-index={" : " enum={");
            for (std::size_t i = 0; i < p.enums.size(); ++i) {
                if (i) out << ", ";
                out << p.enums[i].value << ':' << p.enums[i].name;
            }
            out << '}';
        }
        out << '\n';
    }
}
void print_formats(std::ostream& out, const std::vector<std::uint32_t>& formats) {
    for (auto f : formats) out << ' ' << fourcc_name(f);
    out << '\n';
}
bool has_format(const Plane& p, std::uint32_t format) {
    return std::find(p.formats.begin(), p.formats.end(), format) != p.formats.end();
}
}
void print_report(std::ostream& out, const DeviceSnapshot& d) {
    out << "Device " << d.path << " driver=" << d.driver << '\n'
        << "Query only: no modeset, framebuffer allocation, atomic commit or master request.\n"
        << "Client capabilities: atomic=" << d.atomic << " universal_planes=" << d.universal_planes
        << " writeback_visibility=" << d.writeback_connectors << '\n';
    for (const auto& c : d.capabilities) {
        out << "  " << c.name << '=';
        if (c.available) out << c.value; else out << "unavailable";
        out << '\n';
    }
    for (const auto& warning : d.warnings) out << "WARNING: " << warning << '\n';
    out << "CRTCs (possible_crtcs masks refer to index, not object ID):\n";
    for (const auto& c : d.crtcs) {
        out << "  CRTC " << c.id << " index=" << c.index << '\n';
        print_properties(out, c.properties);
    }
    out << "Encoders:\n";
    for (const auto& e : d.encoders)
        out << "  encoder " << e.id << " current_crtc=" << e.crtc_id
            << " possible_crtcs=0x" << std::hex << e.possible_crtcs << std::dec << '\n';
    out << "Connectors:\n";
    for (const auto& c : d.connectors) {
        out << "  connector " << c.id << ' ' << c.name << ' ' << c.connection
            << " current_encoder=" << c.encoder_id << " possible_crtcs=0x"
            << std::hex << c.possible_crtcs << std::dec << " encoders=";
        for (auto e : c.encoders) out << e << ' ';
        out << '\n';
        for (const auto& m : c.modes)
            out << "    mode " << m.name << ' ' << m.width << 'x' << m.height
                << " @" << m.refresh_hz << "Hz" << (m.preferred ? " preferred" : "") << '\n';
        if (c.writeback) { out << "    writeback formats:"; print_formats(out, c.writeback_formats); }
        print_properties(out, c.properties);
    }
    out << "Planes:\n";
    for (const auto& p : d.planes) {
        out << "  plane " << p.id << " type=" << p.type << " current_crtc=" << p.crtc_id
            << " current_fb=" << p.fb_id << " possible_crtcs=0x" << std::hex << p.possible_crtcs << std::dec << '\n';
        out << "    formats:"; print_formats(out, p.formats);
        if (!p.has_in_formats) out << "    IN_FORMATS unavailable: explicit modifiers unknown (not assumed LINEAR).\n";
        for (const auto& fm : p.format_modifiers) {
            out << "    format/modifier " << fourcc_name(fm.format) << " / 0x"
                << std::hex << std::setw(16) << std::setfill('0') << fm.modifier << std::dec << std::setfill(' ');
            if (fm.modifier == DRM_FORMAT_MOD_LINEAR) out << " LINEAR";
            out << '\n';
        }
        print_properties(out, p.properties);
        for (const char* name : {"zpos", "alpha", "pixel blend mode", "IN_FENCE_FD", "COLOR_ENCODING", "COLOR_RANGE"})
            if (std::none_of(p.properties.begin(), p.properties.end(),
                    [&](const auto& prop) { return prop.name == name; }))
                out << "    " << name << ": not exposed\n";
    }
    out << "NV12 + AR24 non-cursor plane candidates (format/routing only):\n";
    bool found = false;
    for (const auto& crtc : d.crtcs) {
        if (crtc.index >= 32) continue;
        const auto mask = std::uint32_t{1} << crtc.index;
        for (const auto& video : d.planes) for (const auto& ui : d.planes) {
            if (video.id == ui.id || video.type == "Cursor" || ui.type == "Cursor" ||
                !(video.possible_crtcs & ui.possible_crtcs & mask) ||
                !has_format(video, DRM_FORMAT_NV12) || !has_format(ui, DRM_FORMAT_ARGB8888)) continue;
            found = true;
            out << "  CRTC " << crtc.id << ": video=" << video.id << " UI=" << ui.id << " writeback=";
            bool wb = false;
            for (const auto& c : d.connectors) if (c.writeback && (c.possible_crtcs & mask)) {
                out << c.id << ' '; wb = true;
            }
            if (!wb) out << "none";
            out << '\n';
        }
    }
    if (!found) out << "  none\n";
    out << "Candidates still require layer order/blending/modifier checks and atomic TEST_ONLY with real buffers.\n";
}
} // namespace zenith::display
