#pragma once
// gzip (RFC 1952) for backups.
//
// GzipWriter compresses on several threads at once, like pigz: the input is cut into 1 MiB pieces, each deflated on
// its own and ended with a sync flush, so the pieces join into one ordinary deflate stream - a single gzip member
// that every gunzip, pigz and TWRP reads. The pieces' CRCs are joined in order.
//
// GunzipReader reads one member or several back to back (`cat a.gz b.gz`, what some tools write), checks every
// member's CRC-32 and length, and allows zero padding after the last one.
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <sys/types.h>

namespace tw {

class GzipWriter {
public:
    using WriteFn = std::function<bool(const void*, size_t)>;  // called on the writing thread, in order
    // threads: 0 = one per CPU core, at most 8.
    explicit GzipWriter(WriteFn out, int level = 1, unsigned threads = 0);
    ~GzipWriter();
    bool write(const void* data, size_t n);
    bool finish();  // the last piece and the trailer
    unsigned threads() const;
    static unsigned defaultThreads();

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

class GunzipReader {
public:
    using ReadFn = std::function<ssize_t(uint8_t*, size_t)>;  // bytes read; 0 = end of input; < 0 = read error
    explicit GunzipReader(ReadFn in);
    ~GunzipReader();
    ssize_t read(void* out, size_t n);  // > 0, 0 at the end, -1 on damaged data (error())
    const std::string& error() const;
    int members() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

}  // namespace tw
