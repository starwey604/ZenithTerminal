#pragma once
#include <optional>
#include <string>
#include <zenith/video.hpp>
namespace zenith::video {
// Local HEVC MP4 only: container PTS drives pacing; Annex-B AUs leave this boundary.
class HevcFile {
  public:
    HevcFile(const std::string &path, bool loop);
    ~HevcFile();
    HevcFile(const HevcFile &) = delete;
    HevcFile &operator=(const HevcFile &) = delete;
    std::optional<AccessUnit> next();
    unsigned loops() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace zenith::video
