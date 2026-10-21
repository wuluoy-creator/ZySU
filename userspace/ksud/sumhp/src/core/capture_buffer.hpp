#pragma once

#include <algorithm>
#include <limits>
#include <streambuf>
#include <string>

namespace sumhp {
// Discard excess output without interrupting an already-running mutation.
// The receipt separately reports completeness; discard never means success.
class CaptureBuffer final : public std::streambuf {
public:
    explicit CaptureBuffer(std::size_t limit = std::numeric_limits<std::size_t>::max())
        : limit_(limit) {}
    const std::string& str() const { return bytes_; }
    bool complete() const { return complete_; }

protected:
    std::streamsize xsputn(const char* data, std::streamsize count) override {
        if (count <= 0)
            return 0;
        const auto size = static_cast<std::size_t>(count);
        const auto kept = std::min(size, limit_ - bytes_.size());
        bytes_.append(data, kept);
        if (kept != size)
            complete_ = false;
        return count;
    }
    int_type overflow(int_type value) override {
        if (traits_type::eq_int_type(value, traits_type::eof()))
            return traits_type::not_eof(value);
        const auto character = traits_type::to_char_type(value);
        xsputn(&character, 1);
        return value;
    }

private:
    std::size_t limit_;
    std::string bytes_;
    bool complete_ = true;
};
}  // namespace sumhp
