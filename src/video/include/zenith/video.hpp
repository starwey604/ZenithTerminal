#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace zenith::video {
enum class Matrix { bt601, bt709, bt2020 };
enum class Range { limited, full };
struct Color {
    Matrix matrix{Matrix::bt601};
    Range range{Range::full};
    bool operator==(const Color &) const = default;
};
// Complete HEVC Annex-B access unit. Owns bytes, including parameter sets in-band.
struct AccessUnit {
    std::vector<std::uint8_t> bytes;
    std::int64_t pts_us{}, duration_us{16667};
    bool discontinuity{};
};
bool is_random_access(std::span<const std::uint8_t> annexb);
// First desktop path: owned, linear CPU NV12 after an explicit VAAPI download.
// No AVFrame/VA object escapes the decoder. DRM copies into its own scanout slots.
struct Frame {
    int width{}, height{}, y_pitch{}, uv_pitch{};
    Color color;
    std::int64_t pts_us{};
    std::vector<std::uint8_t> y, uv;
    void copy_to(std::byte *y_out, std::size_t y_stride, std::byte *uv_out, std::size_t uv_stride, int w,
                 int h) const;
};
class Decoder {
  public:
    virtual ~Decoder() = default;
    virtual std::vector<std::shared_ptr<const Frame>> push(const AccessUnit &) = 0;
    virtual std::vector<std::shared_ptr<const Frame>> drain() = 0;
    virtual void reset() = 0;
};
} // namespace zenith::video
