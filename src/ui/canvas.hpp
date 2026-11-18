#pragma once

#include <zenith/ui.hpp>
#include <SDL3/SDL.h>
#include <memory>
#include <stdexcept>
#include <string>

namespace zenith::ui::detail {
inline void ok(bool success) {
    if (!success) throw std::runtime_error(SDL_GetError());
}

// Wraps a mapped ARGB buffer; all translucent colors are premultiplied
// before being written. No SDL window and no ownership of DRM pixels.
class Canvas {
public:
    Canvas(void* pixels, int width, int height, int pitch)
        : surface_(SDL_CreateSurfaceFrom(width, height, SDL_PIXELFORMAT_ARGB8888, pixels, pitch), SDL_DestroySurface),
          renderer_(surface_ ? SDL_CreateSoftwareRenderer(surface_.get()) : nullptr, SDL_DestroyRenderer),
          sx_(width / canvas_width), sy_(height / canvas_height) {
        ok(renderer_ != nullptr);
        ok(SDL_SetRenderDrawBlendMode(renderer_.get(), SDL_BLENDMODE_NONE));
        ok(SDL_SetRenderDrawColor(renderer_.get(), 0, 0, 0, 0));
        ok(SDL_RenderClear(renderer_.get()));
        ok(SDL_SetRenderScale(renderer_.get(), sx_, sy_));
    }

    void rect(float x, float y, float w, float h, Uint8 red, Uint8 green, Uint8 blue, Uint8 alpha) {
        auto* r = renderer_.get();
        ok(SDL_SetRenderDrawColor(r, (red * alpha + 127) / 255, (green * alpha + 127) / 255,
                                  (blue * alpha + 127) / 255, alpha));
        SDL_FRect area{x, y, w, h};
        ok(SDL_RenderFillRect(r, &area));
    }

    void text(float x, float y, const std::string& value, Uint8 red = 221, Uint8 green = 234,
              Uint8 blue = 241, float scale = 2.0f) {
        auto* r = renderer_.get();
        ok(SDL_SetRenderScale(r, sx_ * scale, sy_ * scale));
        ok(SDL_SetRenderDrawColor(r, red, green, blue, 255));
        ok(SDL_RenderDebugText(r, x / scale, y / scale, value.c_str()));
        ok(SDL_SetRenderScale(r, sx_, sy_));
    }

    void pixel_coordinates() { ok(SDL_SetRenderScale(renderer_.get(), 1, 1)); }
    void finish() { ok(SDL_RenderPresent(renderer_.get())); }

private:
    std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> surface_;
    std::unique_ptr<SDL_Renderer, decltype(&SDL_DestroyRenderer)> renderer_;
    float sx_, sy_;
};
} // namespace zenith::ui::detail
