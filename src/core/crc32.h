#pragma once
// CRC-32 (the one in gzip, zip and PNG), eight bytes at a time; and the CRC of two pieces joined, so pieces checked
// on different threads give the CRC of the whole.
#include <cstddef>
#include <cstdint>

namespace tw::crc32 {

uint32_t update(uint32_t crc, const void* data, size_t n);  // crc = update(0, data, n), then update(crc, more, m)
uint32_t combine(uint32_t crcA, uint32_t crcB, uint64_t lenB);  // the CRC of A followed by B

}  // namespace tw::crc32
