#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace zenith::display {
struct MappedImage {
    std::byte* pixels{};
    int width{}, height{};
    std::uint32_t pitch{}, uv_offset{};
};
enum class VideoFormat { nv12, xrgb8888 };
struct DisplayOptions {
    VideoFormat video_format{VideoFormat::nv12};
    bool writeback{true};
    bool full_state_each_frame{true};
};
struct FrameTargets { unsigned slot{}; MappedImage video, ui_argb; };
struct PresentTiming { double request_ms{}, commit_ms{}, writeback_ms{}; };
struct CapturedImage { const std::byte* pixels{}; int width{}, height{}; std::uint32_t pitch{}; };

// One owner for master, KMS state, buffers and writeback fences. Requires an idle
// CRTC (or explicit takeover) and returns committed state to idle. No SDL dependency or driver-specific commit path.
class AtomicDisplay {
public:
    AtomicDisplay(const std::string& device, int width, int height, bool take_over = false, DisplayOptions options = {});
    ~AtomicDisplay();
    AtomicDisplay(const AtomicDisplay&) = delete;
    AtomicDisplay& operator=(const AtomicDisplay&) = delete;
    // Writable until submitted; do not retain writable views after submit()/present().
    // After submit() returns, the old scanning slot is writable even while writeback runs.
    FrameTargets next_frame();
    void test_only(unsigned slot);
    // Blocks for commit and, when enabled, writeback. No writeback returns an empty
    // capture. A captured image remains valid until this slot is reused.
    CapturedImage present(unsigned slot, PresentTiming* timing = nullptr);
    // Split present for overlap: blocking commit releases the old scanning slot.
    // Only one submission may be outstanding. finish() waits for its writeback.
    // Prepare next_frame() between these calls; never touch the submitted slot.
    void submit(unsigned slot, PresentTiming* timing = nullptr);
    CapturedImage finish(PresentTiming* timing = nullptr);
    void shutdown();
    std::string description() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace zenith::display
