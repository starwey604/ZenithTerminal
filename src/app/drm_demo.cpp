#include "drm_demo.hpp"
#include <zenith/display/atomic_display.hpp>
#include <zenith/display/probe.hpp>
#include <zenith/ui.hpp>
#include <SDL3/SDL.h>
#ifdef ZT_ENABLE_PREVIEW
#include <zenith/preview.hpp>
#endif
#include <array>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
#include <iomanip>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
volatile std::sig_atomic_t interrupted = 0;
void stop(int) { interrupted = 1; }
int count(const std::string& value) {
    int result{}; auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size() || result < 0) throw std::runtime_error("invalid frame count");
    return result;
}
void fill_video(const zenith::display::MappedImage& image, unsigned frame) {
    auto* yplane = reinterpret_cast<std::uint8_t*>(image.pixels);
    auto* uvplane = yplane + image.uv_offset;
    struct Yuv { std::uint8_t y, u, v; };
    const auto convert = [](int r, int g, int b) -> Yuv {
        return {static_cast<std::uint8_t>((77*r + 150*g + 29*b + 128) >> 8),
            static_cast<std::uint8_t>(((-43*r - 85*g + 128*b + 128) >> 8) + 128),
            static_cast<std::uint8_t>(((128*r - 107*g - 21*b + 128) >> 8) + 128)};
    };
    constexpr std::array<std::array<int, 3>, 8> rgb{{{185,198,208},{180,163,62},{55,174,178},{65,157,110},
        {144,92,163},{182,78,81},{65,106,174},{28,39,51}}};
    std::array<Yuv, 8> bars;
    for (unsigned i = 0; i < bars.size(); ++i) bars[i] = convert(rgb[i][0],rgb[i][1],rgb[i][2]);
    // The original picture is constant within each 2x2 block. Hoist color
    // conversion and bar division out of the pixel loop, without caching frames.
    std::vector<Yuv> columns(image.width / 2);
    for (int x = 0; x < image.width; x += 2) columns[x/2] = bars[std::min(7,x*8/image.width)];
    const Yuv gray{static_cast<std::uint8_t>(80 + frame%64),128,128};
    for (int y = 0; y < image.height; y += 2) {
        const bool upper = y < image.height * 3 / 4;
        const int r = 19+y*15/image.height, g = 40+y*25/image.height, b = 57+y*30/image.height;
        const auto base = convert(r,g,b), grid = convert(r+24,g+36,b+39);
        for (int x = 0; x < image.width; x += 2) {
            Yuv c = upper ? (((x + static_cast<int>(frame*2)) % 128 < 2 || y%80 < 2) ? grid : base) : columns[x/2];
            if (x < 160 && y >= image.height - 64) c = gray;
            const std::uint16_t yy = c.y * 257;
            std::memcpy(yplane+y*image.pitch+x, &yy, 2);
            std::memcpy(yplane+(y+1)*image.pitch+x, &yy, 2);
            uvplane[(y/2)*image.pitch+x] = c.u; uvplane[(y/2)*image.pitch+x+1] = c.v;
        }
    }
}
void fill_source(const zenith::display::MappedImage& image, unsigned frame, zenith::display::VideoFormat format) {
    if (format == zenith::display::VideoFormat::nv12) { fill_video(image, frame); return; }
    // Diagnostic RGB source preserves geometry and changing gray alpha-check region.
    for (int y = 0; y < image.height; ++y) for (int x = 0; x < image.width; ++x) {
        std::uint32_t pixel = 0xff193950;
        if ((x + frame*2) % 128 < 2 || y % 80 < 2) pixel = 0xff315d77;
        if (x < 160 && y >= image.height - 64) { const auto gray = 80 + frame%64; pixel = 0xff000000 | gray*0x010101; }
        std::memcpy(image.pixels + y*image.pitch + x*4, &pixel, 4);
    }
}
void verify(const zenith::display::CapturedImage& image, unsigned frame) {
    const int base = 80 + frame % 64;
    const std::array<int, 3> expected{base, 128 + (base*127 + 127)/255, 255};
    const std::array<int, 3> x{36, 76, 116};
    for (unsigned i = 0; i < 3; ++i) {
        std::uint32_t pixel;
        std::memcpy(&pixel, image.pixels + (image.height-28)*image.pitch + x[i]*4, 4);
        for (int shift : {0,8,16}) {
            const int actual = (pixel >> shift) & 255;
            if (std::abs(actual - expected[i]) > 3)
                throw std::runtime_error("writeback alpha sample " + std::to_string(i) + " frame=" + std::to_string(frame) +
                    " expected=" + std::to_string(expected[i]) + " got=" + std::to_string(actual));
        }
    }
}
void save(const std::string& path, const zenith::display::CapturedImage& image) {
    auto* surface = SDL_CreateSurfaceFrom(image.width, image.height, SDL_PIXELFORMAT_XRGB8888,
        const_cast<std::byte*>(image.pixels), image.pitch);
    if (!surface) throw std::runtime_error(SDL_GetError());
    const bool ok = SDL_SaveBMP(surface, path.c_str()); SDL_DestroySurface(surface);
    if (!ok) throw std::runtime_error(SDL_GetError());
}
}
int run_drm_demo(int argc, char** argv) {
    try {
        bool test_only = false, preview = false, take_over = false, uncapped = false, pipeline = false;
#ifdef ZT_ENABLE_PREVIEW
        preview = true;
#endif
        zenith::display::DisplayOptions options;
        int frames = 300, warmup = 10, width = 1280, height = 720;
        std::string device, capture, profile;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--pipeline") pipeline = true;
            else if (arg == "--test-only") test_only = true;
            else if (arg == "--uncapped") uncapped = true;
            else if (arg == "--take-over") take_over = true;
            else if (arg == "--no-writeback") options.writeback = false;
            else if (arg == "--no-preview") preview = false;
            else if ((arg == "--size" || arg == "--device" || arg == "--frames" || arg == "--capture" || arg == "--profile" || arg == "--warmup" || arg == "--video-format" || arg == "--state") && i + 1 < argc) {
                const std::string value = argv[++i];
                if (arg == "--size") {
                    const auto split = value.find('x');
                    if (split == std::string::npos) throw std::runtime_error("size must be WIDTHxHEIGHT");
                    width = count(value.substr(0, split)); height = count(value.substr(split + 1));
                    if (width < 320 || height < 240 || width > 4096 || height > 2160 || width % 2 || height % 2)
                        throw std::runtime_error("size must be even and within 320x240..4096x2160");
                } else if (arg == "--device") device = value;
                else if (arg == "--frames") frames = count(value);
                else if (arg == "--capture") capture = value;
                else if (arg == "--profile") profile = value;
                else if (arg == "--warmup") warmup = count(value);
                else if (arg == "--video-format") {
                    if (value != "nv12" && value != "xrgb") throw std::runtime_error("video format must be nv12 or xrgb");
                    options.video_format = value == "nv12" ? zenith::display::VideoFormat::nv12 : zenith::display::VideoFormat::xrgb8888;
                } else {
                    if (value != "full" && value != "delta") throw std::runtime_error("state must be full or delta");
                    options.full_state_each_frame = value == "full";
                }
            } else if (arg == "--help") {
                std::cout << "ZenithTerminal --drm-demo [--size WIDTHxHEIGHT] [--pipeline] [--test-only] [--take-over] [--no-preview] [--frames N] [--capture out.bmp] [--device /dev/dri/cardN] [--profile out.csv] [--warmup N] [--uncapped] [--video-format nv12|xrgb] [--no-writeback] [--state full|delta]\n"
                    "Default: VKMS, 1280x720, 300 frames; N=0 runs until Escape/SIGINT. Requires DRM master; --take-over permits replacing inherited active state.\n";
                return 0;
            } else throw std::runtime_error("unknown/missing demo argument: " + arg);
        }
        if (!options.writeback && (preview || !capture.empty()))
            throw std::runtime_error("--no-writeback requires --no-preview and no --capture");
        std::ofstream csv;
        if (!profile.empty()) {
            csv.open(profile);
            if (!csv) throw std::runtime_error("cannot open profile CSV: " + profile);
            csv << "frame,warmup,prepared_frame,video_ms,ui_ms,request_ms,commit_ms,writeback_ms,verify_ms,preview_ms,capture_ms,work_ms,pacing_ms,total_ms\n";
            csv << std::fixed << std::setprecision(6);
        }
        if (device.empty()) {
            for (const auto& path : zenith::display::card_paths())
                if (zenith::display::driver_name(path) == "vkms") { device = path; break; }
            if (device.empty()) throw std::runtime_error("no VKMS card found");
        }
        zenith::display::AtomicDisplay display(device, width, height, take_over, options);
        std::cout << display.description() << '\n' << std::flush;
        auto first = display.next_frame(); fill_source(first.video, 0, options.video_format);
        zenith::ui::draw(first.ui_argb.pixels, width, height, first.ui_argb.pitch, 0);
        display.test_only(first.slot);
        std::cout << "ATOMIC TEST_ONLY PASS (real two-plane framebuffers, writeback=" << options.writeback << ")\n" << std::flush;
        if (test_only) return 0;
#ifdef ZT_ENABLE_PREVIEW
        std::unique_ptr<zenith::Preview> window;
        if (preview) window = std::make_unique<zenith::Preview>(width, height);
#else
        (void)preview;
#endif
        interrupted = 0;
        const auto old_int = std::signal(SIGINT, stop), old_term = std::signal(SIGTERM, stop);
        struct RestoreSignals { decltype(old_int) a, b; ~RestoreSignals() { std::signal(SIGINT,a); std::signal(SIGTERM,b); } } restore{old_int,old_term};
        unsigned completed = 0;
        auto prepared_targets = first;
        const auto start = std::chrono::steady_clock::now();
        for (unsigned frame = 0; !interrupted && (!frames || frame < static_cast<unsigned>(frames)); ++frame) {
            using Clock = std::chrono::steady_clock;
            const auto begin = Clock::now();
            auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b-a).count(); };
            double video_ms = 0, ui_ms = 0;
            int prepared_frame = -1;
            auto prepare = [&](unsigned number) {
                const auto before = Clock::now();
                auto targets = display.next_frame();
                fill_source(targets.video, number, options.video_format);
                const auto generated = Clock::now();
                zenith::ui::draw(targets.ui_argb.pixels, width, height, targets.ui_argb.pitch, number);
                video_ms = ms(before, generated); ui_ms = ms(generated, Clock::now());
                prepared_frame = number;
                return targets;
            };
            zenith::display::PresentTiming timing;
            zenith::display::CapturedImage image;
            if (pipeline) {
                display.submit(prepared_targets.slot, &timing);
                // Blocking commit has released the previous scanning slot. Its
                // CPU writes can overlap the new slot's independent writeback.
                if (!interrupted && (!frames || frame + 1 < static_cast<unsigned>(frames)))
                    prepared_targets = prepare(frame + 1);
                image = display.finish(&timing);
            } else {
                const auto targets = prepare(frame);
                image = display.present(targets.slot, &timing);
            }
            const auto presented = Clock::now();
            if (options.writeback) verify(image, frame);
            ++completed;
            const auto verified = Clock::now();
            if (!capture.empty() && (frame == 30 || (frames && frame+1 == static_cast<unsigned>(frames)))) save(capture, image);
            const auto captured = Clock::now();
            bool keep_running = true;
