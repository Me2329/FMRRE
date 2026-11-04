#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace tw {

// SHA-1, only to check signatures made with it: Android's recovery picks the hash of an OTA signature from the
// signing certificate, and old keys - the public AOSP test keys among them - give SHA-1.
class Sha1 {
public:
    Sha1() { reset(); }
    void reset();
    void update(const void* data, size_t len);
    std::string digest();  // finalizes; raw 20 bytes

private:
    void block(const uint8_t* p);
    uint32_t h_[5];
    uint8_t buf_[64];
    size_t bufLen_;
    uint64_t total_;
};

}  // namespace tw
