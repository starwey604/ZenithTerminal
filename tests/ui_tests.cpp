#include <zenith/ui.hpp>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>
void ui_tests() {
    constexpr int width=1280, height=720, pitch=width*4+64;
    std::vector<std::uint8_t> pixels(pitch*height, 0xa5);
    zenith::ui::draw(pixels.data(), width, height, pitch, 42);
    const int x[]{36,76,116};
    const std::uint32_t expected[]{0x00000000,0x80808080,0xffffffff};
    for (int i=0;i<3;++i) {
        std::uint32_t p; std::memcpy(&p, pixels.data()+(height-28)*pitch+x[i]*4,4);
        if (p!=expected[i]) throw std::runtime_error("UI premultiplied alpha swatch mismatch");
    }
    for (int y=0;y<height;++y)
        for (int x=width*4;x<pitch;++x)
            if (pixels[y*pitch+x]!=0xa5) throw std::runtime_error("UI overwrote pitch padding");
}
