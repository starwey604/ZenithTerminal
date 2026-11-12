#include <charconv>
#include <chrono>
#include <csignal>
#include <iostream>
#include <thread>
#include <zenith/video_file.hpp>
#include <zenith/video_transport.hpp>
namespace {
volatile std::sig_atomic_t stopped = 0;
void stop(int) { stopped = 1; }
unsigned number(const std::string &s) {
    unsigned n{};
    auto [p, e] = std::from_chars(s.data(), s.data() + s.size(), n);
    if (e != std::errc{} || p != s.data() + s.size())
        throw std::runtime_error("invalid number: " + s);
    return n;
}
} // namespace
int main(int argc, char **argv) {
    try {
        std::string file, host = "127.0.0.1";
        unsigned port = 3334, loops = 0;
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--help") {
                std::cout << "zenith_video_replay --file HEVC.mp4 [--host 127.0.0.1] [--port 3334] [--loops "
                             "N]\nN=0 loops until SIGINT. Uses BUAA/RM2027 headers, not RTP/MPEG-TS.\n";
                return 0;
            }
            if (i + 1 >= argc)
                throw std::runtime_error("missing value: " + arg);
            std::string v = argv[++i];
            if (arg == "--file")
                file = v;
            else if (arg == "--host")
                host = v;
            else if (arg == "--port")
                port = number(v);
            else if (arg == "--loops")
                loops = number(v);
            else
                throw std::runtime_error("unknown option: " + arg);
        }
        if (file.empty() || port == 0 || port > 65535)
            throw std::runtime_error("--file and valid UDP port required");
        zenith::video::HevcFile source(file, true);
        zenith::backend::UdpSender tx(host, port);
        std::signal(SIGINT, stop);
        std::signal(SIGTERM, stop);
        auto start = std::chrono::steady_clock::now();
        std::uint64_t frames = 0, bytes = 0;
        while (!stopped) {
            auto au = source.next();
            if (loops && source.loops() >= loops)
                break;
            auto deadline = start + std::chrono::microseconds(au->pts_us);
            // Long PTS gaps remain interruptible; normal fixture spacing is 16.67 ms.
            while (!stopped && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_until(
                    std::min(deadline, std::chrono::steady_clock::now() + std::chrono::milliseconds(20)));
            if (stopped)
                break;
            tx.send(*au);
            ++frames;
            bytes += au->bytes.size();
        }
        std::cout << "REPLAY COMPLETE frames=" << frames << " bytes=" << bytes << " loops=" << source.loops()
                  << '\n';
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "REPLAY FAIL: " << e.what() << '\n';
        return 1;
    }
}
