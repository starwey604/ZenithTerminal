#include <zenith/display/atomic_display.hpp>
#include <zenith/display/probe.hpp>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include <linux/sync_file.h>
#include <array>
#include <chrono>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <poll.h>
#include <stdexcept>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace zenith::display {
namespace {
void require(bool ok, const std::string& message) { if (!ok) throw std::runtime_error(message); }
void check(int result, const char* message) {
    if (result < 0) throw std::runtime_error(std::string(message) + ": " + std::strerror(errno));
}
struct Fd {
    int value{-1};
    ~Fd() { if (value >= 0) close(value); }
    void reset(int next = -1) { if (value >= 0) close(value); value = next; }
};
const Property* prop(const std::vector<Property>& values, const char* name) {
    auto it = std::find_if(values.begin(), values.end(), [&](const auto& p) { return p.name == name; });
    return it == values.end() ? nullptr : &*it;
}
bool linear(const Plane& plane, std::uint32_t format) {
    return std::any_of(plane.format_modifiers.begin(), plane.format_modifiers.end(),
        [&](const auto& f) { return f.format == format && f.modifier == DRM_FORMAT_MOD_LINEAR; });
}
class Request {
public:
    Request() : value(drmModeAtomicAlloc()) { require(value, "drmModeAtomicAlloc failed"); }
    ~Request() { drmModeAtomicFree(value); }
    void add(std::uint32_t id, const std::vector<Property>& properties, const char* name, std::uint64_t v) {
        const auto* p = prop(properties, name);
        require(p != nullptr, std::string("missing atomic property: ") + name);
        check(drmModeAtomicAddProperty(value, id, p->id, v), "atomic add property");
    }
    void optional(std::uint32_t id, const std::vector<Property>& properties, const char* name, std::uint64_t v) {
        if (prop(properties, name)) add(id, properties, name, v);
    }
    void enumeration(std::uint32_t id, const std::vector<Property>& properties, const char* name, const char* label) {
        const auto* p = prop(properties, name);
        if (!p) return;
        const auto it = std::find_if(p->enums.begin(), p->enums.end(), [&](const auto& e) { return e.name == label; });
        require(it != p->enums.end(), std::string("missing enum ") + name + ": " + label);
        add(id, properties, name, it->value);
    }
    drmModeAtomicReq* value;
};
struct Buffer {
    int fd;
    std::uint32_t handle{}, fb{}, pitch{}, uv_offset{};
    std::uint64_t size{};
    void* pixels{MAP_FAILED};
    int width, height;
    Buffer(int device, int w, int h) : fd(device), width(w), height(h) {}
    void allocate(std::uint32_t format) {
        const bool nv12 = format == DRM_FORMAT_NV12;
        drm_mode_create_dumb create{};
        create.width = width; create.height = nv12 ? height + height / 2 : height;
        create.bpp = nv12 ? 8 : 32;
        check(drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create), "create dumb buffer");
        handle = create.handle; pitch = create.pitch; size = create.size;
        require(size <= std::numeric_limits<std::size_t>::max(), "buffer too large to map");
        const std::uint64_t uv = std::uint64_t{pitch} * height;
        require(!nv12 || (uv <= UINT32_MAX && size >= uv + std::uint64_t{pitch} * height / 2), "invalid NV12 allocation");
        uv_offset = nv12 ? static_cast<std::uint32_t>(uv) : 0;
        std::uint32_t handles[4]{handle, nv12 ? handle : 0, 0, 0};
        std::uint32_t pitches[4]{pitch, nv12 ? pitch : 0, 0, 0};
        std::uint32_t offsets[4]{0, uv_offset, 0, 0};
        std::uint64_t modifiers[4]{};
        check(drmModeAddFB2WithModifiers(fd, width, height, format, handles, pitches, offsets,
            modifiers, &fb, DRM_MODE_FB_MODIFIERS), "add linear framebuffer");
        drm_mode_map_dumb map{}; map.handle = handle;
        check(drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map), "map dumb offset");
        pixels = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, map.offset);
        require(pixels != MAP_FAILED, "mmap dumb buffer failed");
        std::memset(pixels, 0, size);
    }
    ~Buffer() {
        if (pixels != MAP_FAILED) munmap(pixels, size);
        if (fb) drmModeRmFB(fd, fb);
        if (handle) { drm_mode_destroy_dumb destroy{}; destroy.handle = handle; drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy); }
    }
    MappedImage view() { return {static_cast<std::byte*>(pixels), width, height, pitch, uv_offset}; }
};
}
struct AtomicDisplay::Impl {
    Fd device, fence;
    DeviceSnapshot snapshot;
    Crtc crtc;
    Connector connector, writeback;
    Plane video, ui;
    drmModeModeInfo mode{};
    std::uint32_t mode_blob{};
    struct Buffers { std::unique_ptr<Buffer> video, ui, capture; };
    std::array<Buffers, 2> slots;
    int current{-1};
    bool active{}, pending{};
    int width{}, height{};
    DisplayOptions options;

