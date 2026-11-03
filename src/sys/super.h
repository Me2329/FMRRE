#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace tw {

class Task;

// Dynamic partitions: reads and rewrites the LP ("liblp") metadata at the start
// of the `super` partition and keeps the device-mapper tables in sync, like
// fastbootd's resize-logical-partition / delete-logical-partition do.
namespace lp {

constexpr uint32_t kAttrReadonly = 1, kAttrSlotSuffixed = 2, kAttrUpdated = 4, kAttrDisabled = 8;
constexpr uint32_t kTargetLinear = 0, kTargetZero = 1;
constexpr uint32_t kHeaderFlagVirtualAB = 1;

struct Extent {
    uint64_t sectors = 0;      // 512-byte sectors
    uint32_t type = kTargetLinear;
    uint64_t target = 0;       // physical sector on the block device
    uint32_t source = 0;       // block device index
};

struct Part {
    std::string name;
    uint32_t attrs = 0;
    uint32_t group = 0;
    std::vector<Extent> extents;
    uint64_t sectors() const;
    uint64_t bytes() const { return sectors() * 512; }
};

struct Group {
    std::string name;
    uint32_t flags = 0;
    uint64_t maxSize = 0;  // 0 = unlimited
};

struct BlockDev {
    uint64_t firstSector = 0;  // first usable sector (after the metadata)
    uint32_t alignment = 0, alignmentOffset = 0;
    uint64_t size = 0;
    std::string name;
    uint32_t flags = 0;
};

struct Region { uint64_t start = 0, sectors = 0; };

struct Metadata {
    // geometry
    uint32_t maxSize = 65536, slotCount = 2, blockSize = 4096;
    // header
    uint16_t major = 10, minor = 0;
    uint32_t headerSize = 128, headerFlags = 0;

    std::vector<Part> parts;
    std::vector<Group> groups;
    std::vector<BlockDev> devs;

    Part* find(const std::string& name);
    const Part* find(const std::string& name) const;
    int groupIndex(const std::string& name) const;
    uint64_t groupUsed(uint32_t g) const;
    uint64_t totalBytes() const;  // usable bytes of the super device(s)
    uint64_t usedBytes() const;
    std::vector<Region> freeRegions(uint32_t dev = 0) const;
    uint64_t freeBytes() const;   // bytes that can still be allocated
    // How far `name` can grow: limited by free space and by its group's maximum size.
    uint64_t maxGrowBytes(const std::string& name) const;

    bool resize(const std::string& name, uint64_t bytes, std::string& err);
    bool remove(const std::string& name, std::string& err);
    bool add(const std::string& name, const std::string& group, uint32_t attrs, std::string& err);
    bool validate(std::string& err) const;

    std::string serialize() const;  // header + tables, checksummed
};

std::string superDevice();
uint32_t currentSlotIndex();

bool read(const std::string& dev, uint32_t slot, Metadata& m, std::string& err);
// Writes `m` to every metadata slot (backup copy first, then primary), then reads it back.
bool write(const std::string& dev, const Metadata& m, std::string& err);
// Lay out a fresh super (geometry + metadata in all slots). Used by the simulator and tests.
bool format(const std::string& dev, const Metadata& m, std::string& err);

// Raw copy of everything before the first usable sector (geometry + all metadata slots).
bool saveMetadataRegion(const std::string& dev, const std::string& file, std::string& err);
bool restoreMetadataRegion(const std::string& dev, const std::string& file, std::string& err);

// Refuse to touch super while a Virtual A/B update is pending or merging.
bool busy(std::string& why);

// ---- device-mapper ------------------------------------------------------
std::string mappedPath(const std::string& name);  // "" if not mapped
bool map(const Metadata& m, const std::string& name, bool readWrite, std::string& err);
bool unmap(const std::string& name, std::string& err);
// For /dev/block/mapper/<name> (or dm-N): reload the mapping read-write if it is read-only.
bool ensureWritable(const std::string& blockDev, std::string& err);

// ---- high level operations (log into the task, back up metadata first) ---
bool resizePartition(const std::string& name, uint64_t bytes, Task& t);
bool deletePartition(const std::string& name, Task& t);
bool createPartition(const std::string& name, const std::string& group, uint64_t bytes, Task& t);
// Shrink every partition of the other slot to zero (frees space for a GSI on virtual A/B devices).
bool freeOtherSlot(Task& t);
// Where automatic metadata backups go.
std::string backupDir();

// ---- whole-slot layout (A/B OTA install) ----------------------------------
struct SlotGroup {
    std::string name;     // without the slot suffix, e.g. "qti_dynamic_partitions"
    uint64_t maxSize = 0;
    std::vector<std::pair<std::string, uint64_t>> parts;  // names without suffix, new sizes
};
struct SlotFit {
    bool fits = false;
    bool needsFreeSource = false;  // only fits after removing the other slot's partitions
    uint64_t needed = 0, available = 0;
    std::string error;
};
// Like update_engine before a full OTA: every partition of these groups in slot `suffix` is
// removed and re-created with the new size (and the groups get the payload's maximum sizes).
SlotFit fitSlot(const std::vector<SlotGroup>& groups, const std::string& suffix);
// freeSource: Virtual A/B devices only have room for one slot in super; if needed, empty the
// other slot's partitions first (Android's own recovery does the same for full OTAs).
// Nothing is written when the layout doesn't fit. The new partitions are mapped read-write.
bool prepareSlot(const std::vector<SlotGroup>& groups, const std::string& suffix, bool freeSource, Task& t);

}  // namespace lp
}  // namespace tw