#ifdef ZT_ENABLE_PREVIEW
            if (window) keep_running = window->show(image.pixels, image.pitch);
#endif
            const auto shown = Clock::now();
            if (completed % 60 == 0) std::cout << (options.writeback ? "WRITEBACK PASS frames=" : "SCANOUT frames=") << completed << (options.writeback ? " alpha=0/128/255\n" : " (no capture validation)\n") << std::flush;
            if (!uncapped) std::this_thread::sleep_until(start + std::chrono::milliseconds((frame+1)*33));
            const auto end = Clock::now();
            if (csv) csv << frame << ',' << (frame < static_cast<unsigned>(warmup)) << ','
                << prepared_frame << ',' << video_ms << ',' << ui_ms << ',' << timing.request_ms << ','
                << timing.commit_ms << ',' << timing.writeback_ms << ',' << ms(presented,verified) << ','
                << ms(captured,shown) << ',' << ms(verified,captured) << ',' << ms(begin,shown) << ','
                << ms(shown,end) << ',' << ms(begin,end) << '\n';
            if (!keep_running) break;
        }
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        display.shutdown();
        std::cout << (options.writeback ? "COMPOSITION PASS frames=" : "SCANOUT COMPLETE frames=") << completed << " elapsed=" << std::fixed << std::setprecision(2)
            << elapsed << "s fps=" << (elapsed > 0 ? completed / elapsed : 0)
            << "; owned CRTC disabled; buffers released on exit\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << "DRM DEMO FAIL: " << e.what() << '\n'; return 1; }
}
