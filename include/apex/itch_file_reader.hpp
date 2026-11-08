#pragma once
// Reads ITCH messages one at a time from a file on disk.
//
// NASDAQ sample files use "BinaryFILE" framing: every message is preceded by a
// 2-byte big-endian length. The files are gzip-compressed (many GB uncompressed), so
// we read them through zlib, which also reads plain uncompressed files unchanged.
// That way we never need to unzip the whole file to disk.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct gzFile_s;  // zlib's file handle type (forward-declared to keep zlib out of headers)

namespace apex::itch {

class ItchFileReader {
public:
    explicit ItchFileReader(const std::string& path);
    ~ItchFileReader();

    ItchFileReader(const ItchFileReader&) = delete;
    ItchFileReader& operator=(const ItchFileReader&) = delete;

    bool is_open() const { return file_ != nullptr; }

    // Gets the next message. Returns false at end of file (or if the file ends in the
    // middle of a message, see truncated()). `data` stays valid until the next call.
    bool next(const std::uint8_t*& data, std::size_t& len);

    // True if the file ended partway through a message (a damaged or cut-off file).
    bool truncated() const { return truncated_; }

private:
    // Makes sure at least `need` unread bytes are in the buffer. False if the file ran out.
    bool fill(std::size_t need);

    gzFile_s* file_ = nullptr;
    std::vector<std::uint8_t> buffer_;
    std::size_t begin_ = 0;  // first unread byte in buffer_
    std::size_t end_ = 0;    // one past the last valid byte in buffer_
    bool eof_ = false;
    bool truncated_ = false;
};

}  // namespace apex::itch
