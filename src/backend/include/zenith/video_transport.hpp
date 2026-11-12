#pragma once
#include <chrono>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <zenith/video.hpp>
namespace zenith::backend {
// Wire codec is compiled directly from the pinned BUAA source. No FFmpeg/MQTT dependency.
using PacketSink = std::function<void(std::span<const std::uint8_t>)>;
void fragment(const video::AccessUnit &, std::uint16_t sequence, const PacketSink &);
struct TransportStats {
    std::uint64_t packets{}, complete{}, invalid{}, duplicates{}, expired{}, gaps{}, stale{};
};
class Reassembler {
  public:
    std::optional<video::AccessUnit> accept(std::span<const std::uint8_t>, std::int64_t now_ms);
    void expire(std::int64_t now_ms);
    const TransportStats &stats() const { return stats_; }

  private:
    struct Pending {
        std::uint32_t expected{};
        std::size_t got{};
        std::int64_t since{};
        std::map<std::uint16_t, std::vector<std::uint8_t>> parts;
    };
    std::map<std::uint16_t, Pending> pending_;
    TransportStats stats_;
    std::optional<std::uint16_t> previous_;
    std::int64_t last_complete_ms_{};
};
class UdpReceiver {
  public:
    UdpReceiver(const std::string &address, std::uint16_t port);
    ~UdpReceiver();
    UdpReceiver(const UdpReceiver &) = delete;
    UdpReceiver& operator=(const UdpReceiver&) = delete;
    // Bounded wait; caller can stop promptly or retain the last decoded frame.
    std::optional<video::AccessUnit> next(int timeout_ms = 100);
    const TransportStats &stats() const { return assembly_.stats(); }

  private:
    int fd_{-1};
    Reassembler assembly_;
};
class UdpSender {
  public:
    UdpSender(const std::string &address, std::uint16_t port);
    ~UdpSender();
    UdpSender(const UdpSender &) = delete;
    UdpSender& operator=(const UdpSender&) = delete;
    void send(const video::AccessUnit &);

  private:
    int fd_{-1};
    std::uint16_t sequence_{};
};
} // namespace zenith::backend
