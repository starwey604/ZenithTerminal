#pragma once
#include <string>
#include <zenith/video.hpp>
namespace zenith::video {
std::unique_ptr<Decoder> make_vaapi_decoder(const std::string &render_device);
}
