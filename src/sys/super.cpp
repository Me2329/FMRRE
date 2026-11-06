#include "sys/super.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/dm-ioctl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <linux/fs.h>
#include <unistd.h>

#include "core/log.h"
#include "core/sha256.h"
#include "core/task.h"
#include "core/util.h"
#include "platform/device.h"
#include "sys/partitions.h"

namespace tw::lp {

// On-disk layout (system/core/fs_mgr/liblp/include/liblp/metadata_format.h).
namespace {

constexpr uint32_t kGeometryMagic = 0x616c4467;
constexpr uint32_t kHeaderMagic = 0x414C5030;
constexpr uint64_t kReserved = 4096, kGeometrySize = 4096;
constexpr size_t kGeometryStructSize = 52;
constexpr size_t kPartEntry = 52, kExtentEntry = 24, kGroupEntry = 48, kDevEntry = 64;

uint64_t rd(const uint8_t* p, int n) {
    uint64_t v = 0;
    for (int i = n - 1; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}
void wr(std::string& s, size_t off, uint64_t v, int n) {
    for (int i = 0; i < n; i++) s[off + i] = char(v >> (8 * i));
}
std::string name36(const uint8_t* p) { return std::string((const char*)p, strnlen((const char*)p, 36)); }
void putName(std::string& s, size_t off, const std::string& n) { memcpy(&s[off], n.data(), std::min<size_t>(n.size(), 35)); }

// Raw 32-byte SHA-256 (Sha256 gives hex).
std::string sha(const std::string& data) {
    Sha256 h;
    h.update(data.data(), data.size());
    std::string hex = h.hexDigest(), bin(32, '\0');
    for (int i = 0; i < 32; i++) bin[i] = (char)std::stoi(hex.substr(i * 2, 2), nullptr, 16);
    return bin;
}

uint64_t primaryOffset(const Metadata& m, uint32_t slot) { return kReserved + kGeometrySize * 2 + uint64_t(slot) * m.maxSize; }
uint64_t backupOffset(const Metadata& m, uint32_t slot) {
    return kReserved + kGeometrySize * 2 + uint64_t(m.slotCount) * m.maxSize + uint64_t(slot) * m.maxSize;
}

bool preadAll(int fd, void* buf, size_t n, uint64_t off) {
    char* p = (char*)buf;
    while (n) {
        ssize_t r = pread(fd, p, n, off);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return false;
        p += r; n -= r; off += r;
    }
    return true;
}
bool pwriteAll(int fd, const void* buf, size_t n, uint64_t off) {
    const char* p = (const char*)buf;
    while (n) {
        ssize_t r = pwrite(fd, p, n, off);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return false;
        p += r; n -= r; off += r;
    }
    return true;
}

bool parseGeometry(const uint8_t* g, Metadata& m, std::string& err) {
    if (rd(g, 4) != kGeometryMagic) { err = "no LP geometry (not a super partition)"; return false; }
    uint32_t ss = (uint32_t)rd(g + 4, 4);
    if (ss < kGeometryStructSize || ss > kGeometrySize) { err = "bad geometry size"; return false; }
    std::string copy((const char*)g, ss);
    memset(&copy[8], 0, 32);
    if (sha(copy) != std::string((const char*)g + 8, 32)) { err = "geometry checksum mismatch"; return false; }
    m.maxSize = (uint32_t)rd(g + 40, 4);
    m.slotCount = (uint32_t)rd(g + 44, 4);
    m.blockSize = (uint32_t)rd(g + 48, 4);
    if (m.maxSize < 512 || m.maxSize % 512 || m.slotCount == 0 || m.slotCount > 8 || m.blockSize < 512) {
        err = "geometry values out of range";
        return false;
    }
    return true;
}

bool parseMetadata(const std::string& blob, Metadata& m, std::string& err) {
    const uint8_t* h = (const uint8_t*)blob.data();
    if (blob.size() < 128 || rd(h, 4) != kHeaderMagic) { err = "bad metadata header magic"; return false; }
    m.major = (uint16_t)rd(h + 4, 2);
    m.minor = (uint16_t)rd(h + 6, 2);
    m.headerSize = (uint32_t)rd(h + 8, 4);
    if (m.major != 10 || m.headerSize < 128 || m.headerSize > blob.size()) { err = "unsupported metadata version"; return false; }
    std::string hdr = blob.substr(0, m.headerSize);
    memset(&hdr[12], 0, 32);
    if (sha(hdr) != blob.substr(12, 32)) { err = "metadata header checksum mismatch"; return false; }
    uint32_t tablesSize = (uint32_t)rd(h + 44, 4);
    if (m.headerSize + uint64_t(tablesSize) > blob.size()) { err = "metadata tables truncated"; return false; }
    std::string tables = blob.substr(m.headerSize, tablesSize);
    if (sha(tables) != blob.substr(48, 32)) { err = "metadata tables checksum mismatch"; return false; }
    m.headerFlags = m.headerSize >= 132 ? (uint32_t)rd(h + 128, 4) : 0;
    const uint8_t* t = (const uint8_t*)tables.data();
    auto desc = [&](size_t off, size_t want, uint32_t& o, uint32_t& n, std::string& e) {
        o = (uint32_t)rd(h + off, 4);
        n = (uint32_t)rd(h + off + 4, 4);
        uint32_t es = (uint32_t)rd(h + off + 8, 4);
        if (es != want || uint64_t(o) + uint64_t(n) * es > tablesSize) { e = "bad table descriptor"; return false; }
        return true;
    };
    uint32_t po, pn, eo, en, go, gn, bo, bn;
    if (!desc(80, kPartEntry, po, pn, err) || !desc(92, kExtentEntry, eo, en, err) || !desc(104, kGroupEntry, go, gn, err) ||
        !desc(116, kDevEntry, bo, bn, err))
        return false;
    std::vector<Extent> ext(en);
    for (uint32_t i = 0; i < en; i++) {
        const uint8_t* e = t + eo + i * kExtentEntry;
        ext[i] = {rd(e, 8), (uint32_t)rd(e + 8, 4), rd(e + 12, 8), (uint32_t)rd(e + 20, 4)};
    }
    m.parts.clear();
    for (uint32_t i = 0; i < pn; i++) {
        const uint8_t* p = t + po + i * kPartEntry;
        Part part;
        part.name = name36(p);
        part.attrs = (uint32_t)rd(p + 36, 4);
        uint32_t first = (uint32_t)rd(p + 40, 4), num = (uint32_t)rd(p + 44, 4);
        part.group = (uint32_t)rd(p + 48, 4);
        if (uint64_t(first) + num > en || part.group >= gn) { err = "partition " + part.name + " has bad extents/group"; return false; }
        part.extents.assign(ext.begin() + first, ext.begin() + first + num);
        m.parts.push_back(part);
    }
    m.groups.clear();
    for (uint32_t i = 0; i < gn; i++) {
        const uint8_t* g = t + go + i * kGroupEntry;
        m.groups.push_back({name36(g), (uint32_t)rd(g + 36, 4), rd(g + 40, 8)});
    }
    m.devs.clear();
    for (uint32_t i = 0; i < bn; i++) {
        const uint8_t* b = t + bo + i * kDevEntry;
        BlockDev d;
        d.firstSector = rd(b, 8);
        d.alignment = (uint32_t)rd(b + 8, 4);
        d.alignmentOffset = (uint32_t)rd(b + 12, 4);
        d.size = rd(b + 16, 8);
        d.name = name36(b + 24);
        d.flags = (uint32_t)rd(b + 60, 4);
        m.devs.push_back(d);
    }
    if (m.devs.empty()) { err = "no block devices in metadata"; return false; }
    return true;
}

std::string serializeGeometry(const Metadata& m) {
    std::string g(kGeometryStructSize, '\0');
    wr(g, 0, kGeometryMagic, 4);
    wr(g, 4, kGeometryStructSize, 4);
    wr(g, 40, m.maxSize, 4);
    wr(g, 44, m.slotCount, 4);
    wr(g, 48, m.blockSize, 4);
    std::string c = sha(g);
    memcpy(&g[8], c.data(), 32);
    g.resize(kGeometrySize, '\0');
    return g;
}

uint64_t alignUp(uint64_t sector, uint64_t alignSectors, uint64_t offSectors) {
    if (alignSectors <= 1) return sector;
    uint64_t base = sector < offSectors ? 0 : sector - offSectors;
    uint64_t r = (base + alignSectors - 1) / alignSectors * alignSectors + offSectors;
    return std::max(r, sector);
}

}  // namespace

// ---- Metadata model ---------------------------------------------------------

uint64_t Part::sectors() const {
    uint64_t s = 0;
    for (auto& e : extents) s += e.sectors;
    return s;
}

Part* Metadata::find(const std::string& name) {
    for (auto& p : parts) if (p.name == name) return &p;
    return nullptr;
}
const Part* Metadata::find(const std::string& name) const {
    for (auto& p : parts) if (p.name == name) return &p;
    return nullptr;
}
int Metadata::groupIndex(const std::string& name) const {
    for (size_t i = 0; i < groups.size(); i++) if (groups[i].name == name) return (int)i;
    return -1;
}
uint64_t Metadata::groupUsed(uint32_t g) const {
    uint64_t s = 0;
    for (auto& p : parts) if (p.group == g) s += p.bytes();
    return s;
}
uint64_t Metadata::totalBytes() const {
    uint64_t s = 0;
    for (auto& d : devs) s += d.size - d.firstSector * 512;
    return s;
}
uint64_t Metadata::usedBytes() const {
    uint64_t s = 0;
    for (auto& p : parts) s += p.bytes();
    return s;
}

std::vector<Region> Metadata::freeRegions(uint32_t dev) const {
    std::vector<Region> used, out;
    if (dev >= devs.size()) return out;
    for (auto& p : parts)
        for (auto& e : p.extents)
            if (e.type == kTargetLinear && e.source == dev) used.push_back({e.target, e.sectors});
    std::sort(used.begin(), used.end(), [](const Region& a, const Region& b) { return a.start < b.start; });
    uint64_t cur = devs[dev].firstSector, end = devs[dev].size / 512;
    for (auto& u : used) {
        if (u.start > cur) out.push_back({cur, u.start - cur});
        cur = std::max(cur, u.start + u.sectors);
    }
    if (end > cur) out.push_back({cur, end - cur});
    return out;
}

uint64_t Metadata::freeBytes() const {
    uint64_t s = 0;
    uint64_t blk = blockSize / 512;
    for (uint32_t d = 0; d < devs.size(); d++) {
        uint64_t as = devs[d].alignment / 512, ao = devs[d].alignmentOffset / 512;
        for (auto& r : freeRegions(d)) {
            uint64_t a = alignUp(r.start, as, ao);
            if (a < r.start + r.sectors) s += (r.start + r.sectors - a) / blk * blk * 512;
        }
    }
    return s;
}

uint64_t Metadata::maxGrowBytes(const std::string& name) const {
    const Part* p = find(name);
    if (!p) return 0;
    uint64_t fb = freeBytes();
    // The region right after the last extent can be used without alignment loss.
    if (!p->extents.empty()) {
        const Extent& last = p->extents.back();
        for (auto& r : freeRegions(last.source))
            if (r.start == last.target + last.sectors) {
                uint64_t as = devs[last.source].alignment / 512, ao = devs[last.source].alignmentOffset / 512;
                uint64_t a = alignUp(r.start, as, ao);
                uint64_t aligned = a < r.start + r.sectors ? (r.start + r.sectors - a) : 0;
                fb += (r.sectors - aligned) / (blockSize / 512) * blockSize;
            }
    }
    const Group& g = groups[p->group];
    if (g.maxSize) {
        uint64_t room = g.maxSize > groupUsed(p->group) ? g.maxSize - groupUsed(p->group) : 0;
        fb = std::min(fb, room);
    }
    return fb;
}

bool Metadata::resize(const std::string& name, uint64_t bytes, std::string& err) {
    Part* p = find(name);
    if (!p) { err = "no such partition: " + name; return false; }
    bytes = (bytes + blockSize - 1) / blockSize * blockSize;
    uint64_t want = bytes / 512, have = p->sectors();
    const Group& g = groups[p->group];
    if (g.maxSize && groupUsed(p->group) - have * 512 + bytes > g.maxSize) {
        err = "group " + g.name + " would exceed its maximum size (" + util::humanSize(g.maxSize) + ")";
        return false;
    }
    if (want < have) {  // shrink from the end
        uint64_t cut = have - want;
        while (cut && !p->extents.empty()) {
            Extent& e = p->extents.back();
            if (e.sectors <= cut) { cut -= e.sectors; p->extents.pop_back(); }
            else { e.sectors -= cut; cut = 0; }
        }
        return true;
    }
    uint64_t need = want - have, blk = blockSize / 512;
    std::vector<Extent> added;
    // Extend in place first.
    if (!p->extents.empty() && p->extents.back().type == kTargetLinear) {
        Extent& last = p->extents.back();
        for (auto& r : freeRegions(last.source))
            if (r.start == last.target + last.sectors) {
                uint64_t take = std::min(need, r.sectors / blk * blk);
                last.sectors += take;
                need -= take;
                break;
            }
    }
    for (uint32_t d = 0; d < devs.size() && need; d++) {
        uint64_t as = devs[d].alignment / 512, ao = devs[d].alignmentOffset / 512;
        for (auto& r : freeRegions(d)) {
            if (!need) break;
            uint64_t a = alignUp(r.start, as, ao), end = r.start + r.sectors;
            if (a >= end) continue;
            uint64_t take = std::min(need, (end - a) / blk * blk);
            if (!take) continue;
            p->extents.push_back({take, kTargetLinear, a, d});
            need -= take;
        }
    }
    if (need) {
        err = "not enough free space in super (" + util::humanSize(need * 512) + " short)";
        return false;  // caller discards the modified copy
    }
    return true;
}

bool Metadata::remove(const std::string& name, std::string& err) {
    auto it = std::find_if(parts.begin(), parts.end(), [&](const Part& p) { return p.name == name; });
    if (it == parts.end()) { err = "no such partition: " + name; return false; }
    parts.erase(it);
    return true;
}

bool Metadata::add(const std::string& name, const std::string& group, uint32_t attrs, std::string& err) {
    if (name.empty() || name.size() > 35) { err = "invalid name"; return false; }
    if (find(name)) { err = name + " already exists"; return false; }
    int g = groupIndex(group);
    if (g < 0) { err = "no such group: " + group; return false; }
    parts.push_back({name, attrs, (uint32_t)g, {}});
    return true;
}

bool Metadata::validate(std::string& err) const {
    if (devs.empty()) { err = "no block device"; return false; }
    std::vector<std::pair<uint64_t, uint64_t>> spans;
    for (auto& p : parts) {
        if (p.group >= groups.size()) { err = p.name + ": bad group"; return false; }
        for (auto& e : p.extents) {
            if (e.type != kTargetLinear) continue;
            if (e.source >= devs.size() || e.target < devs[e.source].firstSector ||
                (e.target + e.sectors) * 512 > devs[e.source].size) {
                err = p.name + ": extent outside the super device";
                return false;
            }
            spans.push_back({(uint64_t(e.source) << 56) | e.target, e.sectors});
        }
    }
    std::sort(spans.begin(), spans.end());
    for (size_t i = 1; i < spans.size(); i++)
        if (spans[i - 1].first + spans[i - 1].second > spans[i].first && (spans[i - 1].first >> 56) == (spans[i].first >> 56)) {
            err = "overlapping extents";
            return false;
        }
    for (uint32_t g = 0; g < groups.size(); g++)
        if (groups[g].maxSize && groupUsed(g) > groups[g].maxSize) { err = "group " + groups[g].name + " over its maximum"; return false; }
    if (serialize().size() > maxSize) { err = "metadata too large for its slot"; return false; }
    return true;
}

std::string Metadata::serialize() const {
    size_t nExt = 0;
    for (auto& p : parts) nExt += p.extents.size();
    size_t po = 0, eo = po + parts.size() * kPartEntry, go = eo + nExt * kExtentEntry, bo = go + groups.size() * kGroupEntry;
    std::string t(bo + devs.size() * kDevEntry, '\0');
    size_t ei = 0;
    for (size_t i = 0; i < parts.size(); i++) {
        size_t o = po + i * kPartEntry;
        putName(t, o, parts[i].name);
        wr(t, o + 36, parts[i].attrs, 4);
        wr(t, o + 40, ei, 4);
        wr(t, o + 44, parts[i].extents.size(), 4);
        wr(t, o + 48, parts[i].group, 4);
        for (auto& e : parts[i].extents) {
            size_t x = eo + ei * kExtentEntry;
            wr(t, x, e.sectors, 8);
            wr(t, x + 8, e.type, 4);
            wr(t, x + 12, e.target, 8);
            wr(t, x + 20, e.source, 4);
            ei++;
        }
    }
    for (size_t i = 0; i < groups.size(); i++) {
        size_t o = go + i * kGroupEntry;
        putName(t, o, groups[i].name);
        wr(t, o + 36, groups[i].flags, 4);
        wr(t, o + 40, groups[i].maxSize, 8);
    }
    for (size_t i = 0; i < devs.size(); i++) {
        size_t o = bo + i * kDevEntry;
        wr(t, o, devs[i].firstSector, 8);
        wr(t, o + 8, devs[i].alignment, 4);
        wr(t, o + 12, devs[i].alignmentOffset, 4);
        wr(t, o + 16, devs[i].size, 8);
        putName(t, o + 24, devs[i].name);
        wr(t, o + 60, devs[i].flags, 4);
    }
    std::string h(headerSize, '\0');
    wr(h, 0, kHeaderMagic, 4);
    wr(h, 4, major, 2);
    wr(h, 6, minor, 2);
    wr(h, 8, headerSize, 4);
    wr(h, 44, t.size(), 4);
    std::string tc = sha(t);
    memcpy(&h[48], tc.data(), 32);
    auto d = [&](size_t off, size_t o, size_t n, size_t es) { wr(h, off, o, 4); wr(h, off + 4, n, 4); wr(h, off + 8, es, 4); };
    d(80, po, parts.size(), kPartEntry);
    d(92, eo, nExt, kExtentEntry);
    d(104, go, groups.size(), kGroupEntry);
    d(116, bo, devs.size(), kDevEntry);
    if (headerSize >= 132) wr(h, 128, headerFlags, 4);
    std::string hc = sha(h);
    memcpy(&h[12], hc.data(), 32);
    return h + t;
}

// ---- I/O ----------------------------------------------------------------------

std::string superDevice() {
    std::string by = Partitions::byNameDir();
    std::string slot = device::info().slot;
    for (auto n : {std::string("super"), "super" + slot, "system" + slot})
        if (util::exists(by + "/" + n)) {
            // Retrofit devices use system_a as super; only accept it if it has LP geometry.
            if (n != "super") {
                Metadata m;
                std::string e;
                if (!read(by + "/" + n, 0, m, e)) continue;
            }
            return by + "/" + n;
        }
    return by + "/super";
}

uint32_t currentSlotIndex() {
    const std::string& s = device::info().slot;
    return s == "_b" ? 1 : 0;
}

bool read(const std::string& dev, uint32_t slot, Metadata& m, std::string& err) {
    int fd = ::open(dev.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) { err = dev + ": " + strerror(errno); return false; }
    std::vector<uint8_t> g(kGeometrySize);
    bool ok = false;
    std::string gerr;
    for (uint64_t off : {kReserved, kReserved + kGeometrySize})
        if (preadAll(fd, g.data(), g.size(), off) && parseGeometry(g.data(), m, gerr)) { ok = true; break; }
    if (!ok) { close(fd); err = gerr; return false; }
    slot = std::min(slot, m.slotCount - 1);
    std::string merr;
    ok = false;
    for (uint64_t off : {primaryOffset(m, slot), backupOffset(m, slot)}) {
        std::string blob(m.maxSize, '\0');
        if (preadAll(fd, &blob[0], blob.size(), off) && parseMetadata(blob, m, merr)) { ok = true; break; }
    }
    close(fd);
    if (!ok) err = merr;
    return ok;
}

bool write(const std::string& dev, const Metadata& m, std::string& err) {
    if (!m.validate(err)) return false;
    std::string blob = m.serialize();
    int fd = ::open(dev.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) { err = dev + ": " + strerror(errno); return false; }
    // Same order as liblp: backup copy first, so a crash mid-way leaves a valid primary.
    for (uint32_t s = 0; s < m.slotCount; s++) {
        if (!pwriteAll(fd, blob.data(), blob.size(), backupOffset(m, s)) || fsync(fd) != 0 ||
            !pwriteAll(fd, blob.data(), blob.size(), primaryOffset(m, s)) || fsync(fd) != 0) {
            err = std::string("write failed: ") + strerror(errno);
            close(fd);
            return false;
        }
    }
    ioctl(fd, BLKFLSBUF, 0);
    close(fd);
    for (uint32_t s = 0; s < m.slotCount; s++) {
        Metadata back;
        if (!read(dev, s, back, err) || back.serialize() != blob) {
            err = "read-back verification failed for slot " + std::to_string(s) + (err.empty() ? "" : ": " + err);
            return false;
        }
    }
    return true;
}

bool format(const std::string& dev, const Metadata& m, std::string& err) {
    int fd = ::open(dev.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) { err = strerror(errno); return false; }
    std::string zero(kReserved, '\0'), geo = serializeGeometry(m);
    bool ok = pwriteAll(fd, zero.data(), zero.size(), 0) && pwriteAll(fd, geo.data(), geo.size(), kReserved) &&
              pwriteAll(fd, geo.data(), geo.size(), kReserved + kGeometrySize);
    close(fd);
    if (!ok) { err = "write failed"; return false; }
    return write(dev, m, err);
}

bool parseImage(const std::string& data, Metadata& m, std::string& err) {
    std::string e1;
    if (data.size() >= kGeometrySize && parseGeometry((const uint8_t*)data.data(), m, e1))
        return parseMetadata(data.substr(kGeometrySize), m, err);
    if (data.size() >= kReserved + kGeometrySize && parseGeometry((const uint8_t*)data.data() + kReserved, m, err)) {
        uint64_t off = primaryOffset(m, 0);
        if (data.size() < off + 128) { err = "metadata truncated"; return false; }
        return parseMetadata(data.substr(off), m, err);
    }
    err = e1;
    return false;
}

std::string serializeImage(const Metadata& m) { return serializeGeometry(m) + m.serialize(); }

bool saveMetadataRegion(const std::string& dev, const std::string& file, std::string& err) {
    Metadata m;
    if (!read(dev, 0, m, err)) return false;
    uint64_t len = backupOffset(m, m.slotCount);
    int fd = ::open(dev.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) { err = strerror(errno); return false; }
    std::string data(len, '\0');
    bool ok = preadAll(fd, &data[0], len, 0);
    close(fd);
    if (!ok) { err = "read failed"; return false; }
    util::mkdirs(util::dirname(file));
    if (!util::writeFile(file, data)) { err = "cannot write " + file; return false; }
    return true;
}

bool restoreMetadataRegion(const std::string& dev, const std::string& file, std::string& err) {
    std::string data;
    if (!util::readFile(file, data, 64 << 20) || data.size() < kReserved + 2 * kGeometrySize) { err = "unreadable backup"; return false; }
    Metadata m;
    if (!parseGeometry((const uint8_t*)data.data() + kReserved, m, err)) return false;
    if (data.size() != backupOffset(m, m.slotCount)) { err = "backup size does not match its geometry"; return false; }
    std::string blob = data.substr(primaryOffset(m, 0), m.maxSize);
    if (!parseMetadata(blob, m, err)) return false;
    if (m.devs[0].size != Partitions::blockSize(dev) && !device::isSim()) { err = "backup is from a super of a different size"; return false; }
    int fd = ::open(dev.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) { err = strerror(errno); return false; }
    // Never touch sector 0..4095 (reserved, may hold a bootloader structure on some devices).
    bool ok = pwriteAll(fd, data.data() + kReserved, data.size() - kReserved, kReserved) && fsync(fd) == 0;
    close(fd);
    if (!ok) err = "write failed";
    return ok;
}

bool busy(std::string& why) {
    std::string st;
    if (util::readFile(device::path("/metadata/ota/state"), st, 4096) && st.size() >= 2 && (uint8_t)st[0] == 0x08) {
        int state = (uint8_t)st[1];
        static const char* names[] = {"None", "Initiated", "Unverified", "Merging", "MergeNeedsReboot", "MergeCompleted", "MergeFailed", "Cancelled"};
        if (state == 1 || state == 2 || state == 3 || state == 4 || state == 6) {
            why = std::string("A Virtual A/B OTA is in progress (state: ") + (state < 8 ? names[state] : "?") +
                  "). Boot Android and let the update finish merging first.";
            return true;
        }
    }
    for (auto& e : util::listDir(device::path("/dev/block/mapper")))
        if (util::endsWith(e.name, "-cow") || util::endsWith(e.name, "-base") || util::endsWith(e.name, "-cow-img")) {
            why = "Snapshot devices are mapped (" + e.name + "): a Virtual A/B update is still merging.";
            return true;
        }
    return false;
}

// ---- device-mapper ------------------------------------------------------------

std::string mappedPath(const std::string& name) {
    std::string p = device::path("/dev/block/mapper/" + name);
    return util::exists(p) ? p : "";
}

namespace {

struct DmIoctl {
    alignas(8) char buf[16384];
    dm_ioctl* io() { return (dm_ioctl*)buf; }
    void init(const std::string& name, uint32_t flags = 0) {
        memset(buf, 0, sizeof(buf));
        io()->version[0] = 4;
        io()->data_size = sizeof(buf);
        io()->data_start = sizeof(dm_ioctl);
        io()->flags = flags;
        strncpy(io()->name, name.c_str(), DM_NAME_LEN - 1);
    }
};

int dmControl() {
    for (auto p : {"/dev/device-mapper", "/dev/mapper/control"}) {
        int fd = ::open(p, O_RDWR | O_CLOEXEC);
        if (fd >= 0) return fd;
    }
    return -1;
}

bool superMajMin(std::string& out) {
    struct stat st;
    if (stat(superDevice().c_str(), &st) != 0 || !S_ISBLK(st.st_mode)) return false;
    out = std::to_string(major(st.st_rdev)) + ":" + std::to_string(minor(st.st_rdev));
    return true;
}

bool loadTable(int ctl, const Metadata& m, const Part& p, bool rw, std::string& err) {
    std::string dev;
    if (!superMajMin(dev)) { err = "super is not a block device"; return false; }
    DmIoctl d;
    d.init(p.name, rw ? 0 : DM_READONLY_FLAG);
    size_t off = sizeof(dm_ioctl);
    uint64_t logical = 0;
    for (auto& e : p.extents) {
        auto* spec = (dm_target_spec*)(d.buf + off);
        spec->sector_start = logical;
        spec->length = e.sectors;
        strcpy(spec->target_type, e.type == kTargetZero ? "zero" : "linear");
        std::string params = e.type == kTargetZero ? "" : dev + " " + std::to_string(e.target);
        char* pp = d.buf + off + sizeof(dm_target_spec);
        if (off + sizeof(dm_target_spec) + params.size() + 16 > sizeof(d.buf)) { err = "too many extents"; return false; }
        memcpy(pp, params.c_str(), params.size() + 1);
        size_t next = (sizeof(dm_target_spec) + params.size() + 1 + 7) & ~size_t(7);
        spec->next = (uint32_t)next;
        off += next;
        logical += e.sectors;
    }
    d.io()->target_count = (uint32_t)p.extents.size();
    if (ioctl(ctl, DM_TABLE_LOAD, d.buf) != 0) { err = std::string("DM_TABLE_LOAD: ") + strerror(errno); return false; }
    d.init(p.name);
    if (ioctl(ctl, DM_DEV_SUSPEND, d.buf) != 0) { err = std::string("DM resume: ") + strerror(errno); return false; }
    return true;
}

}  // namespace

bool map(const Metadata& m, const std::string& name, bool rw, std::string& err) {
    const Part* p = m.find(name);
    if (!p) { err = "no such partition"; return false; }
    if (device::isSim()) {
        std::string f = device::path("/dev/block/mapper/" + name);
        util::mkdirs(util::dirname(f));
        int fd = ::open(f.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
        if (fd < 0 || ftruncate(fd, (off_t)p->bytes()) != 0) { if (fd >= 0) close(fd); err = "sim: cannot size mapper file"; return false; }
        close(fd);
        return true;
    }
    if (p->extents.empty()) {  // zero-size: nothing to map
        std::string e2;
        unmap(name, e2);
        return true;
    }
    int ctl = dmControl();
    if (ctl < 0) { err = "no /dev/device-mapper"; return false; }
    DmIoctl d;
    d.init(name);
    bool exists = ioctl(ctl, DM_DEV_STATUS, d.buf) == 0;
    if (!exists) {
        d.init(name);
        if (ioctl(ctl, DM_DEV_CREATE, d.buf) != 0) { err = std::string("DM_DEV_CREATE: ") + strerror(errno); close(ctl); return false; }
    }
    dev_t devnum = d.io()->dev;
    bool ok = loadTable(ctl, m, *p, rw, err);
    close(ctl);
    if (!ok) return false;
    // ueventd normally creates /dev/block/dm-N and the mapper/<name> link; make sure they exist.
    std::string node = "/dev/block/dm-" + std::to_string(minor(devnum));
    for (int i = 0; i < 20 && !util::exists(node); i++) usleep(100 * 1000);
    if (!util::exists(node)) mknod(node.c_str(), S_IFBLK | 0600, devnum);
    std::string link = "/dev/block/mapper/" + name;
    if (!util::exists(link)) {
        util::mkdirs("/dev/block/mapper");
        if (symlink(node.c_str(), link.c_str()) != 0) LOGW("symlink %s failed", link.c_str());
    }
    return true;
}

bool unmap(const std::string& name, std::string& err) {
    if (device::isSim()) {
        unlink(device::path("/dev/block/mapper/" + name).c_str());
        return true;
    }
    int ctl = dmControl();
    if (ctl < 0) { err = "no /dev/device-mapper"; return false; }
    DmIoctl d;
    d.init(name);
    bool ok = ioctl(ctl, DM_DEV_REMOVE, d.buf) == 0 || errno == ENXIO;
    if (!ok) err = std::string("DM_DEV_REMOVE: ") + strerror(errno) + " (is it still mounted?)";
    close(ctl);
    if (ok) unlink(("/dev/block/mapper/" + name).c_str());
    return ok;
}

bool ensureWritable(const std::string& blockDev, std::string& err) {
    if (device::isSim()) return true;
    int fd = ::open(blockDev.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return true;  // let the caller report the open error
    int ro = 0;
    ioctl(fd, BLKROGET, &ro);
    if (ro) {
        int zero = 0;
        ioctl(fd, BLKROSET, &zero);
        ioctl(fd, BLKROGET, &ro);
    }
    close(fd);
    if (!ro) return true;
    // dm device created read-only: reload its table read-write.
    char real[PATH_MAX];
    if (!realpath(blockDev.c_str(), real)) { err = "cannot resolve " + blockDev; return false; }
    std::string dmName = util::readLine(std::string("/sys/block/") + util::basename(real) + "/dm/name");
    if (dmName.empty()) { err = "device is read-only (BLKROSET refused)"; return false; }
    Metadata m;
    if (!read(superDevice(), currentSlotIndex(), m, err)) return false;
    if (!m.find(dmName)) { err = dmName + " is not in the super metadata"; return false; }
    LOGI("remapping %s read-write", dmName.c_str());
    return map(m, dmName, true, err);
}

// ---- high level ---------------------------------------------------------------

std::string backupDir() { return Partitions::get().appDir() + "/super"; }

namespace {

bool loadForEdit(Metadata& m, Task& t) {
    std::string why;
    if (busy(why)) { t.print("Refusing: %s", why.c_str()); return false; }
    std::string err, dev = superDevice();
    if (!read(dev, currentSlotIndex(), m, err)) { t.print("Cannot read super metadata from %s: %s", dev.c_str(), err.c_str()); return false; }
    if (m.devs.size() != 1) { t.print("Refusing: super spans %zu block devices (retrofit layout); use fastbootd.", m.devs.size()); return false; }
    static bool backedUp = false;
    if (!backedUp) {
        std::string f = backupDir() + "/super_metadata_" + util::dateStamp() + ".bin";
        if (saveMetadataRegion(dev, f, err)) {
            t.print("Metadata backup: %s", f.c_str());
            backedUp = true;
        } else {
            t.print("Cannot back up the metadata (%s) - not continuing.", err.c_str());
            return false;
        }
    }
    return true;
}

void unmountIfMapped(const std::string& name, Task& t) {
    std::string dev = mappedPath(name);
    for (auto& p : Partitions::get().all())
        if (!dev.empty() && (p.blockDev == dev || util::basename(p.blockDev) == name) && p.mounted) {
            t.print("Unmounting %s", p.mountPoint.c_str());
            Partitions::get().unmount(p);
        }
}

bool commit(const Metadata& m, Task& t) {
    std::string err;
    t.print("Writing metadata to %u slot(s)...", m.slotCount);
    if (!write(superDevice(), m, err)) { t.print("FAILED: %s", err.c_str()); return false; }
    t.print("Metadata written and verified.");
    return true;
}

}  // namespace

bool resizePartition(const std::string& name, uint64_t bytes, Task& t) {
    Metadata m;
    if (!loadForEdit(m, t)) return false;
    const Part* p = m.find(name);
    if (!p) { t.print("No logical partition named %s", name.c_str()); return false; }
    t.print("Resize %s: %s -> %s (free in super: %s)", name.c_str(), util::humanSize(p->bytes()).c_str(),
            util::humanSize(bytes).c_str(), util::humanSize(m.freeBytes()).c_str());
    std::string err;
    Metadata nm = m;
    if (!nm.resize(name, bytes, err)) { t.print("Cannot resize: %s", err.c_str()); return false; }
    unmountIfMapped(name, t);
    if (!commit(nm, t)) return false;
    if (!map(nm, name, true, err)) {
        t.print("Mapping %s failed: %s", name.c_str(), err.c_str());
        t.print("The new size is saved; reboot to recovery to apply it.");
        return false;
    }
    t.print("%s is now %s", name.c_str(), util::humanSize(nm.find(name)->bytes()).c_str());
    return true;
}

bool deletePartition(const std::string& name, Task& t) {
    Metadata m;
    if (!loadForEdit(m, t)) return false;
    std::string err;
    unmountIfMapped(name, t);
    if (!mappedPath(name).empty() && !unmap(name, err)) { t.print("Cannot unmap %s: %s", name.c_str(), err.c_str()); return false; }
    if (!m.remove(name, err)) { t.print("%s", err.c_str()); return false; }
    if (!commit(m, t)) return false;
    t.print("Deleted %s. Free in super: %s", name.c_str(), util::humanSize(m.freeBytes()).c_str());
    return true;
}

bool createPartition(const std::string& name, const std::string& group, uint64_t bytes, Task& t) {
    Metadata m;
    if (!loadForEdit(m, t)) return false;
    std::string err;
    if (!m.add(name, group, 0, err) || (bytes && !m.resize(name, bytes, err))) { t.print("Cannot create: %s", err.c_str()); return false; }
    if (!commit(m, t)) return false;
    if (!map(m, name, true, err)) { t.print("Mapping failed: %s", err.c_str()); return false; }
    t.print("Created %s (%s)", name.c_str(), util::humanSize(bytes).c_str());
    return true;
}

namespace {

std::string otherSuffix(const std::string& s) { return s == "_a" ? "_b" : "_a"; }

bool inGroups(const std::vector<SlotGroup>& groups, const std::string& name, const std::string& suffix) {
    for (auto& g : groups)
        for (auto& p : g.parts)
            if (p.first + suffix == name) return true;
    return false;
}

// The metadata after laying out the slot, or false (+ err) if it doesn't fit.
bool layoutSlot(const Metadata& m, const std::vector<SlotGroup>& groups, const std::string& suffix, bool freeSource, Metadata& out,
                std::string& err) {
    out = m;
    if (freeSource)
        for (auto& p : out.parts)
            if (util::endsWith(p.name, otherSuffix(suffix))) p.extents.clear();
    std::vector<int> gi;
    for (auto& g : groups) {
        std::string gname = g.name + suffix;
        int i = out.groupIndex(gname);
        if (i < 0) {
            out.groups.push_back({gname, 0, g.maxSize});
            i = (int)out.groups.size() - 1;
        } else {
            out.groups[i].maxSize = g.maxSize;
        }
        gi.push_back(i);
    }
    out.parts.erase(std::remove_if(out.parts.begin(), out.parts.end(),
                                   [&](const Part& p) {
                                       return std::find(gi.begin(), gi.end(), (int)p.group) != gi.end() || inGroups(groups, p.name, suffix);
                                   }),
                    out.parts.end());
    for (auto& g : groups)
        for (auto& p : g.parts) {
            if (!out.add(p.first + suffix, g.name + suffix, kAttrReadonly, err)) return false;
            if (p.second && !out.resize(p.first + suffix, p.second, err)) return false;
        }
    return out.validate(err);
}

}  // namespace

SlotFit fitSlot(const std::vector<SlotGroup>& groups, const std::string& suffix) {
    SlotFit f;
    Metadata m, out;
    std::string err;
    if (!read(superDevice(), currentSlotIndex(), m, err)) { f.error = "Cannot read super: " + err; return f; }
    for (auto& g : groups)
        for (auto& p : g.parts) f.needed += (p.second + m.blockSize - 1) / m.blockSize * m.blockSize;
    f.available = m.freeBytes();
    for (auto& p : m.parts)
        if (util::endsWith(p.name, suffix)) {
            int g = (int)p.group;
            bool ours = inGroups(groups, p.name, suffix);
            for (auto& sg : groups) ours |= m.groups[g].name == sg.name + suffix;
            if (ours) f.available += p.bytes();
        }
    if (m.devs.size() != 1) { f.error = "super spans several block devices (retrofit layout)"; return f; }
    if (layoutSlot(m, groups, suffix, false, out, err)) { f.fits = true; return f; }
    std::string err2;
    if (layoutSlot(m, groups, suffix, true, out, err2)) {
        f.fits = true;
        f.needsFreeSource = true;
        return f;
    }
    f.error = err;
    return f;
}

bool prepareSlot(const std::vector<SlotGroup>& groups, const std::string& suffix, bool freeSource, Task& t) {
    Metadata m, nm;
    if (!loadForEdit(m, t)) return false;
    std::string err;
    bool freed = false;
    if (!layoutSlot(m, groups, suffix, false, nm, err)) {
        if (!freeSource || !layoutSlot(m, groups, suffix, true, nm, err)) { t.print("The new partitions don't fit in super: %s", err.c_str()); return false; }
        freed = true;
        t.print("super only has room for one slot: emptying the %s partitions first.", otherSuffix(suffix).c_str());
    }
    // Everything that changes must be unmounted and unmapped first.
    for (auto& p : m.parts) {
        const Part* np = nm.find(p.name);
        bool changed = !np || np->extents.size() != p.extents.size() || np->bytes() != p.bytes() ||
                       util::endsWith(p.name, suffix) || (freed && util::endsWith(p.name, otherSuffix(suffix)));
        if (!changed) continue;
        unmountIfMapped(p.name, t);
        if (!mappedPath(p.name).empty() && !unmap(p.name, err)) { t.print("Cannot unmap %s: %s", p.name.c_str(), err.c_str()); return false; }
    }
    for (auto& g : groups)
        for (auto& p : g.parts) t.print("  %s%s: %s", p.first.c_str(), suffix.c_str(), util::humanSize(p.second).c_str());
    if (!commit(nm, t)) return false;
    for (auto& g : groups)
        for (auto& p : g.parts)
            if (!map(nm, p.first + suffix, true, err)) { t.print("Mapping %s%s failed: %s", p.first.c_str(), suffix.c_str(), err.c_str()); return false; }
    t.print("Free in super afterwards: %s", util::humanSize(nm.freeBytes()).c_str());
    return true;
}

bool unmapAll(Task& t) {
    Metadata m;
    std::string err;
    if (!read(superDevice(), currentSlotIndex(), m, err)) return true;  // nothing mapped from an unreadable super
    bool ok = true;
    for (auto& p : m.parts) {
        if (mappedPath(p.name).empty()) continue;
        unmountIfMapped(p.name, t);
        if (!unmap(p.name, err)) { t.print("Cannot unmap %s: %s", p.name.c_str(), err.c_str()); ok = false; }
    }
    return ok;
}

void simSyncFromSuper() {
    if (!device::isSim()) return;
    Metadata m;
    std::string err, dev = superDevice();
    if (!read(dev, currentSlotIndex(), m, err)) return;
    int in = ::open(dev.c_str(), O_RDONLY | O_CLOEXEC);
    if (in < 0) return;
    std::vector<char> buf(1 << 20);
    for (auto& p : m.parts) {
        std::string f = device::path("/dev/block/mapper/" + p.name);
        unlink(f.c_str());
        if (!p.bytes()) continue;
        int out = ::open(f.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (out < 0) continue;
        if (ftruncate(out, (off_t)p.bytes()) != 0) {}
        uint64_t logical = 0;
        for (auto& e : p.extents) {
            uint64_t start = e.target * 512, end = start + e.sectors * 512;
            for (off_t pos = (off_t)start; e.type == kTargetLinear && pos < (off_t)end;) {
                off_t data = lseek(in, pos, SEEK_DATA);  // super is a sparse file: copy only real data
                if (data < 0 || data >= (off_t)end) break;
                off_t hole = lseek(in, data, SEEK_HOLE);
                if (hole < 0 || hole > (off_t)end) hole = (off_t)end;
                for (off_t o = data; o < hole;) {
                    size_t k = (size_t)std::min<off_t>((off_t)buf.size(), hole - o);
                    if (!preadAll(in, buf.data(), k, (uint64_t)o) || !pwriteAll(out, buf.data(), k, logical + (uint64_t)(o - (off_t)start))) break;
                    o += (off_t)k;
                }
                pos = hole;
            }
            logical += e.sectors * 512;
        }
        ::close(out);
    }
    ::close(in);
}

bool freeOtherSlot(Task& t) {
    Metadata m;
    if (!loadForEdit(m, t)) return false;
    std::string other = currentSlotIndex() == 0 ? "_b" : "_a";
    uint64_t freed = 0;
    std::string err;
    for (auto& p : m.parts)
        if (util::endsWith(p.name, other) && p.bytes()) {
            t.print("  %s: %s -> 0", p.name.c_str(), util::humanSize(p.bytes()).c_str());
            freed += p.bytes();
            unmountIfMapped(p.name, t);
            if (!mappedPath(p.name).empty() && !unmap(p.name, err)) { t.print("Cannot unmap %s: %s", p.name.c_str(), err.c_str()); return false; }
            p.extents.clear();
        }
    if (!freed) { t.print("The other slot's partitions are already empty."); return true; }
    if (!commit(m, t)) return false;
    t.print("Freed %s. The other slot is no longer bootable until you flash it again.", util::humanSize(freed).c_str());
    return true;
}

}  // namespace tw::lp
