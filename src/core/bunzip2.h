#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <sys/types.h>
#include <vector>

namespace tw {

// Streaming bzip2 decompressor, written from the format description (BWT + MTF + Huffman,
// with the initial run-length stage). Used for REPLACE_BZ operations in OTA payloads.
// Memory: one block (up to 900 kB) as 32-bit entries, about 3.6 MB.
// Concatenated streams (pbzip2 output) are decoded as one. Block and stream CRCs are checked.
// Randomised blocks (written only by bzip2 0.9.0 and older) are rejected.
class Bunzip2 {
public:
    // Returns the number of bytes read into the buffer, 0 at the end of the input, -1 on error.
    using Reader = std::function<ssize_t(uint8_t*, size_t)>;
    explicit Bunzip2(Reader in);

    // Bytes produced, 0 at the end of the stream(s), -1 on error (see error()).
    ssize_t read(uint8_t* out, size_t n);
    const std::string& error() const { return err_; }
    bool finished() const { return state_ == State::End; }

    // Convenience: whole buffer in memory. `maxOut` guards against decompression bombs.
    static bool decode(const uint8_t* in, size_t n, std::string& out, std::string& err, size_t maxOut = size_t(1) << 31);

private:
    enum class State { Header, BlockStart, Output, End, Error };
    bool fill();
    bool need(int n);
    uint32_t take(int n);  // after need(n)
    bool bits(int n, uint32_t& v);
    bool readStreamHeader(bool first);
    bool readBlock();
    bool fail(const char* what);

    Reader in_;
    std::vector<uint8_t> inBuf_;
    size_t inPos_ = 0, inLen_ = 0;
    bool inEof_ = false;
    uint64_t bitBuf_ = 0;
    int bitCount_ = 0;

    State state_ = State::Header;
    std::string err_;
    uint32_t blockMax_ = 0;
    std::vector<uint32_t> tt_;

    // current block being emitted
    uint32_t tPos_ = 0, left_ = 0;
    uint32_t blockCrc_ = 0, crc_ = 0, streamCrc_ = 0;
    int last_ = -1, run_ = 0, repeat_ = 0;
};

}  // namespace tw
