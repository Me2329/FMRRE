#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <sys/types.h>
#include <vector>

namespace tw {

// xxHash64 (the content checksum of Zstandard frames), streaming.
class Xxh64 {
public:
    explicit Xxh64(uint64_t seed = 0) { reset(seed); }
    void reset(uint64_t seed = 0);
    void update(const void* data, size_t n);
    uint64_t digest() const;
    static uint64_t of(const void* data, size_t n, uint64_t seed = 0);

private:
    uint64_t v_[4] = {}, seed_ = 0, total_ = 0;
    uint8_t mem_[32] = {};
    size_t memLen_ = 0;
};

// Zstandard (RFC 8878) - what the zstd tool writes: raw, RLE and compressed blocks (Huffman-coded literals,
// FSE-coded sequences, repeat offsets, tables repeated from the previous block), concatenated and skippable
// frames, the frame content size and the content checksum. Frames that need a dictionary are refused (images are
// never made with one). Damaged input is refused without reading or writing outside the buffers.
class ZstdFrame {
public:
    using ReadFn = std::function<size_t(uint8_t*, size_t)>;  // bytes read; 0 = end of input
    // maxWindow: the largest history a frame may ask for (zstd --long / --ultra frames ask for more).
    explicit ZstdFrame(ReadFn in, size_t maxWindow = size_t(256) << 20);
    ~ZstdFrame();
    // Decoded bytes: > 0, 0 at the end, -1 on damaged data (error()).
    ssize_t read(void* out, size_t n);
    const std::string& error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

}  // namespace tw
