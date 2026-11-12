#include "vaapi.hpp"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
}
#include <cstring>
#include <stdexcept>
namespace zenith::video {
namespace {
void check(int r, const char *action) {
    if (r < 0) {
        char b[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(r, b, sizeof b);
        throw std::runtime_error(std::string(action) + ": " + b);
    }
}
struct Vaapi final : Decoder {
    AVCodecContext *ctx{};
    AVBufferRef *device{};
    AVFrame *hw{};
    AVFrame *sw{};
    bool waiting_key{true}, ended{};
    explicit Vaapi(const std::string &path) {
        try {
            check(av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_VAAPI, path.c_str(), nullptr, 0),
                  "create VAAPI device");
            const auto *codec = avcodec_find_decoder(AV_CODEC_ID_HEVC);
            if (!codec)
                throw std::runtime_error("HEVC decoder unavailable");
            ctx = avcodec_alloc_context3(codec);
            hw = av_frame_alloc();
            sw = av_frame_alloc();
            if (!ctx || !hw || !sw)
                throw std::bad_alloc();
            ctx->hw_device_ctx = av_buffer_ref(device);
            if (!ctx->hw_device_ctx)
                throw std::bad_alloc();
            ctx->get_format = [](AVCodecContext *, const AVPixelFormat *f) {
                for (; *f != AV_PIX_FMT_NONE; ++f)
                    if (*f == AV_PIX_FMT_VAAPI)
                        return *f;
                return AV_PIX_FMT_NONE; // Never silently decode on the CPU.
            };
            ctx->pkt_timebase = {1, 1000000};
            ctx->thread_count = 1;
            check(avcodec_open2(ctx, codec, nullptr), "open HEVC VAAPI");
        } catch (...) {
            release();
            throw;
        }
    }
    void release() {
        av_frame_free(&hw);
        av_frame_free(&sw);
        avcodec_free_context(&ctx);
        av_buffer_unref(&device);
    }
    ~Vaapi() override { release(); }
    void reset() override {
        avcodec_flush_buffers(ctx);
        waiting_key = true;
        ended = false;
    }
    std::vector<std::shared_ptr<const Frame>> receive() {
        std::vector<std::shared_ptr<const Frame>> out;
        while (true) {
            const int r = avcodec_receive_frame(ctx, hw);
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF)
                break;
            check(r, "receive HEVC frame");
            if (hw->format != AV_PIX_FMT_VAAPI)
                throw std::runtime_error("decoder did not produce a VAAPI surface");
            av_frame_unref(sw);
            check(av_hwframe_transfer_data(sw, hw, 0), "download VAAPI surface");
            if (sw->format != AV_PIX_FMT_NV12)
                throw std::runtime_error("this desktop staging path requires 8-bit NV12 (not Main10)");
            auto f = std::make_shared<Frame>();
            f->width = sw->width;
            f->height = sw->height;
            if (f->width <= 0 || f->height <= 0 || f->width % 2 || f->height % 2 || f->width > 4096 ||
                f->height > 2160)
                throw std::runtime_error("unsupported decoded dimensions");
            switch (hw->colorspace) {
            case AVCOL_SPC_BT709:
                f->color.matrix = Matrix::bt709;
                break;
            case AVCOL_SPC_BT470BG:
            case AVCOL_SPC_SMPTE170M:
                f->color.matrix = Matrix::bt601;
                break;
            case AVCOL_SPC_BT2020_NCL:
                f->color.matrix = Matrix::bt2020;
                break;
            default:
                throw std::runtime_error("HEVC must declare supported color matrix metadata");
            }
            if (hw->color_range == AVCOL_RANGE_MPEG)
                f->color.range = Range::limited;
            else if (hw->color_range == AVCOL_RANGE_JPEG)
                f->color.range = Range::full;
            else
                throw std::runtime_error("HEVC must declare color range metadata");
            f->pts_us = hw->best_effort_timestamp;
            f->y_pitch = f->uv_pitch = f->width;
            f->y.resize(static_cast<std::size_t>(f->width) * f->height);
            f->uv.resize(f->y.size() / 2);
            for (int y = 0; y < f->height; ++y)
                std::memcpy(f->y.data() + y * f->width, sw->data[0] + y * sw->linesize[0], f->width);
            for (int y = 0; y < f->height / 2; ++y)
                std::memcpy(f->uv.data() + y * f->width, sw->data[1] + y * sw->linesize[1], f->width);
            out.push_back(std::move(f));
            av_frame_unref(hw);
        }
        return out;
    }
    std::vector<std::shared_ptr<const Frame>> push(const AccessUnit &au) override {
        if (au.discontinuity)
            reset();
        if (ended)
            throw std::runtime_error("decoder drained; reset before submitting another stream");
        if (au.bytes.empty() || au.bytes.size() > (8u << 20))
            throw std::runtime_error("invalid HEVC access unit size");
        if (waiting_key && !is_random_access(au.bytes))
            return {};
        waiting_key = false;
        AVPacket *p = av_packet_alloc();
        if (!p)
            throw std::bad_alloc();
        struct Free {
            AVPacket *&p;
            ~Free() { av_packet_free(&p); }
        } guard{p};
        check(av_new_packet(p, static_cast<int>(au.bytes.size())), "allocate padded HEVC packet");
        std::memcpy(p->data, au.bytes.data(), au.bytes.size());
        p->pts = p->dts = au.pts_us;
        p->duration = au.duration_us;
        // Each push drains every available output, so EAGAIN here is a contract error.
        check(avcodec_send_packet(ctx, p), "send HEVC access unit");
        return receive();
    }
    std::vector<std::shared_ptr<const Frame>> drain() override {
        if (ended)
            return {};
        ended = true;
        check(avcodec_send_packet(ctx, nullptr), "drain HEVC");
        return receive();
    }
};
} // namespace
std::unique_ptr<Decoder> make_vaapi_decoder(const std::string &path) { return std::make_unique<Vaapi>(path); }
} // namespace zenith::video
