#pragma once
#include <cstdint>
#include <functional>
#include <string>

namespace tw::verity {

// Since Android 10, OTA payloads usually leave out the dm-verity hash tree and the
// FEC (error correction) data of system/vendor/product...: update_engine computes
// them on the device after writing the partition. These functions do the same, in
// the AVB layout (avbtool + the `fec` tool), so the result is byte-identical to the
// factory image and matches the partition hash in the payload.

using Progress = std::function<void(uint64_t done, uint64_t total)>;

// Size of the hash tree for `dataSize` bytes (avbtool calc_hash_level_offsets).
uint64_t hashTreeSize(uint64_t dataSize, uint32_t blockSize, uint32_t digestSize = 32);

// Hash the `dataSize` bytes at `dataOffset` of `fd` and write the tree (top level first) at
// `treeOffset`. algorithm: "sha256" (the only one AVB images use in practice). `root` gets the raw
// root digest, which is what vbmeta's hashtree descriptor stores.
bool buildHashTree(int fd, uint64_t dataOffset, uint64_t dataSize, uint64_t treeOffset, uint64_t treeSize,
                   uint32_t blockSize, const std::string& algorithm, const std::string& salt, std::string& root,
                   std::string& err, const Progress& progress = nullptr);

// Reed-Solomon RS(255, 255 - roots) parity over the interleaved data, as libfec / update_engine's
// VerityWriterAndroid::EncodeFEC lay it out. Covers `dataSize` bytes at `dataOffset` (the file
// system plus the hash tree) and writes `fecSize` bytes at `fecOffset`.
bool encodeFec(int fd, uint64_t dataOffset, uint64_t dataSize, uint64_t fecOffset, uint64_t fecSize, uint32_t roots,
               uint32_t blockSize, std::string& err, const Progress& progress = nullptr);
uint64_t fecSize(uint64_t dataSize, uint32_t roots, uint32_t blockSize);

// Exposed for the self-tests: parity of one RS message (255 - roots bytes).
void rsEncode(uint32_t roots, const uint8_t* msg, uint8_t* parity);
// Syndromes of a full codeword (message + parity), all zero when it is valid.
bool rsCheck(uint32_t roots, const uint8_t* codeword);

}  // namespace tw::verity
