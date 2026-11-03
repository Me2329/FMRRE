#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace tw {

class Sha256 {
public:
    Sha256() { reset(); }
    void reset();
    void update(const void* data, size_t len);
    std::string hexDigest();  // finalizes
    std::string digest();     // finalizes; raw 32 bytes
    static std::string toHex(const std::string& bin);

    static std::string hexOfFile(const std::string& path, void (*progress)(uint64_t, uint64_t, void*) = nullptr,
                                 void* ctx = nullptr);
    static std::string hexOf(const std::string& s);

private:
    void block(const uint8_t* p);
    uint32_t h_[8];
    uint8_t buf_[64];
    size_t bufLen_;
    uint64_t total_;
};

}  // namespace tw
