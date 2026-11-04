#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <sys/types.h>
#include <vector>

namespace tw {

// xxHash32 (the checksum of the LZ4 frame format), streaming.
class Xxh32 {
public:
    explicit Xxh32(uint32_t seed = 0) { reset(seed); }
    void reset(uint32_t seed = 0);
    void update(const void* data, size_t n);
    uint32_t digest() const;
    static uint32_t of(const void* data, size_t n, uint32_t seed = 0);

private:
    void stripe(const uint8_t* p);
    uint32_t v_[4] = {}, seed_ = 0;
    uint64_t total_ = 0;
    uint8_t mem_[16] = {};
    size_t memLen_ = 0;
};

// One LZ4 block: appends the decoded bytes to `out` (which may already hold up to 64 KiB of earlier output
// that matches can refer to), at most `limit` new bytes. false + err for damaged data (never reads or writes
// outside the buffers).
bool lz4Block(const uint8_t* src, size_t n, std::vector<uint8_t>& out, size_t limit, std::string& err);

// The LZ4 frame format (what the lz4 tool writes, and Samsung firmware images are packed in): concatenated and
// skippable frames, linked or independent blocks, uncompressed blocks, and every checksum (header, blocks,
// content) is checked.
class Lz4Frame {
public:
    using ReadFn = std::function<size_t(uint8_t*, size_t)>;  // bytes read; 0 = end of input
    explicit Lz4Frame(ReadFn in) : in_(std::move(in)) {}
    // Decoded bytes: > 0, 0 at the end, -1 on damaged data (error()).
    ssize_t read(void* out, size_t n);
    const std::string& error() const { return err_; }

private:
    bool readExact(uint8_t* p, size_t n, bool* cleanEof = nullptr);
    bool nextFrame();
    bool nextBlock();
    bool fail(const std::string& e) { err_ = "lz4: " + e; failed_ = true; return false; }

    ReadFn in_;
    std::vector<uint8_t> out_, blk_;
    size_t outPos_ = 0;
    Xxh32 content_;
    bool inFrame_ = false, done_ = false, failed_ = false;
    bool independent_ = true, blockSum_ = false, contentSum_ = false;
    size_t maxBlock_ = 0;
    int frames_ = 0;
    std::string err_;
};

}  // namespace tw
