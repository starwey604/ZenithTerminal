#include "drm_demo.hpp"
#ifdef ZT_ENABLE_VIDEO
#include "video_player.hpp"
#endif
#include <zenith/display/atomic_display.hpp>
#include <zenith/display/probe.hpp>
#include <zenith/ui.hpp>
#include <SDL3/SDL.h>
#ifdef ZT_ENABLE_PREVIEW
#include <zenith/preview.hpp>
#endif
#include <array>
#include <cmath>
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
#ifdef ZT_ENABLE_VIDEO
void verify_video(const zenith::display::CapturedImage& image, const zenith::video::Frame& f,
                  const zenith::display::MappedImage& ui) {
    // Independent color/range and premultiplied-alpha reference at a spread of pixels.
    const double kr=f.color.matrix==zenith::video::Matrix::bt709 ? .2126 :
        f.color.matrix==zenith::video::Matrix::bt2020 ? .2627 : .299;
    const double kb=f.color.matrix==zenith::video::Matrix::bt709 ? .0722 :
        f.color.matrix==zenith::video::Matrix::bt2020 ? .0593 : .114;
    const bool full=f.color.range==zenith::video::Range::full;
    for(int y : {28, image.height/3, image.height/2, image.height-28})
        for(int x : {36,76,116,image.width/4,image.width/2,3*image.width/4,image.width-36}) {
            const double l=(f.y[y*f.y_pitch+x]-(full?0:16))*(full?1.:255./219.);
            const auto uv=(y/2)*f.uv_pitch+(x&~1);
            const double u=(f.uv[uv]-128)*(full?1.:255./224.);
            const double v=(f.uv[uv+1]-128)*(full?1.:255./224.);
            const std::array<double,3> rgb{l+2*(1-kb)*u,l-2*kb*(1-kb)/(1-kr-kb)*u-2*kr*(1-kr)/(1-kr-kb)*v,l+2*(1-kr)*v};
            std::uint32_t overlay,pixel;
            std::memcpy(&overlay,ui.pixels+y*ui.pitch+x*4,4);
            std::memcpy(&pixel,image.pixels+y*image.pitch+x*4,4);
            const int alpha=overlay>>24;
            for(int c=0;c<3;++c) {
                const auto expected=std::clamp(rgb[c],0.,255.)*(255-alpha)/255.+((overlay>>(8*c))&255);
                if(std::abs(static_cast<int>((pixel>>(8*c))&255)-expected)>4.)
                    throw std::runtime_error("video writeback color/alpha mismatch at "+std::to_string(x)+","+std::to_string(y));
            }
        }
}
#endif
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
        std::string device, capture, profile, video_file, video_bind="127.0.0.1", va_device="/dev/dri/renderD128";
        bool video_loop=false, explicit_size=false; unsigned video_port=0;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--video-loop") video_loop=true;
            else if (arg == "--pipeline") pipeline = true;
            else if (arg == "--test-only") test_only = true;
            else if (arg == "--uncapped") uncapped = true;
            else if (arg == "--take-over") take_over = true;
            else if (arg == "--no-writeback") options.writeback = false;
            else if (arg == "--no-preview") preview = false;
            else if ((arg == "--video-file" || arg == "--video-udp" || arg == "--video-bind" || arg == "--va-device" || arg == "--size" || arg == "--device" || arg == "--frames" || arg == "--capture" || arg == "--profile" || arg == "--warmup" || arg == "--video-format" || arg == "--state") && i + 1 < argc) {
                const std::string value = argv[++i];
                if (arg == "--video-file") { if(value.empty()) throw std::runtime_error("video file must not be empty"); video_file=value; }
                else if (arg == "--video-bind") video_bind=value;
                else if (arg == "--va-device") va_device=value;
                else if (arg == "--video-udp") { video_port=count(value); if(!video_port||video_port>65535) throw std::runtime_error("invalid UDP port"); }
                else if (arg == "--size") {
                    explicit_size=true;
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
                std::cout << "ZenithTerminal --drm-demo [--size WIDTHxHEIGHT] [--pipeline] [--test-only] [--take-over] [--no-preview] [--frames N] [--capture out.bmp] [--device /dev/dri/cardN] [--profile out.csv] [--warmup N] [--uncapped] [--video-format nv12|xrgb] [--no-writeback] [--state full|delta] [--video-file HEVC.mp4 | --video-udp PORT] [--video-loop] [--video-bind IP] [--va-device /dev/dri/renderD128]\n"
                    "Default: VKMS, 1280x720, 300 frames; N=0 runs until Escape/SIGINT. Requires DRM master; --take-over permits replacing inherited active state.\n";
                std::cout << "Decoded video uses VAAPI with explicit CPU NV12 staging; video defaults to 60 Hz rendering. --video-loop applies to files only.\n";
                return 0;
            } else throw std::runtime_error("unknown/missing demo argument: " + arg);
        }
        if (!options.writeback && (preview || !capture.empty()))
            throw std::runtime_error("--no-writeback requires --no-preview and no --capture");
        const bool use_video=!video_file.empty()||video_port;
        if(!video_file.empty()&&video_port) throw std::runtime_error("choose --video-file or --video-udp, not both");
        if(video_loop&&video_file.empty()) throw std::runtime_error("--video-loop requires --video-file");
        if(use_video&&options.video_format!=zenith::display::VideoFormat::nv12)
            throw std::runtime_error("decoded video requires nv12");
