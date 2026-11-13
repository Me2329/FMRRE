#include "sys/verity.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <thread>
#include <unistd.h>
#include <vector>

#include "core/sha256.h"

namespace tw::verity {

namespace {

bool preadAll(int fd, void* buf, size_t n, uint64_t off) {
    char* p = (char*)buf;
    while (n) {
        ssize_t r = pread(fd, p, n, (off_t)off);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return false;
        p += r; n -= (size_t)r; off += (uint64_t)r;
    }
    return true;
}

bool pwriteAll(int fd, const void* buf, size_t n, uint64_t off) {
    const char* p = (const char*)buf;
    while (n) {
        ssize_t r = pwrite(fd, p, n, (off_t)off);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return false;
        p += r; n -= (size_t)r; off += (uint64_t)r;
    }
    return true;
}

unsigned workers() {
    unsigned n = std::thread::hardware_concurrency();
    return std::max(1u, std::min(n ? n : 1u, 8u));
}

// Runs fn(begin, end) over [0, n) split across the CPU cores.
template <typename F>
void parallelFor(size_t n, F fn) {
    unsigned t = workers();
    if (t <= 1 || n < 2) { fn(size_t(0), n); return; }
    std::vector<std::thread> th;
    size_t per = (n + t - 1) / t;
    for (size_t b = 0; b < n; b += per) th.emplace_back(fn, b, std::min(n, b + per));
    for (auto& x : th) x.join();
}

// ---- Reed-Solomon over GF(2^8), Phil Karn's formulation as used by libfec:
// symsize 8, field polynomial 0x11d, first root 0, primitive element 1.
struct Rs {
    static constexpr int NN = 255, A0 = 255;
    int nroots;
    uint8_t alphaTo[256], indexOf[256], genpoly[256];

