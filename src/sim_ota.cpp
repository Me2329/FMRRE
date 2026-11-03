// Builds A/B OTA packages (payload.bin) for the simulator's demo ROM and for the self-tests.
// Only what a full OTA uses: REPLACE / ZERO operations and verity regions left to the device.
#include "sim.h"

#include <algorithm>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

#include "core/sha256.h"
#include "core/util.h"
#include "sys/payload.h"
#include "sys/verity.h"
#include "miniz.h"

namespace tw::sim {

namespace {

struct PbW {
    std::string s;
    void varint(uint64_t v) {
        while (v >= 0x80) { s.push_back(char(v | 0x80)); v >>= 7; }
        s.push_back(char(v));
    }
    void key(uint32_t f, uint32_t w) { varint((uint64_t(f) << 3) | w); }
    void u64(uint32_t f, uint64_t v) { key(f, 0); varint(v); }
    void bytes(uint32_t f, const std::string& b) { key(f, 2); varint(b.size()); s += b; }
};

std::string extent(uint64_t start, uint64_t count) {
    PbW e;
    e.u64(1, start);
    e.u64(2, count);
    return e.s;
}

void be(std::string& s, uint64_t v, int n) {
    for (int i = n - 1; i >= 0; i--) s.push_back(char(v >> (8 * i)));
}

bool allZero(const char* p, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (p[i]) return false;
    return true;
}

}  // namespace

bool otaPartFromImage(const std::string& name, std::string data, bool verity, const std::string& tmpFile, OtaPart& out,
                      std::string& err) {
    const uint32_t bs = 4096;
    data.resize((data.size() + bs - 1) / bs * bs, '\0');
    out = OtaPart();
    out.name = name;
    out.dataBlocks = data.size() / bs;
    std::string image = data;
    if (verity) {
        uint64_t tree = verity::hashTreeSize(data.size(), bs);
        uint64_t fec = verity::fecSize(data.size() + tree, 2, bs);
        out.treeCount = tree / bs;
        out.fecCount = fec / bs;
        out.salt = Sha256::hexOf(name).substr(0, 32);  // any bytes do; deterministic for the demo
        image.resize(data.size() + tree + fec + bs, '\0');  // + AVB footer block (zero here)
        int fd = ::open(tmpFile.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd < 0) { err = "cannot create " + tmpFile; return false; }
        std::string root;
        bool ok = write(fd, image.data(), image.size()) == (ssize_t)image.size() &&
                  verity::buildHashTree(fd, 0, data.size(), data.size(), tree, bs, "sha256", out.salt, root, err) &&
                  verity::encodeFec(fd, 0, data.size() + tree, data.size() + tree, fec, 2, bs, err) &&
                  pread(fd, &image[0], image.size(), 0) == (ssize_t)image.size();
        close(fd);
        unlink(tmpFile.c_str());
        if (!ok) { if (err.empty()) err = "I/O error"; return false; }
    }
    out.size = image.size();
    Sha256 h;
    h.update(image.data(), image.size());
    out.hash = h.digest();
    // Operations: 1 MiB chunks of the data, then the blocks after the verity regions.
    auto addOps = [&](uint64_t first, uint64_t last) {
        for (uint64_t b = first; b < last;) {
            uint64_t n = std::min<uint64_t>(256, last - b);
            const char* p = image.data() + b * bs;
            OtaOp op;
            op.start = b;
            op.count = n;
            if (allZero(p, n * bs)) op.type = payload::kZero;
            else { op.type = payload::kReplace; op.blob.assign(p, n * bs); }
            out.ops.push_back(std::move(op));
            b += n;
        }
    };
    addOps(0, out.dataBlocks);
    addOps(out.dataBlocks + out.treeCount + out.fecCount, out.size / bs);
    return true;
}

std::string encodePayload(const std::vector<OtaPart>& parts, const std::string& group, uint64_t groupSize,
                          const std::vector<std::string>& dynamicNames, const std::string& securityPatch) {
    PbW m;
    std::string blobs;
    m.u64(3, 4096);
    m.u64(12, 0);  // full payload
    for (auto& p : parts) {
        PbW pu;
        pu.bytes(1, p.name);
        PbW info;
        info.u64(1, p.size);
        info.bytes(2, p.hash);
        pu.bytes(7, info.s);
        for (auto& op : p.ops) {
            PbW o;
            o.u64(1, op.type);
            if (!op.blob.empty()) {
                o.u64(2, blobs.size());
                o.u64(3, op.blob.size());
            }
            o.bytes(6, extent(op.start, op.count));
            if (!op.blob.empty()) {
                Sha256 h;
                h.update(op.blob.data(), op.blob.size());
                o.bytes(8, h.digest());
                blobs += op.blob;
            }
            pu.bytes(8, o.s);
        }
        if (p.treeCount) {
            pu.bytes(10, extent(0, p.dataBlocks));
            pu.bytes(11, extent(p.dataBlocks, p.treeCount));
            pu.bytes(12, "sha256");
            pu.bytes(13, p.salt);
            pu.bytes(14, extent(0, p.dataBlocks + p.treeCount));
            pu.bytes(15, extent(p.dataBlocks + p.treeCount, p.fecCount));
            pu.u64(16, 2);
        }
        m.bytes(13, pu.s);
    }
    m.u64(14, 1793500000);
    if (!dynamicNames.empty()) {
        PbW g;
        g.bytes(1, group);
        g.u64(2, groupSize);
        for (auto& n : dynamicNames) g.bytes(3, n);
        PbW d;
        d.bytes(1, g.s);
        d.u64(2, 1);  // snapshot_enabled (Virtual A/B)
        m.bytes(15, d.s);
    }
    if (!securityPatch.empty()) m.bytes(18, securityPatch);
    std::string out = "CrAU";
    be(out, 2, 8);
    be(out, m.s.size(), 8);
    be(out, 0, 4);  // no metadata signature
    return out + m.s + blobs;
}

bool writeOtaZip(const std::string& path, const std::string& payload, const std::string& metadata) {
    mz_zip_archive z{};
    if (!mz_zip_writer_init_file(&z, path.c_str(), 0)) return false;
    std::string props = "FILE_SIZE=" + std::to_string(payload.size()) + "\nMETADATA_SIZE=" + std::to_string(payload.size() ? 24 : 0) + "\n";
    bool ok = mz_zip_writer_add_mem(&z, "META-INF/com/android/metadata", metadata.data(), metadata.size(), MZ_BEST_COMPRESSION) &&
              mz_zip_writer_add_mem(&z, "payload_properties.txt", props.data(), props.size(), MZ_BEST_COMPRESSION) &&
              mz_zip_writer_add_mem(&z, "payload.bin", payload.data(), payload.size(), MZ_NO_COMPRESSION) &&
              mz_zip_writer_finalize_archive(&z);
    mz_zip_writer_end(&z);
    return ok;
}

}  // namespace tw::sim
