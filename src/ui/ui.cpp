#include "canvas.hpp"
namespace zenith::ui {
namespace {
constexpr SDL_FRect button_bounds{960, 244, 292, 68};
}
void InputButton::move(float x, float y) {
    hovered_ = x >= button_bounds.x && x < button_bounds.x + button_bounds.w &&
               y >= button_bounds.y && y < button_bounds.y + button_bounds.h;
}
void InputButton::mouse(bool down) {
    if (down) mouse_down_ = hovered_;
    else {
        if (mouse_down_ && hovered_) ++activations_;
        mouse_down_ = false;
    }
}
void InputButton::space(bool down) {
    if (!down && space_down_) ++activations_;
    space_down_ = down;
}
void InputButton::cancel() {
    hovered_ = mouse_down_ = space_down_ = false;
}

void draw(void* pixels, int width, int height, int pitch, std::uint64_t frame,
          std::string_view source, const InputButton* button) {
    detail::Canvas c{pixels, width, height, pitch};
    c.rect(0, 0, 1280, 84, 7, 16, 24, 235);
    c.rect(28, 25, 4, 32, 78, 231, 193, 255);
    c.text(48, 26, "ZENITH / RM2027"); c.text(48, 47, "TERMINAL DISPLAY LAB", 125, 155, 170, 1.5f);
    c.text(811, 28, "VKMS / ATOMIC / WRITEBACK", 78, 231, 193);
    c.text(811, 49, "NV12 VIDEO + PREMULTIPLIED ARGB UI", 125, 155, 170, 1.5f);
    c.rect(28, 112, 320, 190, 7, 16, 24, 198);
    c.text(48, 134, "DISPLAY PIPELINE", 78, 231, 193);
    c.text(48, 166, "01  NV12 SOURCE"); c.text(48, 190, "02  SDL UI");
    c.text(48, 214, "03  KMS COMPOSITE"); c.text(48, 238, "04  WRITEBACK");
    c.text(48, 270, source.empty() ? "GENERATED SOURCE" : std::string(source), 125, 155, 170);
    c.rect(960, 112, 292, 114, 7, 16, 24, 198);
    c.text(980, 134, "FRAME / " + std::to_string(frame), 78, 231, 193);
    c.text(980, 164, source.empty() ? "2 PLANES / 2 SLOTS" : "2 PLANES / NV12"); c.text(980, 190, "LINEAR BUFFERS", 125, 155, 170);
    if (button) {
        const auto [x, y, w, h] = button_bounds;
        const bool held = button->held(), hovered = button->hovered();
        if (held) c.rect(x, y, w, h, 78, 231, 193, 255);
        else if (hovered) c.rect(x, y, w, h, 20, 60, 63, 246);
        else c.rect(x, y, w, h, 7, 16, 24, 230);
        c.rect(x, y, 4, h, held ? 7 : 78, held ? 16 : 231, held ? 24 : 193, hovered || held ? 255 : 150);
        c.text(x + 20, y + 14, "INPUT CHECK", held ? 7 : 221, held ? 16 : 234, held ? 24 : 241);
        c.text(x + 20, y + 40, held ? "RELEASE TO ADD" : "CLICK OR SPACE",
             held ? 7 : 158, held ? 16 : 182, held ? 24 : 195, 1.5f);
        c.text(x + 211, y + 40, "#" + std::to_string(button->activations()),
             held ? 7 : 78, held ? 16 : 231, held ? 24 : 193, 1.5f);
    }
    // Sparse reticle leaves the video plane visible over most of the canvas.
    c.rect(600, 359, 26, 2, 78, 231, 193, 230); c.rect(654, 359, 26, 2, 78, 231, 193, 230);
    c.rect(639, 320, 2, 26, 78, 231, 193, 230); c.rect(639, 374, 2, 26, 78, 231, 193, 230);
    c.rect(635, 355, 10, 10, 78, 231, 193, 140);
    c.rect(28, 526, 1224, 66, 7, 16, 24, 195);
    c.text(48, 546, "LIVE COMPOSITION", 78, 231, 193);
    c.text(48, 568, source.empty() ? "NV12 color bars + moving grid. Captured after the writeback fence." : "HEVC > VAAPI > CPU NV12 staging. Real two-plane writeback.", 158, 182, 195, 1.5f);
    c.rect(28, 614, 1224, 3, 56, 78, 91, 180);
    c.rect(28, 614, static_cast<float>((frame % 180) * 1224 / 180), 3, 78, 231, 193, 255);
    c.text(184, 668, "ALPHA CHECK   0% / 50% / 100%", 210, 225, 233);
    c.text(890, 668, "ESC TO CLOSE", 125, 155, 170);
    c.pixel_coordinates();
    // Exact framebuffer-space swatches are sampled from writeback by integration tests.
    c.rect(16, height - 48.0f, 128, 32, 0, 0, 0, 0);
    c.rect(24, height - 40.0f, 24, 24, 255, 255, 255, 0);
    c.rect(64, height - 40.0f, 24, 24, 255, 255, 255, 128);
    c.rect(104, height - 40.0f, 24, 24, 255, 255, 255, 255);
    c.finish();
}
} // namespace zenith::ui
