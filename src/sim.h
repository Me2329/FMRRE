#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace tw::sim {
// Create (once) a fake device filesystem used by --sim mode.
void ensureRoot(const std::string& root);

// ---- A/B OTA builder (demo ROM in the sandbox, self-tests) -------------------------
struct OtaOp {
    uint32_t type = 0;               // payload::OpType
    uint64_t start = 0, count = 0;   // destination blocks
    std::string blob;                // data as stored in the payload (raw / bzip2 / xz)
};
struct OtaPart {
    std::string name;
    uint64_t size = 0;               // final partition image size
    std::string hash;                // raw SHA-256 of the final image
    std::vector<OtaOp> ops;
    // dm-verity regions the device computes (blocks); treeCount == 0: none
    uint64_t dataBlocks = 0, treeCount = 0, fecCount = 0;
    std::string salt;
};
// REPLACE / ZERO operations for `data`; with `verity`, a hash tree + FEC (2 roots) + one footer
// block are appended to the image but left out of the operations, as in real OTAs.
bool otaPartFromImage(const std::string& name, std::string data, bool verity, const std::string& tmpFile, OtaPart& out,
                      std::string& err);
// payload.bin: dynamic partitions are the ones listed in `group`.
std::string encodePayload(const std::vector<OtaPart>& parts, const std::string& group, uint64_t groupSize,
                          const std::vector<std::string>& dynamicNames, const std::string& securityPatch);
// OTA zip: payload.bin stored uncompressed + payload_properties.txt + META-INF/com/android/metadata.
bool writeOtaZip(const std::string& path, const std::string& payload, const std::string& metadata);
}  // namespace tw::sim