    void initialize(const std::string& path, int w, int h, bool take_over, DisplayOptions config) {
        options = config;
        require(w >= 320 && h >= 240 && w <= 4096 && h <= 2160 && !(w % 2) && !(h % 2), "invalid/even frame dimensions required");
        width = w; height = h;
        device.value = open(path.c_str(), O_RDWR | O_CLOEXEC);
        check(device.value, "open DRM device");
        require(drmIsMaster(device.value) == 1, "DRM master is held by another process; release the virtual card first (see doc/VKMS_PREVIEW.md)");
        check(drmSetClientCap(device.value, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1), "enable universal planes");
        check(drmSetClientCap(device.value, DRM_CLIENT_CAP_ATOMIC, 1), "enable atomic");
        if (options.writeback) check(drmSetClientCap(device.value, DRM_CLIENT_CAP_WRITEBACK_CONNECTORS, 1), "enable writeback");
        snapshot = probe_device(path);
        bool selected = false;
        for (const auto& c : snapshot.crtcs) {
            if (selected || c.index >= 32) continue;
            const auto mask = std::uint32_t{1} << c.index;
            const auto* active_prop = prop(c.properties, "ACTIVE");
            if (!active_prop || (active_prop->value && !take_over)) continue;
            for (const auto& conn : snapshot.connectors) {
                if (selected || conn.writeback || conn.connection != "connected" || !(conn.possible_crtcs & mask)) continue;
                const auto m = std::find_if(conn.modes.begin(), conn.modes.end(), [&](const auto& candidate) {
                    return candidate.width == static_cast<unsigned>(w) && candidate.height == static_cast<unsigned>(h);
                });
                if (m == conn.modes.end()) continue;
                const auto wb_candidates = options.writeback ? snapshot.connectors : std::vector<Connector>{Connector{}};
                for (const auto& wb : wb_candidates) {
                    if (selected || (options.writeback && (!wb.writeback || !(wb.possible_crtcs & mask) ||
                        std::find(wb.writeback_formats.begin(), wb.writeback_formats.end(), DRM_FORMAT_XRGB8888) == wb.writeback_formats.end()))) continue;
                    for (const auto& v : snapshot.planes) for (const auto& u : snapshot.planes) {
                        if (selected || v.type != "Primary" || u.type != "Overlay" ||
                            !(v.possible_crtcs & u.possible_crtcs & mask) ||
                            !linear(v, options.video_format == VideoFormat::nv12 ? DRM_FORMAT_NV12 : DRM_FORMAT_XRGB8888) || !linear(u, DRM_FORMAT_ARGB8888)) continue;
                        crtc = c; connector = conn; writeback = wb; video = v; ui = u; selected = true;
                    }
                }
            }
        }
        require(selected, "no eligible CRTC (use --take-over for inherited active state) with requested mode, selected linear video primary, ARGB overlay and requested writeback");
        auto* conn = drmModeGetConnectorCurrent(device.value, connector.id);
        require(conn, "get selected connector failed");
        for (int i = 0; i < conn->count_modes; ++i)
            if (conn->modes[i].hdisplay == w && conn->modes[i].vdisplay == h) { mode = conn->modes[i]; break; }
        drmModeFreeConnector(conn);
        require(mode.clock != 0, "selected mode disappeared");
        check(drmModeCreatePropertyBlob(device.value, &mode, sizeof(mode), &mode_blob), "create mode blob");
        for (auto& slot : slots) {
            slot.video = std::make_unique<Buffer>(device.value, w, h); slot.video->allocate(options.video_format == VideoFormat::nv12 ? DRM_FORMAT_NV12 : DRM_FORMAT_XRGB8888);
            slot.ui = std::make_unique<Buffer>(device.value, w, h); slot.ui->allocate(DRM_FORMAT_ARGB8888);
            if (options.writeback) { slot.capture = std::make_unique<Buffer>(device.value, w, h); slot.capture->allocate(DRM_FORMAT_XRGB8888); }
        }
    }
    void set_plane(Request& r, const Plane& p, const Buffer& b) {
        r.add(p.id, p.properties, "FB_ID", b.fb);
        r.add(p.id, p.properties, "CRTC_ID", crtc.id);
        r.add(p.id, p.properties, "SRC_X", 0); r.add(p.id, p.properties, "SRC_Y", 0);
        r.add(p.id, p.properties, "SRC_W", std::uint64_t{static_cast<unsigned>(width)} << 16);
        r.add(p.id, p.properties, "SRC_H", std::uint64_t{static_cast<unsigned>(height)} << 16);
        r.add(p.id, p.properties, "CRTC_X", 0); r.add(p.id, p.properties, "CRTC_Y", 0);
        r.add(p.id, p.properties, "CRTC_W", width); r.add(p.id, p.properties, "CRTC_H", height);
        r.optional(p.id, p.properties, "rotation", DRM_MODE_ROTATE_0);
    }
    void configure(Request& r, unsigned index, int* out_fence) {
        require(index < slots.size() && static_cast<int>(index) != current, "cannot submit or overwrite the scanning slot");
        const auto& slot = slots[index];
        if (!active || options.full_state_each_frame) {
            r.add(connector.id, connector.properties, "CRTC_ID", crtc.id);
            if (options.writeback) r.add(writeback.id, writeback.properties, "CRTC_ID", crtc.id);
            r.add(crtc.id, crtc.properties, "MODE_ID", mode_blob);
            r.add(crtc.id, crtc.properties, "ACTIVE", 1);
            r.optional(crtc.id, crtc.properties, "GAMMA_LUT", 0);
            r.optional(crtc.id, crtc.properties, "DEGAMMA_LUT", 0);
            r.optional(crtc.id, crtc.properties, "CTM", 0);
            for (const auto& p : snapshot.planes) {
                if (p.crtc_id == crtc.id && p.id != video.id && p.id != ui.id) {
                    r.add(p.id, p.properties, "FB_ID", 0); r.add(p.id, p.properties, "CRTC_ID", 0);
                }
            }
            set_plane(r, video, *slot.video); set_plane(r, ui, *slot.ui);
            r.enumeration(video.id, video.properties, "COLOR_ENCODING", "ITU-R BT.601 YCbCr");
            r.enumeration(video.id, video.properties, "COLOR_RANGE", "YCbCr full range");
            r.enumeration(ui.id, ui.properties, "pixel blend mode", "Pre-multiplied");
            r.optional(ui.id, ui.properties, "alpha", 65535);
            // Set exposed mutable zpos; fixed order without properties is verified by writeback samples.
            const auto* vz = prop(video.properties, "zpos");
            const auto* uz = prop(ui.properties, "zpos");
            if (vz && uz) {
                const auto low = (vz->flags & DRM_MODE_PROP_IMMUTABLE) ? vz->value : vz->values.front();
                const auto high = (uz->flags & DRM_MODE_PROP_IMMUTABLE) ? uz->value : uz->values.back();
                require(low < high, "no suitable video/UI zpos ordering");
                if (!(vz->flags & DRM_MODE_PROP_IMMUTABLE)) r.add(video.id, video.properties, "zpos", low);
                if (!(uz->flags & DRM_MODE_PROP_IMMUTABLE)) r.add(ui.id, ui.properties, "zpos", high);
            }
        } else {
            r.add(video.id, video.properties, "FB_ID", slot.video->fb);
            r.add(ui.id, ui.properties, "FB_ID", slot.ui->fb);
        }
        if (options.writeback) {
            r.add(writeback.id, writeback.properties, "WRITEBACK_FB_ID", slot.capture->fb);
            if (out_fence) r.add(writeback.id, writeback.properties, "WRITEBACK_OUT_FENCE_PTR", reinterpret_cast<std::uintptr_t>(out_fence));
        }
    }
    void wait_writeback(int timeout) {
        if (fence.value < 0) return;
        pollfd p{fence.value, POLLIN, 0};
        int status;
        do { status = poll(&p, 1, timeout); } while (status < 0 && errno == EINTR);
        check(status, "poll writeback fence");
        require(status != 0, "writeback fence timeout; buffers remain owned until completion");
        sync_file_info info{};
        check(ioctl(fence.value, SYNC_IOC_FILE_INFO, &info), "query writeback fence status");
        require(info.status != 0, "writeback fence woke before completion");
        fence.reset();
        require(info.status > 0, "writeback fence signaled an error");
    }
    void shutdown() {
        wait_writeback(-1); // Never release an in-flight writeback buffer on a timeout path.
        pending = false;
        if (active) {
            Request r;
            for (const auto* p : {&video, &ui}) {
                r.add(p->id, p->properties, "FB_ID", 0); r.add(p->id, p->properties, "CRTC_ID", 0);
            }
            r.add(connector.id, connector.properties, "CRTC_ID", 0);
            if (options.writeback) {
                r.add(writeback.id, writeback.properties, "CRTC_ID", 0);
                r.add(writeback.id, writeback.properties, "WRITEBACK_FB_ID", 0);
            }
            r.add(crtc.id, crtc.properties, "ACTIVE", 0); r.add(crtc.id, crtc.properties, "MODE_ID", 0);
            check(drmModeAtomicCommit(device.value, r.value, DRM_MODE_ATOMIC_ALLOW_MODESET, nullptr), "disable owned CRTC");
            active = false; current = -1;
        }
    }
    ~Impl() {
        if (fence.value >= 0) std::cerr << "Waiting for outstanding writeback before releasing buffers...\n";
        try { shutdown(); } catch (const std::exception& e) { std::cerr << "DRM cleanup: " << e.what() << '\n'; }
        // Destruction order: detach -> FBs/maps/GEM -> mode blob -> device fd.
        for (auto& slot : slots) { slot.capture.reset(); slot.ui.reset(); slot.video.reset(); }
        if (mode_blob) drmModeDestroyPropertyBlob(device.value, mode_blob);
    }
};
AtomicDisplay::AtomicDisplay(const std::string& device, int w, int h, bool take_over, DisplayOptions options) : impl_(std::make_unique<Impl>()) { impl_->initialize(device, w, h, take_over, options); }
AtomicDisplay::~AtomicDisplay() = default;
FrameTargets AtomicDisplay::next_frame() {
    const unsigned next = impl_->current == 0 ? 1 : 0;
    return {next, impl_->slots[next].video->view(), impl_->slots[next].ui->view()};
}
void AtomicDisplay::test_only(unsigned slot) {
    require(!impl_->pending, "previous submission still pending");
    Request request; impl_->configure(request, slot, nullptr);
    check(drmModeAtomicCommit(impl_->device.value, request.value,
        DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET, nullptr), "atomic TEST_ONLY");
}
CapturedImage AtomicDisplay::present(unsigned slot, PresentTiming* timing) {
    submit(slot, timing);
    return finish(timing);
}
void AtomicDisplay::submit(unsigned slot, PresentTiming* timing) {
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    require(!impl_->pending && impl_->fence.value < 0, "previous submission still pending");
    int fence = -1;
    Request request; impl_->configure(request, slot, &fence);
    const auto prepared = Clock::now();
    const int status = drmModeAtomicCommit(impl_->device.value, request.value, DRM_MODE_ATOMIC_ALLOW_MODESET, nullptr);
    const auto committed = Clock::now();
    impl_->fence.reset(fence);
    check(status, "atomic commit");
    impl_->active = true; impl_->current = slot;
    impl_->pending = true;
    if (timing) *timing = {
        std::chrono::duration<double, std::milli>(prepared - start).count(),
        std::chrono::duration<double, std::milli>(committed - prepared).count(), 0};
    if (impl_->options.writeback) require(fence >= 0, "kernel did not return writeback fence");
}
CapturedImage AtomicDisplay::finish(PresentTiming* timing) {
    require(impl_->pending, "no pending submission");
    const auto start = std::chrono::steady_clock::now();
    if (impl_->options.writeback) impl_->wait_writeback(5000);
    impl_->pending = false;
    if (timing) timing->writeback_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
    if (!impl_->options.writeback) return {};
    const auto& b = *impl_->slots[impl_->current].capture;
    return {static_cast<const std::byte*>(b.pixels), b.width, b.height, b.pitch};
}

void AtomicDisplay::shutdown() { impl_->shutdown(); }
std::string AtomicDisplay::description() const {
    return impl_->snapshot.path + " driver=" + impl_->snapshot.driver + " master=1 CRTC=" + std::to_string(impl_->crtc.id) +
        (impl_->options.video_format == VideoFormat::nv12 ? " NV12=" : " XR24=") + std::to_string(impl_->video.id) + " ARGB=" + std::to_string(impl_->ui.id) +
        " writeback=" + std::to_string(impl_->writeback.id) + " " + std::to_string(impl_->width) + "x" + std::to_string(impl_->height) +
        (impl_->options.full_state_each_frame ? " state=full" : " state=delta");
}
} // namespace zenith::display
