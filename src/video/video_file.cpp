#include <zenith/video_file.hpp>
extern "C" {
#include <libavcodec/bsf.h>
#include <libavformat/avformat.h>
}
#include <filesystem>
#include <fstream>
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
} // namespace
struct HevcFile::Impl {
    std::string path;
    bool loop, eof{}, first{true};
    unsigned cycles{};
    AVFormatContext *input{};
    AVBSFContext *bsf{};
    AVPacket *packet{};
    int index{};
    std::int64_t base{}, first_pts{AV_NOPTS_VALUE}, end{};
    Impl(std::string p, bool l) : path(std::move(p)), loop(l) {
        try {
            open();
        } catch (...) {
            close();
            throw;
        }
    }
    ~Impl() { close(); }
    void close() {
        av_packet_free(&packet);
        av_bsf_free(&bsf);
        avformat_close_input(&input);
    }
    void open() {
        if (!std::filesystem::is_regular_file(path))
            throw std::runtime_error("video file not found: " + path);
        std::ifstream f(path);
        std::string head(42, '\0');
        f.read(head.data(), head.size());
        head.resize(static_cast<std::size_t>(f.gcount()));
        if (head.starts_with("version https://git-lfs.github.com/spec/v1"))
            throw std::runtime_error("video is a Git LFS pointer; run git lfs pull");
        check(avformat_open_input(&input, path.c_str(), nullptr, nullptr), "open video file");
        check(avformat_find_stream_info(input, nullptr), "read video stream");
        index = av_find_best_stream(input, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        check(index, "find video stream");
        auto *s = input->streams[index];
        if (s->codecpar->codec_id != AV_CODEC_ID_HEVC || s->codecpar->video_delay != 0)
            throw std::runtime_error("replay requires HEVC without B frames");
        const auto *filter = av_bsf_get_by_name("hevc_mp4toannexb");
        if (!filter)
            throw std::runtime_error("hevc_mp4toannexb unavailable");
        check(av_bsf_alloc(filter, &bsf), "allocate Annex-B filter");
        check(avcodec_parameters_copy(bsf->par_in, s->codecpar), "copy HEVC parameters");
        bsf->time_base_in = s->time_base;
        check(av_bsf_init(bsf), "initialize Annex-B filter");
        packet = av_packet_alloc();
        if (!packet)
            throw std::bad_alloc();
        eof = false;
        first = true;
        first_pts = AV_NOPTS_VALUE;
    }
    std::optional<AccessUnit> next() {
        for (;;) {
            int r = av_bsf_receive_packet(bsf, packet);
            if (r == 0) {
                if (packet->pts == AV_NOPTS_VALUE || packet->duration <= 0)
                    throw std::runtime_error("replay needs container PTS/duration; use the MP4 fixture");
                const auto pts = av_rescale_q(packet->pts, bsf->time_base_out, AVRational{1, 1000000});
                const auto duration =
                    av_rescale_q(packet->duration, bsf->time_base_out, AVRational{1, 1000000});
                if (first_pts == AV_NOPTS_VALUE)
                    first_pts = pts;
                AccessUnit au;
                au.bytes.assign(packet->data, packet->data + packet->size);
                au.pts_us = base + pts - first_pts;
                au.duration_us = duration;
                au.discontinuity = first;
                first = false;
                end = au.pts_us + duration;
                av_packet_unref(packet);
                return au;
            }
            if (r == AVERROR_EOF) {
                if (!loop)
                    return {};
                if (first)
                    throw std::runtime_error("video contains no access units");
                base = end;
                ++cycles;
                close();
                open();
                continue;
            }
            if (r != AVERROR(EAGAIN))
                check(r, "read Annex-B AU");
            if (eof)
                throw std::runtime_error("Annex-B filter failed to drain");
            for (;;) {
                r = av_read_frame(input, packet);
                if (r < 0 || packet->stream_index == index)
                    break;
                av_packet_unref(packet);
            }
            if (r == AVERROR_EOF) {
                eof = true;
                check(av_bsf_send_packet(bsf, nullptr), "drain Annex-B filter");
            } else {
                check(r, "read video packet");
                check(av_bsf_send_packet(bsf, packet), "convert HEVC packet");
            }
        }
    }
};
HevcFile::HevcFile(const std::string &p, bool l) : impl_(std::make_unique<Impl>(p, l)) {}
HevcFile::~HevcFile() = default;
std::optional<AccessUnit> HevcFile::next() { return impl_->next(); }
unsigned HevcFile::loops() const { return impl_->cycles; }
} // namespace zenith::video
