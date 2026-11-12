#include <zenith/display/probe.hpp>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <memory>
#include <stdexcept>
#include <unistd.h>

namespace zenith::display {
namespace {
[[noreturn]] void fail(const std::string& operation) {
    const int error = errno;
    throw std::runtime_error(operation + ": " + std::strerror(error));
}
class DeviceFd {
public:
    explicit DeviceFd(const std::string& path) : fd_(open(path.c_str(), O_RDONLY | O_CLOEXEC)) {
        if (fd_ < 0) fail("open " + path);
    }
    ~DeviceFd() { close(fd_); }
    DeviceFd(const DeviceFd&) = delete;
    DeviceFd& operator=(const DeviceFd&) = delete;
    int get() const { return fd_; }
private:
    int fd_;
};
template<class T, auto Free> using DrmPtr = std::unique_ptr<T, decltype(Free)>;
std::string get_driver(int fd) {
    DrmPtr<drmVersion, drmFreeVersion> version(drmGetVersion(fd), drmFreeVersion);
    if (!version) fail("drmGetVersion");
    return std::string(version->name, version->name_len);
}
std::vector<Property> properties(int fd, std::uint32_t id, std::uint32_t type) {
    DrmPtr<drmModeObjectProperties, drmModeFreeObjectProperties> list(
        drmModeObjectGetProperties(fd, id, type), drmModeFreeObjectProperties);
    if (!list) fail("drmModeObjectGetProperties " + std::to_string(id));
    std::vector<Property> result;
    for (std::uint32_t i = 0; i < list->count_props; ++i) {
        DrmPtr<drmModePropertyRes, drmModeFreeProperty> p(
            drmModeGetProperty(fd, list->props[i]), drmModeFreeProperty);
        if (!p) fail("drmModeGetProperty");
        Property value{p->prop_id, p->flags, p->name, list->prop_values[i], {}, {}};
        for (int j = 0; j < p->count_values; ++j) value.values.push_back(p->values[j]);
        for (int j = 0; j < p->count_enums; ++j)
            value.enums.push_back({p->enums[j].value, p->enums[j].name});
        result.push_back(std::move(value));
    }
    return result;
}
const Property* find_property(const std::vector<Property>& properties, const std::string& name) {
    const auto it = std::find_if(properties.begin(), properties.end(),
        [&](const auto& p) { return p.name == name; });
    return it == properties.end() ? nullptr : &*it;
}
std::vector<std::byte> get_blob(int fd, std::uint64_t id) {
    if (id > UINT32_MAX) throw std::runtime_error("invalid DRM blob id");
    DrmPtr<drmModePropertyBlobRes, drmModeFreePropertyBlob> blob(
        drmModeGetPropertyBlob(fd, static_cast<std::uint32_t>(id)), drmModeFreePropertyBlob);
    if (!blob) fail("drmModeGetPropertyBlob");
    if (!blob->length) return {};
    const auto* data = static_cast<const std::byte*>(blob->data);
    return {data, data + blob->length};
}
}
std::vector<std::string> card_paths() {
    std::vector<std::string> result;
    if (!std::filesystem::exists("/dev/dri")) return result;
    for (const auto& entry : std::filesystem::directory_iterator("/dev/dri")) {
        const auto name = entry.path().filename().string();
        if (name.starts_with("card") && name.size() > 4 &&
            name.find_first_not_of("0123456789", 4) == std::string::npos)
            result.push_back(entry.path().string());
    }
    std::sort(result.begin(), result.end());
    return result;
}
std::string driver_name(const std::string& path) {
    const DeviceFd device(path);
    return get_driver(device.get());
}
DeviceSnapshot probe_device(const std::string& path) {
    const DeviceFd device(path);
    const int fd = device.get();
    DeviceSnapshot result;
    result.path = path;
    result.driver = get_driver(fd);
    auto client_cap = [&](std::uint64_t cap, const char* name) {
        if (drmSetClientCap(fd, cap, 1) == 0) return true;
        result.warnings.push_back(std::string(name) + ": " + std::strerror(errno));
        return false;
    };
    // These flags affect this fd's query visibility, never display configuration.
    result.universal_planes = client_cap(DRM_CLIENT_CAP_UNIVERSAL_PLANES, "universal planes");
    result.atomic = client_cap(DRM_CLIENT_CAP_ATOMIC, "atomic");
    result.writeback_connectors = client_cap(DRM_CLIENT_CAP_WRITEBACK_CONNECTORS, "writeback visibility");
    for (const auto& [id, name] : std::vector<std::pair<std::uint64_t, const char*>>{
             {DRM_CAP_DUMB_BUFFER, "DUMB_BUFFER"}, {DRM_CAP_PRIME, "PRIME (bit0=import, bit1=export)"},
             {DRM_CAP_ADDFB2_MODIFIERS, "ADDFB2_MODIFIERS"}, {DRM_CAP_CRTC_IN_VBLANK_EVENT, "CRTC_IN_VBLANK_EVENT"}}) {
        std::uint64_t value = 0;
        const bool available = drmGetCap(fd, id, &value) == 0;
        result.capabilities.push_back({name, available, value});
    }
    DrmPtr<drmModeRes, drmModeFreeResources> resources(drmModeGetResources(fd), drmModeFreeResources);
    if (!resources) fail("drmModeGetResources");
    for (int i = 0; i < resources->count_crtcs; ++i) {
        const auto id = resources->crtcs[i];
        result.crtcs.push_back({id, static_cast<std::uint32_t>(i), properties(fd, id, DRM_MODE_OBJECT_CRTC)});
    }
    for (int i = 0; i < resources->count_encoders; ++i) {
        DrmPtr<drmModeEncoder, drmModeFreeEncoder> encoder(
            drmModeGetEncoder(fd, resources->encoders[i]), drmModeFreeEncoder);
        if (!encoder) fail("drmModeGetEncoder");
        result.encoders.push_back({encoder->encoder_id, encoder->crtc_id, encoder->possible_crtcs});
    }
    for (int i = 0; i < resources->count_connectors; ++i) {
        // Current avoids forcing a monitor reprobe on somebody else's display.
        DrmPtr<drmModeConnector, drmModeFreeConnector> connector(
            drmModeGetConnectorCurrent(fd, resources->connectors[i]), drmModeFreeConnector);
        if (!connector) fail("drmModeGetConnectorCurrent");
        Connector c;
        c.id = connector->connector_id;
        c.encoder_id = connector->encoder_id;
        const char* type = drmModeGetConnectorTypeName(connector->connector_type);
        c.name = std::string(type ? type : "Unknown") + "-" + std::to_string(connector->connector_type_id);
        c.connection = connector->connection == DRM_MODE_CONNECTED ? "connected" :
            connector->connection == DRM_MODE_DISCONNECTED ? "disconnected" : "unknown";
        c.writeback = connector->connector_type == DRM_MODE_CONNECTOR_WRITEBACK;
        for (int j = 0; j < connector->count_encoders; ++j) {
            c.encoders.push_back(connector->encoders[j]);
            for (const auto& e : result.encoders)
                if (e.id == connector->encoders[j]) c.possible_crtcs |= e.possible_crtcs;
        }
        for (int j = 0; j < connector->count_modes; ++j) {
            const auto& m = connector->modes[j];
            c.modes.push_back({m.name, m.hdisplay, m.vdisplay, m.vrefresh,
                (m.type & DRM_MODE_TYPE_PREFERRED) != 0});
        }
        c.properties = properties(fd, c.id, DRM_MODE_OBJECT_CONNECTOR);
        if (const auto* p = find_property(c.properties, "WRITEBACK_PIXEL_FORMATS"); p && p->value)
            c.writeback_formats = parse_writeback_formats(get_blob(fd, p->value));
        result.connectors.push_back(std::move(c));
    }
    DrmPtr<drmModePlaneRes, drmModeFreePlaneResources> planes(
        drmModeGetPlaneResources(fd), drmModeFreePlaneResources);
    if (!planes) fail("drmModeGetPlaneResources");
    for (std::uint32_t i = 0; i < planes->count_planes; ++i) {
        DrmPtr<drmModePlane, drmModeFreePlane> plane(drmModeGetPlane(fd, planes->planes[i]), drmModeFreePlane);
        if (!plane) fail("drmModeGetPlane");
        Plane p;
        p.id = plane->plane_id;
        p.crtc_id = plane->crtc_id;
        p.fb_id = plane->fb_id;
        p.possible_crtcs = plane->possible_crtcs;
        p.type = "unknown";
        for (std::uint32_t j = 0; j < plane->count_formats; ++j) p.formats.push_back(plane->formats[j]);
        p.properties = properties(fd, p.id, DRM_MODE_OBJECT_PLANE);
        if (const auto* prop = find_property(p.properties, "type"))
            for (const auto& [value, name] : prop->enums) if (value == prop->value) p.type = name;
        if (const auto* prop = find_property(p.properties, "IN_FORMATS"); prop && prop->value) {
            p.has_in_formats = true;
            p.format_modifiers = parse_format_modifiers(get_blob(fd, prop->value));
        }
        result.planes.push_back(std::move(p));
    }
    return result;
}
} // namespace zenith::display
