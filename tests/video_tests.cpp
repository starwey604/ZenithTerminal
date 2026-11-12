#include <algorithm>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <zenith/video.hpp>
#include <zenith/video_transport.hpp>
using namespace zenith;
void require(bool b, const char *m) {
    if (!b)
        throw std::runtime_error(m);
}
int main() {
    try {
        video::Frame f;
        f.width = 4;
        f.height = 2;
        f.y_pitch = 6;
        f.uv_pitch = 6;
        f.y.assign(12, 64);
        f.uv.assign(6, 128);
        std::vector<std::byte> y(16, std::byte{9}), uv(8, std::byte{9});
        f.copy_to(y.data(), 8, uv.data(), 8, 4, 2);
        require(y[3] == std::byte{64} && y[4] == std::byte{9} && y[12] == std::byte{9} &&
                    uv[4] == std::byte{9},
                "NV12 pitch/padding");
        bool rejected = false;
        try {
            f.copy_to(y.data(), 3, uv.data(), 8, 4, 2);
        } catch (const std::runtime_error &) {
            rejected = true;
        }
        require(rejected, "short pitch rejected");
        require(video::is_random_access(std::vector<std::uint8_t>{0, 0, 0, 1, 40, 1, 0}), "HEVC IDR");
        require(!video::is_random_access(std::vector<std::uint8_t>{0, 0, 1, 2, 1, 0}), "HEVC P");
        video::AccessUnit au;
        au.bytes.resize(3000);
        for (unsigned i = 0; i < au.bytes.size(); ++i)
            au.bytes[i] = i % 251;
        auto packets = [&](std::uint16_t n) {
            std::vector<std::vector<std::uint8_t>> p;
            backend::fragment(au, n, [&](auto b) { p.emplace_back(b.begin(), b.end()); });
            return p;
        };
        auto p = packets(65535);
        require(p.size() == 3 && p[0].size() == 1400 && p[0][0] == 255 && p[0][1] == 255 && p[0][6] == 11 &&
                    p[0][7] == 184,
                "BUAA big-endian golden header");
        backend::Reassembler r;
        require(!r.accept(p[1], 0), "partial");
        require(!r.accept(p[1], 1), "duplicate");
        require(!r.accept(p[2], 2), "reordered partial");
        auto a = r.accept(p[0], 3);
        require(a && a->bytes == au.bytes && a->discontinuity, "reassemble reordered AU");
        require(r.stats().duplicates == 1, "duplicate count");
        require(!r.accept(p[0], 4), "old packet suppressed");
        p = packets(0);
        r.accept(p[0], 5);
        r.accept(p[1], 6);
        a = r.accept(p[2], 7);
        require(a && !a->discontinuity, "16-bit frame wrap");
        p = packets(2);
        r.accept(p[0], 8);
        r.accept(p[1], 9);
        a = r.accept(p[2], 10);
        require(a && a->discontinuity && r.stats().gaps == 1, "missing frame resets chain");
        p = packets(3);
        r.accept(p[0], 11);
        r.expire(400);
        require(r.stats().expired == 1, "incomplete expiry");
        p = packets(1);
        r.accept(p[0], 401);
        r.accept(p[1], 402);
        a = r.accept(p[2], 403);
        require(a && a->discontinuity, "sender restart after idle");
        backend::Reassembler bad;
        p = packets(1);
        p[2][3] = 3;
        bad.accept(p[0], 0);
        bad.accept(p[1], 1);
        require(!bad.accept(p[2], 2) && bad.stats().invalid == 1, "fragment index holes rejected");
        p = packets(4);
        bad.accept(p[0], 3);
        p[0][9] ^= 1;
        require(!bad.accept(p[0], 4) && bad.stats().invalid == 2, "conflicting duplicate rejected");
        std::vector<std::uint8_t> short_packet(8);
        bad.accept(short_packet, 5);
        require(bad.stats().invalid == 3, "empty payload rejected");
        p = packets(5);
        p[0][4] = p[0][5] = p[0][6] = p[0][7] = 0;
        require(!bad.accept(p[0], 6) && bad.stats().invalid == 4, "zero AU length rejected");
        p = packets(6);
        bad.accept(p[0], 7);
        p[1][7] ^= 1;
        require(!bad.accept(p[1], 8) && bad.stats().invalid == 5, "inconsistent AU length rejected");
        std::cout << "PASS: NV12 ownership/pitches, HEVC keyframes, BUAA wire format, "
                     "reorder/duplicate/loss/wrap/bounds\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
