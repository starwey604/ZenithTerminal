#include <cstring>
#include <stdexcept>
#include <zenith/video.hpp>
namespace zenith::video {
bool is_random_access(std::span<const std::uint8_t> b) {
    for (std::size_t i = 0; i + 5 <= b.size(); ++i) {
        std::size_t n = 0;
        if (b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1)
            n = i + 3;
        else if (i + 6 <= b.size() && b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 0 && b[i + 3] == 1)
            n = i + 4;
        if (n && n + 1 < b.size() && !(b[n] & 0x80) && (b[n + 1] & 7)) {
            const auto type = (b[n] >> 1) & 63;
            if (type >= 16 && type <= 21)
                return true;
        }
    }
    return false;
}
void Frame::copy_to(std::byte *yo, std::size_t ys, std::byte *uvo, std::size_t uvs, int w, int h) const {
    if (w != width || h != height || w <= 0 || h <= 0 || w % 2 || h % 2 || !yo || !uvo ||
        ys < static_cast<unsigned>(w) || uvs < static_cast<unsigned>(w) || y_pitch < w || uv_pitch < w ||
        y.size() < static_cast<std::size_t>(y_pitch) * h ||
        uv.size() < static_cast<std::size_t>(uv_pitch) * (h / 2))
        throw std::runtime_error("invalid NV12 frame/copy dimensions or pitches");
    for (int row = 0; row < h; ++row)
        std::memcpy(yo + row * ys, y.data() + row * y_pitch, w);
    for (int row = 0; row < h / 2; ++row)
        std::memcpy(uvo + row * uvs, uv.data() + row * uv_pitch, w);
}
} // namespace zenith::video
