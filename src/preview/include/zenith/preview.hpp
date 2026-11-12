#pragma once
#include <memory>
#include <string>
namespace zenith {
class Preview {
public:
    Preview(int width, int height);
    ~Preview();
    Preview(const Preview&) = delete;
    Preview& operator=(const Preview&) = delete;
    bool show(const void* pixels, int pitch);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
