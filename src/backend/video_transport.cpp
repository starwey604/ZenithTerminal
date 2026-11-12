// Reassembly follows BUAA VideoReceiver's indexed-fragment/total-length design.
// Copyright (c) 2026 huhu233. MIT notice retained in BUAA-LICENSE.
// Zenith adds bounded pending state, contiguous-index checks and restart handling.
#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <cstring>
#include <poll.h>
#include <rm2027/video/udp_header.h>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>
#include <zenith/video_transport.hpp>
namespace zenith::backend {
namespace {
constexpr std::size_t max_au = 8u << 20, max_pending = 8;
constexpr std::int64_t timeout = 300;
std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
void check(int r, const char *action) {
    if (r < 0)
        throw std::runtime_error(std::string(action) + ": " + std::strerror(errno));
}
sockaddr_in address(const std::string &host, std::uint16_t port) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    if (!port || inet_pton(AF_INET, host.c_str(), &a.sin_addr) != 1)
        throw std::runtime_error("invalid UDP IPv4 address/port");
    return a;
}
} // namespace
void fragment(const video::AccessUnit &au, std::uint16_t seq, const PacketSink &sink) {
    if (au.bytes.empty() || au.bytes.size() > max_au)
        throw std::runtime_error("invalid transport AU size");
    std::array<std::uint8_t, rm2027::kDefaultMaxDatagram> p{};
    std::uint16_t index = 0;
    for (std::size_t off = 0; off < au.bytes.size(); off += rm2027::kDefaultPayloadPerPacket) {
        auto n = std::min(rm2027::kDefaultPayloadPerPacket, au.bytes.size() - off);
        rm2027::writeUdpVideoHeader(p.data(), {seq, index++, static_cast<std::uint32_t>(au.bytes.size())});
        std::memcpy(p.data() + 8, au.bytes.data() + off, n);
        sink({p.data(), n + 8});
    }
}
void Reassembler::expire(std::int64_t now) {
    for (auto it = pending_.begin(); it != pending_.end();)
        if (now - it->second.since > timeout) {
            ++stats_.expired;
            it = pending_.erase(it);
        } else
            ++it;
    // After an idle/disconnected source, a restarted sender may begin again at 1.
    if (previous_ && now - last_complete_ms_ > timeout) {
        previous_.reset();
        stats_.expired += pending_.size();
        pending_.clear();
    }
}
std::optional<video::AccessUnit> Reassembler::accept(std::span<const std::uint8_t> p, std::int64_t now) {
    ++stats_.packets;
    expire(now);
    if (p.size() <= 8 || p.size() > 1400) {
        ++stats_.invalid;
        return {};
    }
    auto h = rm2027::readUdpVideoHeader(p.data());
    p = p.subspan(8);
    if (!h.frameTotalBytes || h.frameTotalBytes > max_au || p.size() > h.frameTotalBytes ||
        h.fragmentIndex >= h.frameTotalBytes) {
        ++stats_.invalid;
        return {};
    }
    if (previous_) {
        auto distance = static_cast<std::uint16_t>(h.frameNumber - *previous_);
        if (distance == 0 || distance >= 0x8000) {
            ++stats_.stale;
            return {};
        }
    }
    if (!pending_.contains(h.frameNumber)) {
        if (pending_.size() >= max_pending) {
            auto oldest = std::min_element(pending_.begin(), pending_.end(),
                                           [](auto &a, auto &b) { return a.second.since < b.second.since; });
            pending_.erase(oldest);
            ++stats_.expired;
        }
        pending_[h.frameNumber] = {h.frameTotalBytes, 0, now, {}};
    }
    auto &f = pending_.at(h.frameNumber);
    if (f.expected != h.frameTotalBytes) {
        ++stats_.invalid;
        return {};
    }
    if (auto i = f.parts.find(h.fragmentIndex); i != f.parts.end()) {
        if (i->second.size() == p.size() && std::equal(p.begin(), p.end(), i->second.begin()))
            ++stats_.duplicates;
        else {
            ++stats_.invalid;
            pending_.erase(h.frameNumber);
        }
        return {};
    }
    if (f.got + p.size() > f.expected) {
        ++stats_.invalid;
        pending_.erase(h.frameNumber);
        return {};
    }
    f.parts[h.fragmentIndex] = {p.begin(), p.end()};
    f.got += p.size();
    if (f.got != f.expected)
        return {};
    // BUAA's frameTotalBytes and indexed fragments; additionally require no index holes.
    video::AccessUnit au;
    au.bytes.reserve(f.expected);
    unsigned expected_index = 0;
    for (auto &[index, bytes] : f.parts) {
        if (index != expected_index++) {
            ++stats_.invalid;
            pending_.erase(h.frameNumber);
            return {};
        }
        au.bytes.insert(au.bytes.end(), bytes.begin(), bytes.end());
    }
    au.discontinuity = !previous_ || h.frameNumber != static_cast<std::uint16_t>(*previous_ + 1);
    if (previous_ && au.discontinuity)
        ++stats_.gaps;
    previous_ = h.frameNumber;
    last_complete_ms_ = now;
    pending_.erase(h.frameNumber);
    ++stats_.complete;
    au.pts_us = now * 1000;
    return au;
}
UdpReceiver::UdpReceiver(const std::string &host, std::uint16_t port) {
    auto a = address(host, port);
    fd_ = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    check(fd_, "UDP socket");
    int size = 16 * 1024 * 1024;
    setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &size, sizeof size);
    if (bind(fd_, reinterpret_cast<sockaddr *>(&a), sizeof a) < 0) {
        int e = errno;
        close(fd_);
        fd_ = -1;
        errno = e;
        check(-1, "bind UDP");
    }
}
UdpReceiver::~UdpReceiver() {
    if (fd_ >= 0)
        close(fd_);
}
std::optional<video::AccessUnit> UdpReceiver::next(int ms) {
    const auto end = now_ms() + ms;
    std::array<std::uint8_t, 65536> bytes{};
    do {
        pollfd p{fd_, POLLIN, 0};
        int n = poll(&p, 1, std::max(0, static_cast<int>(end - now_ms())));
        if (n < 0 && errno == EINTR)
            return {};
        check(n, "poll UDP");
        if (!n) {
            assembly_.expire(now_ms());
            return {};
        }
        if (p.revents & (POLLERR | POLLHUP | POLLNVAL))
            throw std::runtime_error("UDP receive socket failed");
        n = recv(fd_, bytes.data(), bytes.size(), 0);
        if (n < 0 && errno == EINTR)
            continue;
        check(n, "receive UDP");
        if (auto au = assembly_.accept({bytes.data(), static_cast<std::size_t>(n)}, now_ms()))
            return au;
    } while (now_ms() < end);
    return {};
}
UdpSender::UdpSender(const std::string &host, std::uint16_t port) {
    auto a = address(host, port);
    fd_ = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    check(fd_, "UDP socket");
    if (connect(fd_, reinterpret_cast<sockaddr *>(&a), sizeof a) < 0) {
        int e = errno;
        close(fd_);
        fd_ = -1;
        errno = e;
        check(-1, "connect UDP");
    }
}
UdpSender::~UdpSender() {
    if (fd_ >= 0)
        close(fd_);
}
void UdpSender::send(const video::AccessUnit &au) {
    fragment(au, ++sequence_, [&](auto p) {
        auto n = ::send(fd_, p.data(), p.size(), 0);
        check(n, "send UDP");
        if (static_cast<std::size_t>(n) != p.size())
            throw std::runtime_error("short UDP send");
    });
}
} // namespace zenith::backend
