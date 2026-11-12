#include <zenith/display/atomic_display.hpp>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace zenith::display;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void rejects(F action, const char* expected) {
    try { action(); }
    catch (const std::runtime_error& e) {
        require(std::string(e.what()).find(expected) != std::string::npos, e.what());
        return;
    }
    throw std::runtime_error("expected lifecycle rejection");
}
void fill(FrameTargets target, unsigned gray) {
    for (int y = 0; y < target.video.height; ++y)
        std::memset(target.video.pixels + y*target.video.pitch, gray, target.video.width);
    for (int y = 0; y < target.video.height/2; ++y)
        std::memset(target.video.pixels + target.video.uv_offset + y*target.video.pitch, 128, target.video.width);
    for (int y = 0; y < target.ui_argb.height; ++y)
        std::memset(target.ui_argb.pixels + y*target.ui_argb.pitch, 0, target.ui_argb.width*4);
}
void verify(CapturedImage image, unsigned gray) {
    require(image.pixels != nullptr, "missing capture");
    for (int y = 0; y < image.height; ++y) for (int x = 0; x < image.width; ++x) {
        unsigned pixel;
        std::memcpy(&pixel, image.pixels + y*image.pitch + x*4, 4);
        for (int shift : {0, 8, 16})
            require(((pixel >> shift) & 255) == gray, "captured wrong slot/frame");
    }
}
int main(int argc, char** argv) {
    if ((argc != 2 && argc != 3) || std::string(argv[1]) == "--help") {
        std::cout << "zenith_drm_lifecycle_test /dev/dri/cardN [--take-over]\n"
                     "Requires an idle, released VKMS device; changes its KMS state.\n";
        return argc == 2 ? 0 : 2;
    }
    try {
        require(argc != 3 || std::string(argv[2]) == "--take-over", "unknown option");
        for (bool writeback : {true, false}) {
            DisplayOptions options; options.writeback = writeback;
            AtomicDisplay display(argv[1], 640, 480, argc == 3, options);
            rejects([&] { display.finish(); }, "no pending");
            rejects([&] { display.submit(2); }, "cannot submit");
            auto a = display.next_frame(); fill(a, 80); display.test_only(a.slot);
            display.submit(a.slot);
            auto b = display.next_frame(); require(a.slot != b.slot, "scanning slot exposed");
            rejects([&] { display.submit(b.slot); }, "still pending");
            rejects([&] { display.test_only(b.slot); }, "still pending");
            fill(b, 120); // Must not change the image whose writeback is in flight.
            auto image = display.finish();
            if (writeback) verify(image, 80); else require(!image.pixels, "unexpected capture");
            rejects([&] { display.finish(); }, "no pending");
            rejects([&] { display.submit(a.slot); }, "cannot submit");
            image = display.present(b.slot); // Existing synchronous API still works.
            if (writeback) verify(image, 120);
            a = display.next_frame(); fill(a, 100); display.submit(a.slot);
            display.shutdown(); // Drains pending writeback, then detaches.
            display.shutdown();
            a = display.next_frame(); fill(a, 90);
            image = display.present(a.slot); // Reuse after explicit shutdown.
            if (writeback) verify(image, 90);
            b = display.next_frame(); fill(b, 110); display.submit(b.slot);
            // Destructor must drain a pending frame too.
        }
        std::cout << "PASS: split/synchronous submit, slot ownership, pending guards, shutdown/reuse/destructor\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
