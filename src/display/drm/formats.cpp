#include <zenith/display/probe.hpp>
#include <drm_fourcc.h>
#include <drm_mode.h>
#include <cstring>
#include <stdexcept>

namespace zenith::display {
namespace {
template<class T> T read(std::span<const std::byte> bytes, std::size_t offset) {
    if (offset > bytes.size() || sizeof(T) > bytes.size() - offset)
        throw std::runtime_error("truncated DRM format blob");
    T value;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}
void check_array(std::span<const std::byte> bytes, std::uint32_t offset,
                 std::uint32_t count, std::size_t size) {
    if (offset > bytes.size() || count > (bytes.size() - offset) / size ||
        (count && offset < sizeof(drm_format_modifier_blob)))
        throw std::runtime_error("invalid DRM format blob array bounds");
}
}
std::string fourcc_name(std::uint32_t value) {
    const bool big_endian = value & DRM_FORMAT_BIG_ENDIAN;
    value &= ~DRM_FORMAT_BIG_ENDIAN;
    std::string result;
    for (unsigned i = 0; i < 4; ++i) {
        const auto c = (value >> (8 * i)) & 0xff;
        result += (c >= 32 && c <= 126) ? static_cast<char>(c) : '?';
    }
    if (big_endian) result += "_BE";
    return result;
}
std::vector<FormatModifier> parse_format_modifiers(std::span<const std::byte> bytes) {
    const auto header = read<drm_format_modifier_blob>(bytes, 0);
    if (header.version != 1 || header.flags != 0)
        throw std::runtime_error("unsupported IN_FORMATS blob version/flags");
    check_array(bytes, header.formats_offset, header.count_formats, sizeof(std::uint32_t));
    check_array(bytes, header.modifiers_offset, header.count_modifiers, sizeof(drm_format_modifier));
    std::vector<FormatModifier> result;
    for (std::size_t i = 0; i < header.count_modifiers; ++i) {
        const auto mod = read<drm_format_modifier>(bytes,
            header.modifiers_offset + i * sizeof(drm_format_modifier));
        for (unsigned bit = 0; bit < 64; ++bit) {
            if (!(mod.formats & (std::uint64_t{1} << bit))) continue;
            const auto index = std::uint64_t{mod.offset} + bit;
            if (index >= header.count_formats)
                throw std::runtime_error("IN_FORMATS modifier index outside format array");
            result.push_back({read<std::uint32_t>(bytes,
                header.formats_offset + index * sizeof(std::uint32_t)), mod.modifier});
        }
    }
    return result;
}
std::vector<std::uint32_t> parse_writeback_formats(std::span<const std::byte> bytes) {
    if (bytes.size() % sizeof(std::uint32_t))
        throw std::runtime_error("invalid WRITEBACK_PIXEL_FORMATS length");
    std::vector<std::uint32_t> result;
    for (std::size_t i = 0; i < bytes.size(); i += sizeof(std::uint32_t))
        result.push_back(read<std::uint32_t>(bytes, i));
    return result;
}
} // namespace zenith::display