#ifndef ZT_ENABLE_VIDEO
        if(use_video) throw std::runtime_error("video backend disabled; configure native with ZT_VIDEO_BACKEND=vaapi");
#endif
        std::ofstream csv;
        if (!profile.empty()) {
            csv.open(profile);
            if (!csv) throw std::runtime_error("cannot open profile CSV: " + profile);
            csv << "frame,warmup,prepared_frame,video_ms,ui_ms,request_ms,commit_ms,writeback_ms,verify_ms,preview_ms,capture_ms,work_ms,pacing_ms,total_ms,begin_mono_ns,fence_mono_ns,end_mono_ns\n";
            csv << std::fixed << std::setprecision(6);
        }
#ifdef ZT_ENABLE_VIDEO
        std::unique_ptr<zenith::VideoPlayer> player;
        zenith::VideoSnapshot video_snapshot;
        std::shared_ptr<const zenith::video::Frame> prepared_video;
        if(use_video) {
            player=std::make_unique<zenith::VideoPlayer>(video_file,video_loop,video_bind,video_port,va_device);
            video_snapshot=player->wait_first(); prepared_video=video_snapshot.frame;
            if(explicit_size&&(width!=prepared_video->width||height!=prepared_video->height))
                throw std::runtime_error("--size must match decoded video dimensions");
            width=prepared_video->width;height=prepared_video->height;
            options.color=prepared_video->color;options.require_color_properties=true;
            std::cout << "VIDEO READY " << width << 'x' << height << " VAAPI -> CPU NV12 staging; color matrix="
                << static_cast<int>(options.color.matrix) << " range=" << static_cast<int>(options.color.range) << '\n' << std::flush;
        }
#else
        (void)explicit_size;
#endif
        const std::string source_label=!use_video ? "" : (video_port ? "UDP HEVC / VAAPI" : "HEVC FILE / VAAPI");
        auto fill=[&](const zenith::display::MappedImage& target,unsigned number) {
#ifdef ZT_ENABLE_VIDEO
            if(player) {
                video_snapshot=player->snapshot();prepared_video=video_snapshot.frame;
                if(prepared_video->color!=options.color)throw std::runtime_error("video color metadata changed; restart display for the new stream");
                prepared_video->copy_to(target.pixels,target.pitch,target.pixels+target.uv_offset,target.pitch,width,height);
                return;
            }
#endif
            fill_source(target,number,options.video_format);
        };
        if (device.empty()) {
            for (const auto& path : zenith::display::card_paths())
                if (zenith::display::driver_name(path) == "vkms") { device = path; break; }
            if (device.empty()) throw std::runtime_error("no VKMS card found");
        }
        zenith::display::AtomicDisplay display(device, width, height, take_over, options);
        std::cout << display.description() << '\n' << std::flush;
        auto first = display.next_frame(); fill(first.video, 0);
        zenith::ui::draw(first.ui_argb.pixels, width, height, first.ui_argb.pitch, 0, source_label);
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
        std::uint64_t unique_video_frames=0;
        std::int64_t last_video_pts=-1;
        auto prepared_targets = first;
        const auto start = std::chrono::steady_clock::now();
        for (unsigned frame = 0; !interrupted && (!frames || frame < static_cast<unsigned>(frames)); ++frame) {
            using Clock = std::chrono::steady_clock;
            const auto begin = Clock::now();
            auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b-a).count(); };
            double video_ms = 0, ui_ms = 0;
            int prepared_frame = -1;
            #ifdef ZT_ENABLE_VIDEO
            auto submitted_video=prepared_video;
