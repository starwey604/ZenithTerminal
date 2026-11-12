#include <zenith/ui.hpp>
#include <SDL3/SDL.h>
#include <memory>
#include <stdexcept>
#include <string>
#include <cstring>
namespace zenith::ui {
namespace {
void ok(bool success) { if (!success) throw std::runtime_error(SDL_GetError()); }
}
void draw(void* pixels, int width, int height, int pitch, std::uint64_t frame) {
    std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> surface(
        SDL_CreateSurfaceFrom(width, height, SDL_PIXELFORMAT_ARGB8888, pixels, pitch), SDL_DestroySurface);
    if (!surface) throw std::runtime_error(SDL_GetError());
    std::unique_ptr<SDL_Renderer, decltype(&SDL_DestroyRenderer)> renderer(SDL_CreateSoftwareRenderer(surface.get()), SDL_DestroyRenderer);
    if (!renderer) throw std::runtime_error(SDL_GetError());
    auto* r = renderer.get();
    ok(SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE));
    ok(SDL_SetRenderDrawColor(r, 0, 0, 0, 0)); ok(SDL_RenderClear(r));
    // Layout uses a 1280x720 design canvas; the mapped target remains the DRM buffer.
    ok(SDL_SetRenderScale(r, width / 1280.0f, height / 720.0f));
    auto rect = [&](float x, float y, float w, float h, Uint8 red, Uint8 green, Uint8 blue, Uint8 alpha) {
        ok(SDL_SetRenderDrawColor(r, red, green, blue, alpha));
        SDL_FRect area{x, y, w, h}; ok(SDL_RenderFillRect(r, &area));
    };
    auto text = [&](float x, float y, const std::string& value, Uint8 red = 221, Uint8 green = 234, Uint8 blue = 241, float scale = 2.0f) {
        ok(SDL_SetRenderScale(r, width / 1280.0f * scale, height / 720.0f * scale));
        ok(SDL_SetRenderDrawColor(r, red, green, blue, 255)); ok(SDL_RenderDebugText(r, x / scale, y / scale, value.c_str()));
        ok(SDL_SetRenderScale(r, width / 1280.0f, height / 720.0f));
    };
    rect(0, 0, 1280, 84, 7, 16, 24, 235);
    rect(28, 25, 4, 32, 78, 231, 193, 255);
    text(48, 26, "ZENITH / RM2027"); text(48, 47, "TERMINAL DISPLAY LAB", 125, 155, 170, 1.5f);
    text(811, 28, "VKMS / ATOMIC / WRITEBACK", 78, 231, 193);
    text(811, 49, "NV12 VIDEO + PREMULTIPLIED ARGB UI", 125, 155, 170, 1.5f);
    rect(28, 112, 320, 190, 7, 16, 24, 198);
    text(48, 134, "DISPLAY PIPELINE", 78, 231, 193);
    text(48, 166, "01  NV12 SOURCE"); text(48, 190, "02  SDL UI");
    text(48, 214, "03  KMS COMPOSITE"); text(48, 238, "04  WRITEBACK");
    text(48, 270, "GENERATED SOURCE", 125, 155, 170);
    rect(960, 112, 292, 114, 7, 16, 24, 198);
    text(980, 134, "FRAME / " + std::to_string(frame), 78, 231, 193);
    text(980, 164, "2 PLANES / 2 SLOTS"); text(980, 190, "LINEAR BUFFERS", 125, 155, 170);
    // Sparse reticle leaves the video plane visible over most of the canvas.
    rect(600, 359, 26, 2, 78, 231, 193, 230); rect(654, 359, 26, 2, 78, 231, 193, 230);
    rect(639, 320, 2, 26, 78, 231, 193, 230); rect(639, 374, 2, 26, 78, 231, 193, 230);
    rect(635, 355, 10, 10, 78, 231, 193, 140);
    rect(28, 526, 1224, 66, 7, 16, 24, 195);
    text(48, 546, "LIVE COMPOSITION", 78, 231, 193);
    text(48, 568, "NV12 color bars + moving grid. Captured after the writeback fence.", 158, 182, 195, 1.5f);
    rect(28, 614, 1224, 3, 56, 78, 91, 180);
    rect(28, 614, static_cast<float>((frame % 180) * 1224 / 180), 3, 78, 231, 193, 255);
    text(184, 668, "ALPHA CHECK   0% / 50% / 100%", 210, 225, 233);
    text(890, 668, "ESC TO CLOSE", 125, 155, 170);
    ok(SDL_SetRenderScale(r, 1, 1));
    // Exact framebuffer-space swatches are sampled from writeback by integration tests.
    rect(16, height - 48.0f, 128, 32, 0, 0, 0, 0);
    rect(24, height - 40.0f, 24, 24, 255, 255, 255, 0);
    rect(64, height - 40.0f, 24, 24, 255, 255, 255, 128);
    rect(104, height - 40.0f, 24, 24, 255, 255, 255, 255);
    ok(SDL_RenderPresent(r));
    // KMS default blending is premultiplied; SDL software draw above writes straight RGB.
    for (int y = 0; y < height; ++y) {
        auto* row = static_cast<std::uint8_t*>(pixels) + y * pitch;
        for (int x = 0; x < width; ++x) {
            std::uint32_t p; std::memcpy(&p, row + 4*x, 4);
            const auto a = p >> 24;
            p = (a << 24) | (((((p >> 16) & 255) * a + 127) / 255) << 16) |
                (((((p >> 8) & 255) * a + 127) / 255) << 8) | (((p & 255) * a + 127) / 255);
            std::memcpy(row + 4*x, &p, 4);
        }
    }
}
} // namespace zenith::ui
