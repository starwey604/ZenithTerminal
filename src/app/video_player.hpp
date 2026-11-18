#pragma once
#include <chrono>
#include <string>
#include <zenith/video.hpp>
#include <zenith/video_transport.hpp>
namespace zenith {
struct VideoSnapshot {
    std::shared_ptr<const video::Frame> frame;
    std::chrono::steady_clock::time_point frame_at{};
    std::uint64_t decoded{};
    unsigned loops{};
    bool finished{};
    backend::TransportStats transport;
};
// Decode/pacing never owns DRM or SDL. Latest *decoded* frame may be replaced;
// compressed reference frames are processed in order (or reset on transport gaps).
class VideoPlayer {
  public:
    VideoPlayer(std::string file, bool loop, std::string bind_address, unsigned port, std::string va_device);
    ~VideoPlayer();
    VideoSnapshot snapshot();
    VideoSnapshot wait_first();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace zenith