    explicit Rs(int roots) : nroots(roots) {
        alphaTo[A0] = 0;
        indexOf[0] = A0;
        unsigned sr = 1;
        for (int i = 0; i < NN; i++) {
            indexOf[sr] = (uint8_t)i;
            alphaTo[i] = (uint8_t)sr;
            sr <<= 1;
            if (sr & 256) sr ^= 0x11d;
            sr &= 255;
        }
        genpoly[0] = 1;
        for (int i = 0, root = 0; i < nroots; i++, root += 1) {
            genpoly[i + 1] = 1;
            for (int j = i; j > 0; j--)
                genpoly[j] = genpoly[j] ? uint8_t(genpoly[j - 1] ^ alphaTo[modnn(indexOf[genpoly[j]] + root)]) : genpoly[j - 1];
            genpoly[0] = alphaTo[modnn(indexOf[genpoly[0]] + root)];
        }
        for (int i = 0; i <= nroots; i++) genpoly[i] = indexOf[genpoly[i]];
    }
    static int modnn(int x) {
        while (x >= NN) { x -= NN; x = (x >> 8) + (x & NN); }
        return x;
    }
    void encode(const uint8_t* data, uint8_t* bb) const {
        memset(bb, 0, (size_t)nroots);
        for (int i = 0; i < NN - nroots; i++) {
            uint8_t fb = indexOf[data[i] ^ bb[0]];
            if (fb != A0)
                for (int j = 1; j < nroots; j++) bb[j] ^= alphaTo[modnn(fb + genpoly[nroots - j])];
            memmove(bb, bb + 1, size_t(nroots - 1));
            bb[nroots - 1] = fb != A0 ? alphaTo[modnn(fb + genpoly[0])] : 0;
        }
    }
    bool check(const uint8_t* cw) const {
        for (int i = 0; i < nroots; i++) {
            uint8_t s = cw[0];
            for (int j = 1; j < NN; j++) s = s ? uint8_t(cw[j] ^ alphaTo[modnn(indexOf[s] + i)]) : cw[j];
            if (s) return false;
        }
        return true;
    }
};

const Rs& rsFor(uint32_t roots) {
    static std::vector<Rs*> cache(256, nullptr);
    static std::mutex* mu = new std::mutex;
    std::lock_guard<std::mutex> l(*mu);
    if (!cache[roots]) cache[roots] = new Rs((int)roots);
    return *cache[roots];
}

}  // namespace

uint64_t hashTreeSize(uint64_t dataSize, uint32_t bs, uint32_t digestSize) {
    uint64_t total = 0, size = dataSize;
    while (size > bs) {
        uint64_t blocks = (size + bs - 1) / bs;
        uint64_t level = (blocks * digestSize + bs - 1) / bs * bs;
        total += level;
        size = level;
    }
    return total;
}

bool buildHashTree(int fd, uint64_t dataOffset, uint64_t dataSize, uint64_t treeOffset, uint64_t treeSize, uint32_t bs,
                   const std::string& algorithm, const std::string& salt, std::string& root, std::string& err,
                   const Progress& progress) {
    if (algorithm != "sha256") { err = "hash tree algorithm '" + algorithm + "' is not supported (only sha256)"; return false; }
    if (!bs || dataSize % bs) { err = "hash tree data is not a whole number of blocks"; return false; }
    std::vector<uint64_t> sizes;
    for (uint64_t size = dataSize; size > bs;) {
        uint64_t level = ((size + bs - 1) / bs * 32 + bs - 1) / bs * bs;
        sizes.push_back(level);
        size = level;
    }
    uint64_t total = 0;
    for (auto s : sizes) total += s;
    if (total != treeSize) {
        err = "hash tree size mismatch (payload says " + std::to_string(treeSize) + ", data needs " + std::to_string(total) + ")";
        return false;
    }
    if (sizes.empty()) {  // a single block: no tree, the root is that block's hash (avbtool does the same)
        std::vector<uint8_t> blk(bs, 0);
        if (dataSize && !preadAll(fd, blk.data(), bs, dataOffset)) { err = std::string("read failed: ") + strerror(errno); return false; }
        Sha256 h;
        h.update(salt.data(), salt.size());
        h.update(blk.data(), blk.size());
        root = h.digest();
        return true;
    }
    std::vector<uint64_t> offsets(sizes.size(), 0);  // level n sits after all the levels above it
    for (size_t n = 0; n < sizes.size(); n++)
        for (size_t m = n + 1; m < sizes.size(); m++) offsets[n] += sizes[m];
    std::vector<uint8_t> tree(total, 0);
    Sha256 salted;
    salted.update(salt.data(), salt.size());

    // Level 0: the data blocks, read in big chunks and hashed on every core.
    const size_t chunk = size_t(bs) * 2048;
    std::vector<uint8_t> buf(chunk);
    uint64_t blocks = dataSize / bs;
    for (uint64_t b = 0; b < blocks;) {
        size_t nb = (size_t)std::min<uint64_t>(blocks - b, chunk / bs);
        if (!preadAll(fd, buf.data(), nb * bs, dataOffset + b * bs)) { err = std::string("read failed: ") + strerror(errno); return false; }
        uint8_t* out = tree.data() + offsets[0] + b * 32;
        parallelFor(nb, [&](size_t lo, size_t hi) {
            for (size_t i = lo; i < hi; i++) {
                Sha256 h = salted;
                h.update(buf.data() + i * bs, bs);
                std::string d = h.digest();
                memcpy(out + i * 32, d.data(), 32);
            }
        });
        b += nb;
        if (progress) progress(b * bs, dataSize);
    }
    // Upper levels hash the level below them.
    for (size_t n = 1; n < sizes.size(); n++) {
        const uint8_t* src = tree.data() + offsets[n - 1];
        uint8_t* dst = tree.data() + offsets[n];
        size_t nb = (size_t)(sizes[n - 1] / bs);
        parallelFor(nb, [&](size_t lo, size_t hi) {
            for (size_t i = lo; i < hi; i++) {
                Sha256 h = salted;
                h.update(src + i * bs, bs);
                std::string d = h.digest();
                memcpy(dst + i * 32, d.data(), 32);
            }
        });
    }
    Sha256 h = salted;
    h.update(tree.data() + offsets.back(), sizes.back());
    root = h.digest();
    if (!pwriteAll(fd, tree.data(), tree.size(), treeOffset)) { err = std::string("writing the hash tree failed: ") + strerror(errno); return false; }
    return true;
}

uint64_t fecSize(uint64_t dataSize, uint32_t roots, uint32_t bs) {
    if (!roots || roots >= 255 || !bs) return 0;
    uint64_t rsn = 255 - roots, blocks = (dataSize + bs - 1) / bs;
    return (blocks + rsn - 1) / rsn * roots * bs;
}

bool encodeFec(int fd, uint64_t dataOffset, uint64_t dataSize, uint64_t fecOffset, uint64_t fecLen, uint32_t roots, uint32_t bs,
               std::string& err, const Progress& progress) {
    if (roots < 2 || roots >= 255) { err = "bad FEC roots (" + std::to_string(roots) + ")"; return false; }
    if (!bs || dataSize % bs) { err = "FEC data is not a whole number of blocks"; return false; }
    const uint64_t rsn = 255 - roots;
    const uint64_t rounds = (dataSize / bs + rsn - 1) / rsn;
    if (rounds * roots * bs != fecLen) {
        err = "FEC size mismatch (payload says " + std::to_string(fecLen) + ", data needs " + std::to_string(rounds * roots * bs) + ")";
        return false;
    }
    const Rs& rs = rsFor(roots);
    // Round i takes block (j * rounds + i) for j = 0..rsn-1, so R consecutive rounds read R
    // consecutive blocks at each j: one sequential read per j instead of R scattered ones.
    // libfec's fec_ecc_interleave() uses its FEC_BLOCKSIZE constant (4096), payloads always use 4096 too.
    const uint64_t kFecBlockSize = 4096;
    const uint64_t R = 16;
    std::vector<uint8_t> in(size_t(R * rsn * bs));  // [j][r][byte]
    std::vector<uint8_t> parity(size_t(R * bs * roots));
    for (uint64_t i0 = 0; i0 < rounds; i0 += R) {
        uint64_t nr = std::min(R, rounds - i0);
        for (uint64_t j = 0; j < rsn; j++) {
            uint8_t* dst = in.data() + (j * R) * bs;
            uint64_t off = i0 * bs + j * rounds * kFecBlockSize;  // fec_ecc_interleave()
            uint64_t want = nr * bs, have = off < dataSize ? std::min(want, dataSize - off) : 0;
            if (have && !preadAll(fd, dst, (size_t)have, dataOffset + off)) { err = std::string("read failed: ") + strerror(errno); return false; }
            if (have < want) memset(dst + have, 0, size_t(want - have));
        }
        // Each round: message k is byte k of each of its rsn blocks; parity k goes to k * roots.
        parallelFor((size_t)nr, [&](size_t lo, size_t hi) {
            std::vector<uint8_t> msgs(size_t(bs * rsn));
            for (size_t r = lo; r < hi; r++) {
                for (uint64_t j = 0; j < rsn; j++) {
                    const uint8_t* blk = in.data() + (j * R + r) * bs;
                    for (uint32_t k = 0; k < bs; k++) msgs[k * rsn + j] = blk[k];
                }
                uint8_t* out = parity.data() + r * bs * roots;
                for (uint32_t k = 0; k < bs; k++) rs.encode(msgs.data() + k * rsn, out + k * roots);
            }
        });
        if (!pwriteAll(fd, parity.data(), size_t(nr * bs * roots), fecOffset + i0 * bs * roots)) {
            err = std::string("writing FEC data failed: ") + strerror(errno);
            return false;
        }
        if (progress) progress(std::min(rounds, i0 + nr), rounds);
    }
    return true;
}

void rsEncode(uint32_t roots, const uint8_t* msg, uint8_t* parity) { rsFor(roots).encode(msg, parity); }
bool rsCheck(uint32_t roots, const uint8_t* cw) { return rsFor(roots).check(cw); }

}  // namespace tw::verity
