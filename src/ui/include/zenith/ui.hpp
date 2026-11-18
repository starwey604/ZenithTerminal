#pragma once
#include <cstdint>
#include <string_view>
namespace zenith::ui {
inline constexpr float canvas_width = 1280.0f;
inline constexpr float canvas_height = 720.0f;

// Demo control: press inside, then release inside to count an activation.
class InputButton {
public:
    void move(float x, float y);
    void leave() { hovered_ = false; }
    void mouse(bool down);
    void space(bool down);
    void cancel();

    bool hovered() const { return hovered_; }
    bool held() const { return (hovered_ && mouse_down_) || space_down_; }
    std::uint64_t activations() const { return activations_; }

private:
    bool hovered_ = false;
    bool mouse_down_ = false;
    bool space_down_ = false;
    std::uint64_t activations_ = 0;
};

// External ARGB8888 pixels, no ownership and no window. Output is premultiplied.
void draw(void* pixels, int width, int height, int pitch, std::uint64_t frame,
          std::string_view source = {}, const InputButton* button = nullptr);
}
