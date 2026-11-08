#include "apex/itch_file_reader.hpp"

#include <zlib.h>

#include <cstring>

#include "apex/itch.hpp"

namespace apex::itch {

namespace {
// 1 MiB read buffer. A single ITCH frame is at most 2 + 65535 bytes, so it always fits.
constexpr std::size_t kBufferSize = 1 << 20;
}  // namespace

ItchFileReader::ItchFileReader(const std::string& path) : buffer_(kBufferSize) {
    file_ = gzopen(path.c_str(), "rb");
    if (file_ != nullptr) gzbuffer(file_, kBufferSize);  // bigger zlib buffer = fewer syscalls
}

ItchFileReader::~ItchFileReader() {
    if (file_ != nullptr) gzclose(file_);
}

bool ItchFileReader::fill(std::size_t need) {
    if (end_ - begin_ >= need) return true;  // fast path: already have enough bytes

    // Move the leftover unread bytes to the front so there is room to read more.
    const std::size_t leftover = end_ - begin_;
    std::memmove(buffer_.data(), buffer_.data() + begin_, leftover);
    begin_ = 0;
    end_ = leftover;

    while (end_ < need && !eof_) {
        const int n = gzread(file_, buffer_.data() + end_, static_cast<unsigned>(buffer_.size() - end_));
        if (n <= 0) {
            eof_ = true;  // end of file, or a read/decompress error
            break;
        }
        end_ += static_cast<std::size_t>(n);
    }
    return end_ >= need;
}

bool ItchFileReader::next(const std::uint8_t*& data, std::size_t& len) {
    if (file_ == nullptr) return false;

    // Step 1: the 2-byte length prefix.
    if (!fill(2)) {
        truncated_ = (end_ != begin_);  // a lone leftover byte means the file was cut off
        return false;
    }
    len = read_u16(buffer_.data() + begin_);

    // Step 2: the message body. fill() may move bytes, so take the pointer afterwards.
    if (!fill(2 + len)) {
        truncated_ = true;
        return false;
    }
    data = buffer_.data() + begin_ + 2;
    begin_ += 2 + len;
    return true;
}

}  // namespace apex::itch
