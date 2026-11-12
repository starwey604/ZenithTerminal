#pragma once
#include <cstdint>
namespace zenith::ui {
// External ARGB8888 pixels, no ownership and no window. Output is premultiplied.
void draw(void* pixels, int width, int height, int pitch, std::uint64_t frame);
}
