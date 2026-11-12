#include <SDL3/SDL.h>
#include <MQTTAsync.h>
#include <mqtt/async_client.h>
#include <google/protobuf/stubs/common.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <thread>

#include "smoke.pb.h"
#ifdef ZT_ENABLE_DRM
#include "src/app/drm_demo.hpp"
#endif

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

void self_check() {
    std::atomic<int> counter{0};
    const auto start = std::chrono::steady_clock::now();
    std::thread worker([&counter] { counter.fetch_add(42); });
    worker.join();
    require(counter.load() == 42 && std::chrono::steady_clock::now() >= start,
            "thread/clock check failed");
    std::cout << "threads/clock: PASS\n";

    zt::smoke::Status sent;
    sent.set_platform(ZT_PLATFORM);
    sent.set_frame_width(64);
    sent.set_sequence(2027);
    std::string wire;
    require(sent.SerializeToString(&wire), "protobuf serialization failed");
    zt::smoke::Status received;
    require(received.ParseFromString(wire) && received.platform() == ZT_PLATFORM &&
                received.frame_width() == 64 && received.has_sequence() && received.sequence() == 2027,
            "protobuf roundtrip failed");
    std::cout << "protobuf " << GOOGLE_PROTOBUF_VERSION << ": PASS (proto3 optional)\n";

    // Surface rendering does not initialize a display driver or require DRM permissions.
    const auto destroy_surface = [](SDL_Surface* p) { SDL_DestroySurface(p); };
    const auto destroy_renderer = [](SDL_Renderer* p) { SDL_DestroyRenderer(p); };
    std::unique_ptr<SDL_Surface, decltype(destroy_surface)> surface(
        SDL_CreateSurface(64, 32, SDL_PIXELFORMAT_ARGB8888), destroy_surface);
    require(surface != nullptr, SDL_GetError());
    std::unique_ptr<SDL_Renderer, decltype(destroy_renderer)> renderer(
        SDL_CreateSoftwareRenderer(surface.get()), destroy_renderer);
    require(renderer != nullptr, SDL_GetError());
    require(SDL_SetRenderDrawColor(renderer.get(), 0, 0, 0, 0), SDL_GetError());
    require(SDL_RenderClear(renderer.get()), SDL_GetError());
    require(SDL_SetRenderDrawBlendMode(renderer.get(), SDL_BLENDMODE_NONE), SDL_GetError());
    require(SDL_SetRenderDrawColor(renderer.get(), 30, 180, 240, 128), SDL_GetError());
    const SDL_FRect rect{8, 8, 24, 16};
    require(SDL_RenderFillRect(renderer.get(), &rect), SDL_GetError());
    require(SDL_RenderPresent(renderer.get()), SDL_GetError());
    Uint8 r{}, g{}, b{}, a{};
    require(SDL_ReadSurfacePixel(surface.get(), 10, 10, &r, &g, &b, &a), SDL_GetError());
    require(r == 30 && g == 180 && b == 240 && a == 128, "SDL foreground pixel mismatch");
    require(SDL_ReadSurfacePixel(surface.get(), 0, 0, &r, &g, &b, &a), SDL_GetError());
    require(r == 0 && g == 0 && b == 0 && a == 0, "SDL background pixel mismatch");
    std::cout << "SDL " << SDL_GetVersion() << ": PASS (ARGB surface)\n";

    // Creating a client exercises both Paho layers without opening a broker connection.
    mqtt::async_client client("tcp://127.0.0.1:1883", "zt-platform-smoke");
    require(!client.is_connected(), "MQTT client unexpectedly connected");
    require(MQTTAsync_getVersionInfo() != nullptr, "Paho C version unavailable");
    std::cout << "Paho C/C++: PASS (offline client)\n";
}
} // namespace

int main(int argc, char** argv) {
#ifdef ZT_ENABLE_DRM
    if (argc >= 2 && std::string_view(argv[1]) == "--drm-demo") return run_drm_demo(argc - 1, argv + 1);
#endif
    if (argc > 2 || (argc == 2 && std::string_view(argv[1]) != "--self-check")) {
        std::cerr << "Usage: ZenithTerminal [--self-check]\n";
        return 2;
    }
    try {
#if defined(__aarch64__)
        constexpr auto arch = "aarch64";
#elif defined(__x86_64__)
        constexpr auto arch = "x86_64";
#else
        constexpr auto arch = "other";
#endif
        std::cout << "ZenithTerminal platform=" << ZT_PLATFORM << " arch=" << arch
                  << " C++=" << __cplusplus << '\n';
        self_check();
        std::cout << "SELF-CHECK PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "SELF-CHECK FAIL: " << error.what() << '\n';
        return 1;
    }
}
