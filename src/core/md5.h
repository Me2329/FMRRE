#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace tw {

// MD5, only to check the .md5 digests of classic TWRP backups (TWRP 3.0 / 3.1; later ones write .sha2).
class Md5 {
public:
    Md5() { reset(); }
    void reset();
    void update(const void* data, size_t len);
    std::string digest();  // finalizes; raw 16 bytes
    static std::string hexOfFile(const std::string& path);

private:
    void block(const uint8_t* p);
    uint32_t h_[4];
    uint8_t buf_[64];
    size_t bufLen_;
    uint64_t total_;
};

}  // namespace tw
