#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace tw {

namespace zipsig { struct PayloadRef; }

class Task;

// A/B OTA packages: the payload.bin inside ROM zips (LineageOS & co. on A/B phones) and full
// factory OTAs. Format: "CrAU" header + DeltaArchiveManifest (protobuf) + data blobs
// (update_engine/update_metadata.proto). Full payloads are supported: REPLACE, REPLACE_BZ,
// REPLACE_XZ, ZERO and DISCARD operations, plus the dm-verity hash tree and FEC that the device
// has to compute itself. Incremental (delta) OTAs need the exact old build and are refused.
namespace payload {

enum OpType : uint32_t {
    kReplace = 0, kReplaceBz = 1, kMove = 2, kBsdiff = 3, kSourceCopy = 4, kSourceBsdiff = 5, kZero = 6,
    kDiscard = 7, kReplaceXz = 8, kPuffdiff = 9, kBrotliBsdiff = 10, kZucchini = 11, kLz4diffBsdiff = 12,
    kLz4diffPuffdiff = 13,
};
const char* opName(uint32_t t);

struct Extent {
    uint64_t start = 0, count = 0;  // blocks
};

struct Op {
    uint32_t type = 0;
    uint64_t dataOffset = 0, dataLength = 0;
    std::vector<Extent> dst;
    std::string dataHash;  // raw SHA-256 of the blob, may be empty
    bool hasSource = false;
    uint64_t dstBytes(uint32_t bs) const;
};

struct PartitionUpdate {
    std::string name;
    uint64_t newSize = 0;
    std::string newHash;      // raw SHA-256 of the whole new partition image
    std::string fsType, version;
    bool hasOldInfo = false;  // delta
    std::vector<Op> ops;
    // dm-verity data the device must compute (empty extents = not needed)
    Extent hashTreeData, hashTree, fecData, fec;
    std::string hashAlgorithm, hashSalt;
    uint32_t fecRoots = 2;
    uint64_t dataBytes(uint32_t bs) const;  // what the operations write
};

struct Group {
    std::string name;
    uint64_t size = 0;
    std::vector<std::string> partitions;
};

struct Payload {
    std::string path;           // .zip or payload.bin
    bool inZip = false;
    uint64_t base = 0;          // offset of payload.bin in the file
    uint64_t size = 0;          // payload.bin size
    uint64_t version = 0, manifestSize = 0;
    uint32_t metadataSigSize = 0;
    uint64_t dataStart = 0;     // blobs, relative to base
    uint64_t sigOffset = 0, sigSize = 0;  // the payload signature blob (relative to dataStart); sigSize 0 = none
    uint32_t blockSize = 4096;
    uint32_t minorVersion = 0;
    bool partial = false;
    int64_t maxTimestamp = 0;
    std::string securityPatch;
    std::vector<PartitionUpdate> parts;
    std::vector<Group> groups;
    bool snapshotEnabled = false, vabc = false;
    std::map<std::string, std::string> metadata;    // META-INF/com/android/metadata (zips)
    std::map<std::string, std::string> properties;  // payload_properties.txt (zips)

    bool full() const;                     // no operation needs the old build
    std::vector<std::string> unsupported() const;  // operation types we can't apply, e.g. "SOURCE_BSDIFF"
    bool isDynamic(const std::string& part) const;
    const PartitionUpdate* find(const std::string& name) const;
    uint64_t totalWriteBytes() const;
    std::string device() const;            // pre-device from the zip metadata
    std::string describe() const;          // "LineageOS 22.1 · full OTA · 14 partitions · 1.2 GB"
};

// Where its signatures are, for zipsig::verifyPayload.
zipsig::PayloadRef signatureRef(const Payload& p);

// Accepts an OTA .zip (payload.bin must be stored uncompressed, as ota_from_target_files
// writes it) or a bare payload.bin. Parses and sanity-checks the manifest.
bool open(const std::string& path, Payload& p, std::string& err);
bool isOtaZip(const std::string& path);

// Write partition images (<outDir>/<name>.img), including the computed verity data, and check
// them against the payload's hashes. Empty `names` = all partitions.
bool extract(const Payload& p, const std::vector<std::string>& names, const std::string& outDir, bool verify, Task& t);

struct InstallOptions {
    std::string slot;          // "_a" / "_b"; empty = the inactive slot (what update_engine does)
    bool switchSlot = true;    // make the target slot active afterwards
    bool verify = true;        // check each partition's hash after writing it
    bool force = false;        // ignore a device-name mismatch
    bool freeSourceIfNeeded = true;  // Virtual A/B: drop the current slot's partitions if super is too small
    std::string historyNote;   // added to the flash history entry (the zip's signature)
};
// What install() would do, for the confirmation screen.
struct Plan {
    bool ok = false;
    std::string error;
    bool deviceMismatch = false;        // only the device name check failed (can be overridden)
    std::string targetSlot;
    std::vector<std::string> notes;     // warnings to show before installing
    bool needsFreeSource = false;       // super only fits after removing the current slot's partitions
    uint64_t superNeeded = 0, superFree = 0;
};
Plan plan(const Payload& p, const InstallOptions& o);
bool install(const Payload& p, const InstallOptions& o, Task& t);

}  // namespace payload
}  // namespace tw
