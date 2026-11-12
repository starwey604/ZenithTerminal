#include "video_player.hpp"
#include "../decoder/vaapi/vaapi.hpp"
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <thread>
#include <zenith/video_file.hpp>
namespace zenith {
struct VideoPlayer::Impl {
    std::mutex mutex;
    std::condition_variable cv;
    VideoSnapshot state;
    std::exception_ptr error;
    std::jthread worker;
    Impl(std::string file, bool loop, std::string bind, unsigned port, std::string va) {
        worker = std::jthread([this, file = std::move(file), loop, bind = std::move(bind), port,
                               va = std::move(va)](std::stop_token stop) {
            try {
                auto decoder = video::make_vaapi_decoder(va);
                std::unique_ptr<video::HevcFile> source;
                std::unique_ptr<backend::UdpReceiver> rx;
                if (!file.empty())
                    source = std::make_unique<video::HevcFile>(file, loop);
                else {
                    rx = std::make_unique<backend::UdpReceiver>(bind, port);
                    std::cout << "VIDEO RX READY " << bind << ':' << port << '\n' << std::flush;
                }
                const auto start = std::chrono::steady_clock::now();
                auto publish = [&](auto frame) {
                    std::unique_lock lock(mutex);
                    if (source)
                        cv.wait_until(lock, start + std::chrono::microseconds(frame->pts_us),
                                      [&] { return stop.stop_requested(); });
                    if (stop.stop_requested())
                        return;
                    state.frame = std::move(frame);
                    ++state.decoded;
                    state.loops = source ? source->loops() : 0;
                    if (rx)
                        state.transport = rx->stats();
                    cv.notify_all();
                };
                while (!stop.stop_requested()) {
                    auto au = source ? source->next() : rx->next();
                    if (!au) {
                        if (!source)
                            continue;
                        for (auto &f : decoder->drain())
                            publish(f);
                        std::lock_guard lock(mutex);
                        state.finished = true;
                        cv.notify_all();
                        return;
                    }
                    for (auto &f : decoder->push(*au))
                        publish(f);
                }
            } catch (...) {
                std::lock_guard lock(mutex);
                error = std::current_exception();
                cv.notify_all();
            }
        });
    }
    ~Impl() {
        worker.request_stop();
        cv.notify_all();
        worker.join();
    }
};
VideoPlayer::VideoPlayer(std::string file, bool loop, std::string bind, unsigned port, std::string va)
    : impl_(std::make_unique<Impl>(std::move(file), loop, std::move(bind), port, std::move(va))) {}
VideoPlayer::~VideoPlayer() = default;
VideoSnapshot VideoPlayer::snapshot() {
    std::lock_guard lock(impl_->mutex);
    if (impl_->error)
        std::rethrow_exception(impl_->error);
    return impl_->state;
}
VideoSnapshot VideoPlayer::wait_first() {
    std::unique_lock lock(impl_->mutex);
    if (!impl_->cv.wait_for(lock, std::chrono::seconds(10),
                            [&] { return impl_->error || impl_->state.frame || impl_->state.finished; }))
        throw std::runtime_error("no decoded video within 10 seconds; start the UDP replay sender");
    if (impl_->error)
        std::rethrow_exception(impl_->error);
    if (!impl_->state.frame)
        throw std::runtime_error("video contained no decodable frames");
    return impl_->state;
}
} // namespace zenith
