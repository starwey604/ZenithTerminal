#include <zenith/preview.hpp>
#include <SDL3/SDL.h>
#include <stdexcept>
#include <iostream>
namespace zenith {
namespace { void ok(bool success) { if (!success) throw std::runtime_error(SDL_GetError()); } }
struct Preview::Impl {
    SDL_Window* window{}; SDL_Renderer* renderer{}; SDL_Texture* texture{};
    ~Impl() { SDL_DestroyTexture(texture); SDL_DestroyRenderer(renderer); SDL_DestroyWindow(window); SDL_QuitSubSystem(SDL_INIT_VIDEO); }
};
Preview::Preview(int width, int height) : impl_(std::make_unique<Impl>()) {
    ok(SDL_InitSubSystem(SDL_INIT_VIDEO));
    impl_->window = SDL_CreateWindow("Zenith / VKMS writeback - NV12 + SDL UI", width, height, SDL_WINDOW_RESIZABLE);
    if (!impl_->window) throw std::runtime_error(SDL_GetError());
    impl_->renderer = SDL_CreateRenderer(impl_->window, nullptr);
    if (!impl_->renderer) throw std::runtime_error(SDL_GetError());
    std::cout << "WRITEBACK PREVIEW driver=" << SDL_GetCurrentVideoDriver() << " renderer=" << SDL_GetRendererName(impl_->renderer) << '\n';
    ok(SDL_SetRenderLogicalPresentation(impl_->renderer, width, height, SDL_LOGICAL_PRESENTATION_LETTERBOX));
    impl_->texture = SDL_CreateTexture(impl_->renderer, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, width, height);
    if (!impl_->texture) throw std::runtime_error(SDL_GetError());
    ok(SDL_SetTextureBlendMode(impl_->texture, SDL_BLENDMODE_NONE));
}
Preview::~Preview() = default;
bool Preview::poll_event(SDL_Event& event) {
    if (!SDL_PollEvent(&event)) return false;
    if (event.type == SDL_EVENT_MOUSE_MOTION || event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
        event.type == SDL_EVENT_MOUSE_BUTTON_UP)
        ok(SDL_ConvertEventToRenderCoordinates(impl_->renderer, &event));
    return true;
}
void Preview::show(const void* pixels, int pitch) {
    ok(SDL_UpdateTexture(impl_->texture, nullptr, pixels, pitch));
    ok(SDL_SetRenderDrawColor(impl_->renderer, 4, 8, 12, 255)); ok(SDL_RenderClear(impl_->renderer));
    ok(SDL_RenderTexture(impl_->renderer, impl_->texture, nullptr, nullptr));
    ok(SDL_RenderPresent(impl_->renderer));
}
} // namespace zenith
