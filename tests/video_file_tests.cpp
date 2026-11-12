#include <iostream>
#include <stdexcept>
#include <zenith/video_file.hpp>
int main(int argc, char **argv) {
    try {
        if (argc != 2)
            throw std::runtime_error("fixture MP4 required");
        zenith::video::HevcFile f(argv[1], true);
        unsigned keys = 0;
        std::int64_t previous = -1;
        for (unsigned n = 0; n < 1201; ++n) {
            auto a = f.next();
            if (!a || a->pts_us <= previous || a->duration_us < 16666 || a->duration_us > 16667 ||
                a->discontinuity != (n % 1200 == 0))
                throw std::runtime_error("AU timing/loop mismatch");
            bool key = zenith::video::is_random_access(a->bytes);
            if (key != (n % 60 == 0))
                throw std::runtime_error("unexpected IDR spacing");
            keys += key;
            previous = a->pts_us;
        }
        if (previous != 20000000 || f.loops() != 1 || keys != 21)
            throw std::runtime_error("loop timeline mismatch");
        zenith::video::HevcFile once(argv[1], false);
        unsigned count = 0;
        while (once.next())
            ++count;
        if (count != 1200 || once.next())
            throw std::runtime_error("EOF mismatch");
        std::cout << "PASS: 1200 AUs, Annex-B IDRs, monotonic PTS across loop, EOF\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
