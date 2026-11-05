#include "sys/samsung.h"

#include <cstring>
#include <fcntl.h>
#include <unistd.h>

#include "core/md5.h"
#include "core/sha256.h"  // Sha256::toHex hexes any binary digest
#include "core/task.h"
#include "core/util.h"

namespace tw::samsung {

namespace {

constexpr size_t kBlock = 512;

uint64_t octal(const char* p, size_t n) {
    // ustar sizes are octal ASCII; GNU base-256 (high bit set) is also accepted for large images.
    if (n && (static_cast<unsigned char>(p[0]) & 0x80)) {
        uint64_t v = static_cast<unsigned char>(p[0]) & 0x7F;
        for (size_t i = 1; i < n; i++) v = (v << 8) | static_cast<unsigned char>(p[i]);
        return v;
    }
    uint64_t v = 0;
    size_t i = 0;
    while (i < n && (p[i] == ' ' || p[i] == 0)) i++;
    for (; i < n && p[i] >= '0' && p[i] <= '7'; i++) v = (v << 3) | uint64_t(p[i] - '0');
    return v;
}

bool zeroBlock(const char* b) {
    for (size_t i = 0; i < kBlock; i++)
        if (b[i]) return false;
    return true;
}

// The .tar.md5 trailer is the tar followed by "<md5hex>  <name>\n". The tar ends in zero padding, so the trailer is
// the printable run at the very end. Returns the trailer length (0 if there is none) and the expected hex / name.
size_t findMd5Trailer(int fd, uint64_t fsize, std::string& hex, std::string& name) {
    if (fsize < 40) return 0;
    size_t tail = size_t(std::min<uint64_t>(fsize, 1024));
    std::vector<char> buf(tail);
    if (pread(fd, buf.data(), tail, off_t(fsize - tail)) != ssize_t(tail)) return 0;
    size_t end = tail;
    while (end > 0 && (buf[end - 1] == '\n' || buf[end - 1] == '\r' || buf[end - 1] == ' ')) end--;
    size_t start = end;
    while (start > 0 && buf[start - 1] != 0 && buf[start - 1] != '\n') start--;
    std::string line(buf.data() + start, end - start);
    // "<32 hex>  <name>"
    size_t sp = line.find_first_of(" \t");
    if (sp != 32) return 0;
    for (char ch : line.substr(0, 32))
        if (!isxdigit((unsigned char)ch)) return 0;
    hex = util::toLower(line.substr(0, 32));
    name = util::trim(line.substr(sp));
    return tail - start;  // bytes from the trailer start to EOF
}

}  // namespace

std::string partitionFor(const std::string& entryName) {
    std::string n = util::toLower(entryName);
    if (n.find('/') != std::string::npos) return "";  // a path (meta-data/...), not a top-level image
    // strip a compression suffix, then an image suffix (boot.img.lz4 -> boot.img -> boot)
    for (const char* ext : {".lz4", ".gz", ".xz", ".zst"})
        if (util::endsWith(n, ext)) { n = n.substr(0, n.size() - strlen(ext)); break; }
    for (const char* ext : {".img", ".ext4", ".bin", ".mbn"})
        if (util::endsWith(n, ext)) { n = n.substr(0, n.size() - strlen(ext)); break; }
    // meta / signature members of an AP that are not partition images
    static const char* kSkip[] = {"meta-data", "metadata", "sha256", "ltable", "removed", "cache"};
    for (const char* s : kSkip)
        if (n == s || n.find(s) != std::string::npos) return "";
    if (n.empty()) return "";
    return n;  // boot, recovery, vbmeta, dtbo, super, system, userdata, up_param, ...
}

bool read(const std::string& path, Package& pkg, std::string& err) {
    pkg = Package{};
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return err = std::string("cannot open ") + util::basename(path) + ": " + strerror(errno), false;
    uint64_t fsize = util::fileSize(path);

    // MD5 (.tar.md5): the trailer is after the tar; verify the tar part.
    std::string hex, md5name;
    size_t trailer = findMd5Trailer(fd, fsize, hex, md5name);
    uint64_t tarLen = fsize - trailer;
    if (trailer) {
        pkg.md5name = md5name;
        Md5 m;  // the hash class (core/md5.h)
        uint8_t buf[1 << 16];
        uint64_t done = 0;
        bool ok = true;
        while (done < tarLen) {
            size_t k = size_t(std::min<uint64_t>(sizeof buf, tarLen - done));
            if (pread(fd, buf, k, off_t(done)) != ssize_t(k)) { ok = false; break; }
            m.update(buf, k);
            done += k;
        }
        pkg.md5 = !ok ? Md5State::Bad : (util::toLower(Sha256::toHex(m.digest())) == hex ? Md5State::Ok : Md5State::Mismatch);
    }

    // Walk the tar headers.
    char h[kBlock];
    uint64_t pos = 0;
    int zeros = 0;
    while (pos + kBlock <= tarLen) {
        if (pread(fd, h, kBlock, off_t(pos)) != ssize_t(kBlock)) break;
        pos += kBlock;
        if (zeroBlock(h)) {
            if (++zeros >= 2) break;  // end of archive
            continue;
        }
        zeros = 0;
        if (memcmp(h + 257, "ustar", 5) != 0 && h[0] == 0) continue;  // not a header we understand
        char type = h[156];
        uint64_t size = octal(h + 124, 12);
        std::string name(h, strnlen(h, 100));
        uint64_t data = pos;
        pos += (size + kBlock - 1) / kBlock * kBlock;  // skip the data to the next header
        if (type != '0' && type != 0) continue;        // only regular files are images
        std::string part = partitionFor(name);
        if (part.empty()) continue;
        pkg.images.push_back({name, part, data, size, util::endsWith(util::toLower(name), ".lz4")});
    }
    ::close(fd);
    if (pkg.images.empty()) return err = "no partition images found - is this a Samsung AP file?", false;
    return true;
}

bool extract(const std::string& path, const Image& img, const std::string& out, Task& t) {
    int in = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (in < 0) { t.print("cannot open %s", util::basename(path).c_str()); return false; }
    int o = ::open(out.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (o < 0) { ::close(in); t.print("cannot write %s", out.c_str()); return false; }
    t.setStatus("Extracting " + img.name);
    std::vector<uint8_t> buf(1 << 20);
    uint64_t done = 0;
    bool ok = true;
    while (done < img.size && !t.cancelled()) {
        size_t k = size_t(std::min<uint64_t>(buf.size(), img.size - done));
        if (pread(in, buf.data(), k, off_t(img.offset + done)) != ssize_t(k)) { ok = false; break; }
        size_t w = 0;
        while (w < k) {
            ssize_t r = ::write(o, buf.data() + w, k - w);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) { ok = false; break; }
            w += size_t(r);
        }
        if (w < k) { ok = false; break; }
        done += k;
        if (img.size) t.setSubProgress(float(double(done) / double(img.size)));
    }
    if (ok && fsync(o) != 0) ok = false;
    ::close(in);
    ::close(o);
    if (!ok || t.cancelled()) {
        unlink(out.c_str());
        if (!t.cancelled()) t.print("extract failed");
        return false;
    }
    return true;
}

}  // namespace tw::samsung
