#include "../src/decoder/vaapi/vaapi.hpp"
#include <iostream>
#include <stdexcept>
#include <zenith/video_file.hpp>
int main(int argc, char **argv) {
    try {
        if (argc != 3)
            throw std::runtime_error("usage: zenith_video_decode_test fixture.mp4 /dev/dri/renderD128");
        zenith::video::HevcFile f(argv[1], true);
        auto d = zenith::video::make_vaapi_decoder(argv[2]);
        std::shared_ptr<const zenith::video::Frame> first;
        unsigned count = 0;
        for (unsigned i = 0; i < 1201; ++i) {
            auto a = f.next();
            auto frames = d->push(*a);
            for (auto &frame : frames) {
                if (frame->width != 1920 || frame->height != 1080 ||
                    frame->color !=
                        zenith::video::Color{zenith::video::Matrix::bt709, zenith::video::Range::limited})
                    throw std::runtime_error("dimensions/color metadata");
                if (frame->y[500 * 1920] != 16 || frame->uv[250 * 1920] != 128)
                    throw std::runtime_error("NV12 black border levels");
                if (!first)
                    first = frame;
                if (i == 1200 && (frame->y != first->y || frame->uv != first->uv))
                    throw std::runtime_error("loop pixels or owned frame changed");
                ++count;
            }
        }
        count += d->drain().size();
        if (count != 1201)
            throw std::runtime_error("decode frame count mismatch");
        d->reset();
        zenith::video::AccessUnit delta;
        delta.bytes = {0, 0, 1, 2, 1, 0};
        if (!d->push(delta).empty())
            throw std::runtime_error("must wait for random access after reset");
        std::cout
            << "PASS: VAAPI 1201 frames across loop, owned NV12 lifetime, BT.709 limited, drain/reset\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