#endif
            auto submitted_ui=prepared_targets.ui_argb;
            auto prepare = [&](unsigned number) {
                const auto before = Clock::now();
                auto targets = display.next_frame();
                fill(targets.video, number);
                const auto generated = Clock::now();
                zenith::ui::draw(targets.ui_argb.pixels, width, height, targets.ui_argb.pitch, number, source_label);
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
#ifdef ZT_ENABLE_VIDEO
                submitted_video=prepared_video;
#endif
                submitted_ui=targets.ui_argb;
                image = display.present(targets.slot, &timing);
            }
            const auto presented = Clock::now();
            if (options.writeback) {
#ifdef ZT_ENABLE_VIDEO
                if(player)verify_video(image,*submitted_video,submitted_ui);else
#endif
                verify(image, frame);
            }
#ifndef ZT_ENABLE_VIDEO
            (void)submitted_ui;
            (void)unique_video_frames; (void)last_video_pts;
#endif
            #ifdef ZT_ENABLE_VIDEO
            if(player && submitted_video->pts_us!=last_video_pts) {++unique_video_frames;last_video_pts=submitted_video->pts_us;}
#endif
            ++completed;
            const auto verified = Clock::now();
            bool final_video_frame=false;
#ifdef ZT_ENABLE_VIDEO
            final_video_frame=player && video_snapshot.finished && submitted_video==video_snapshot.frame;
#endif
            if (!capture.empty() && (frame == 30 || final_video_frame || (frames && frame+1 == static_cast<unsigned>(frames)))) save(capture, image);
            const auto captured = Clock::now();
            bool keep_running = true;
#ifdef ZT_ENABLE_PREVIEW
            if (window) keep_running = window->show(image.pixels, image.pitch);
#endif
            const auto shown = Clock::now();
            if (completed % 60 == 0) std::cout << (options.writeback ? "WRITEBACK PASS frames=" : "SCANOUT frames=") << completed << (options.writeback ? (use_video ? " video color/alpha samples=28\n" : " alpha=0/128/255\n") : " (no capture validation)\n") << std::flush;
            if (!uncapped) std::this_thread::sleep_until(start + (use_video ? std::chrono::microseconds((std::uint64_t{frame}+1)*1000000/60) : std::chrono::microseconds((std::uint64_t{frame}+1)*33000)));
            const auto end = Clock::now();
            if (csv) csv << frame << ',' << (frame < static_cast<unsigned>(warmup)) << ','
                << prepared_frame << ',' << video_ms << ',' << ui_ms << ',' << timing.request_ms << ','
                << timing.commit_ms << ',' << timing.writeback_ms << ',' << ms(presented,verified) << ','
                << ms(captured,shown) << ',' << ms(verified,captured) << ',' << ms(begin,shown) << ','
                << ms(shown,end) << ',' << ms(begin,end) << ','
                << std::chrono::duration_cast<std::chrono::nanoseconds>(begin.time_since_epoch()).count() << ','
                << std::chrono::duration_cast<std::chrono::nanoseconds>(presented.time_since_epoch()).count() << ','
                << std::chrono::duration_cast<std::chrono::nanoseconds>(end.time_since_epoch()).count() << '\n';
            #ifdef ZT_ENABLE_VIDEO
            if(player && video_snapshot.finished && submitted_video==video_snapshot.frame)keep_running=false;
#endif
            if (!keep_running) break;
        }
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        display.shutdown();
        std::cout << (options.writeback ? "COMPOSITION PASS frames=" : "SCANOUT COMPLETE frames=") << completed << " elapsed=" << std::fixed << std::setprecision(2)
            << elapsed << "s fps=" << (elapsed > 0 ? completed / elapsed : 0)
            << "; owned CRTC disabled; buffers released on exit\n";
#ifdef ZT_ENABLE_VIDEO
        if(player) {
            const auto stats=player->snapshot();
            std::cout<<"VIDEO decoded="<<stats.decoded<<" loops="<<stats.loops
                <<" unique_presented="<<unique_video_frames<<" udp_complete="<<stats.transport.complete<<" gaps="<<stats.transport.gaps
                <<" invalid="<<stats.transport.invalid<<" expired="<<stats.transport.expired<<'\n';
        }
#endif
        return 0;
    } catch (const std::exception& e) { std::cerr << "DRM DEMO FAIL: " << e.what() << '\n'; return 1; }
}
