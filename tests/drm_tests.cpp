#include <zenith/display/probe.hpp>
#include <drm_fourcc.h>
#include <drm_mode.h>
#include <cstring>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>

using namespace zenith::display;
namespace {
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
template<class T> void put(std::vector<std::byte>& bytes, std::size_t offset, const T& value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}
template<class F> void rejects(F action) {
    bool caught = false;
    try { action(); } catch (const std::runtime_error&) { caught = true; }
    require(caught, "malformed blob accepted");
}
void formats() {
    require(fourcc_name(DRM_FORMAT_NV12) == "NV12", "NV12 fourcc");
    require(fourcc_name(DRM_FORMAT_ARGB8888) == "AR24", "ARGB fourcc");
    require(fourcc_name(DRM_FORMAT_ARGB8888 | DRM_FORMAT_BIG_ENDIAN) == "AR24_BE", "BE fourcc");
    drm_format_modifier_blob header{};
    header.version = 1;
    header.count_formats = 66;
    header.formats_offset = sizeof(header);
    header.count_modifiers = 2;
    header.modifiers_offset = header.formats_offset + header.count_formats * 4;
    std::vector<std::byte> blob(header.modifiers_offset + 2 * sizeof(drm_format_modifier));
    put(blob, 0, header);
    put(blob, header.formats_offset + 4, std::uint32_t{DRM_FORMAT_NV12});
    put(blob, header.formats_offset + 64 * 4, std::uint32_t{DRM_FORMAT_ARGB8888});
    drm_format_modifier mod{};
    mod.offset = 1;
    mod.formats = 1 | (std::uint64_t{1} << 63);
    mod.modifier = DRM_FORMAT_MOD_LINEAR;
    put(blob, header.modifiers_offset, mod);
    mod.offset = 64; mod.formats = 1; mod.modifier = 0x123456789abcdef0ULL;
    put(blob, header.modifiers_offset + sizeof(mod), mod);
    const auto parsed = parse_format_modifiers(blob);
    require(parsed.size() == 3, "format/modifier count");
    require(parsed[0].format == DRM_FORMAT_NV12 && parsed[0].modifier == 0, "linear NV12");
    require(parsed[1].format == DRM_FORMAT_ARGB8888, "offset plus bit63");
    require(parsed[2].modifier == mod.modifier, "64 bit modifier");
    rejects([&] { parse_format_modifiers(std::span(blob).first(3)); });
    auto broken = blob;
    auto bad_header = header; bad_header.count_formats = UINT32_MAX; put(broken, 0, bad_header);
    rejects([&] { parse_format_modifiers(broken); });
    bad_header = header; bad_header.formats_offset = 0; put(broken, 0, bad_header);
    rejects([&] { parse_format_modifiers(broken); });
    bad_header = header; bad_header.version = 2; put(broken, 0, bad_header);
    rejects([&] { parse_format_modifiers(broken); });
    broken = blob; mod.offset = UINT32_MAX; put(broken, header.modifiers_offset, mod);
    rejects([&] { parse_format_modifiers(broken); });
    std::vector<std::byte> wb(8);
    put(wb, 0, std::uint32_t{DRM_FORMAT_XRGB8888}); put(wb, 4, std::uint32_t{DRM_FORMAT_ARGB8888});
    require(parse_writeback_formats(wb) == std::vector<std::uint32_t>{DRM_FORMAT_XRGB8888, DRM_FORMAT_ARGB8888}, "writeback formats");
    wb.pop_back(); rejects([&] { parse_writeback_formats(wb); });
}
void report() {
    DeviceSnapshot d; d.path = "fixture"; d.driver = "fixture";
    d.crtcs.push_back({77, 1, {}});
    Plane video; video.id = 10; video.type = "Primary"; video.possible_crtcs = 2; video.formats = {DRM_FORMAT_NV12};
    Plane ui; ui.id = 11; ui.type = "Overlay"; ui.possible_crtcs = 2; ui.formats = {DRM_FORMAT_ARGB8888};
    ui.properties.push_back({5, DRM_MODE_PROP_RANGE | DRM_MODE_PROP_IMMUTABLE, "zpos", 1, {0, 2}, {}});
    ui.properties.push_back({6, DRM_MODE_PROP_SIGNED_RANGE, "IN_FENCE_FD", UINT64_MAX, {UINT64_MAX, INT32_MAX}, {}});
    d.planes = {video, ui};
    Connector wb; wb.id = 99; wb.writeback = true; wb.possible_crtcs = 2; d.connectors.push_back(wb);
    std::ostringstream out; print_report(out, d);
    require(out.str().find("CRTC 77: video=10 UI=11 writeback=99") != std::string::npos, "CRTC mask uses index");
    require(out.str().find("explicit modifiers unknown") != std::string::npos, "missing modifiers not linear");
    require(out.str().find("immutable range=[0, 2]") != std::string::npos, "property range");
    require(out.str().find("IN_FENCE_FD (#6) = -1 range=[-1, 2147483647]") != std::string::npos, "signed range");
    d.planes[1].possible_crtcs = 1;
    std::ostringstream unmatched; print_report(unmatched, d);
    require(unmatched.str().find("video=10 UI=11") == std::string::npos, "disjoint CRTC masks");
    d.planes[1].possible_crtcs = 2; d.planes[1].type = "Cursor";
    std::ostringstream cursor; print_report(cursor, d);
    require(cursor.str().find("video=10 UI=11") == std::string::npos, "cursor excluded");
}
}
void ui_tests();
int main() {
    try { formats(); report(); ui_tests(); std::cout << "DRM UNIT PASS\n"; return 0; }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
