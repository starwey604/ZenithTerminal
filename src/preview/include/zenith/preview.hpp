#pragma once
#include <memory>
union SDL_Event;
namespace zenith {
class Preview {
public:
    Preview(int width, int height);
    ~Preview();
    Preview(const Preview&) = delete;
    Preview& operator=(const Preview&) = delete;
    // Pointer event coordinates are in writeback image pixels, not window pixels.
    bool poll_event(SDL_Event& event);
    void show(const void* pixels, int pitch);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
