#include "sys/imagesrc.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/fs.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "sys/history.h"
#include "core/log.h"
#include "core/sha256.h"
#include "core/task.h"
#include "core/util.h"
#include "platform/device.h"
#include "sys/partitions.h"
#include "sys/super.h"
#include "miniz.h"

// Must match third_party/xz/xz_config.h (that header defines min() and can't be included from C++).
#define XZ_USE_CRC64
#define XZ_DEC_CONCATENATED
extern "C" {
#include "xz.h"
}

namespace tw::img {

const char* compressionName(Compression c) {
    switch (c) {
        case Compression::Gzip: return "gzip";
        case Compression::Xz: return "xz";
        case Compression::Zstd: return "zstd";
        case Compression::Lz4: return "lz4";
        case Compression::Zip: return "zip";
        default: return "uncompressed";
    }
}

const char* kindName(Kind k) {
    switch (k) {
        case Kind::Ext4: return "ext4";
        case Kind::Erofs: return "EROFS";
        case Kind::F2fs: return "F2FS";
        case Kind::Boot: return "boot image";
        case Kind::VendorBoot: return "vendor_boot image";
        case Kind::Vbmeta: return "vbmeta";
        case Kind::Super: return "super image";
        case Kind::Dtbo: return "DTBO";
        case Kind::Fat: return "FAT";
        default: return "raw data";
    }
}

std::vector<std::string> imageExtensions() {
    return {".img", ".img.xz", ".img.gz", ".img.zst", ".img.lz4", ".bin", ".mbn", ".xz", ".gz", ".zst", ".lz4", ".raw", ".simg"};
}

bool isImageName(const std::string& name) {
    std::string n = util::toLower(name);
    for (auto& e : imageExtensions())
        if (util::endsWith(n, e)) return true;
    return false;
}

// ---- Source implementations -----------------------------------------------

bool Source::readFull(void* buf, size_t n) {
    char* p = (char*)buf;
    while (n) {
        ssize_t r = read(p, n);
        if (r <= 0) return false;
        p += r;
        n -= r;
    }
    return true;
}

bool Source::skip(uint64_t n) {
    char tmp[16384];
    while (n) {
        ssize_t r = read(tmp, std::min<uint64_t>(n, sizeof(tmp)));
        if (r <= 0) return false;
        n -= r;
    }
    return true;
}

namespace {

ssize_t readFd(int fd, void* buf, size_t n) {
    for (;;) {
        ssize_t r = ::read(fd, buf, n);
        if (r < 0 && errno == EINTR) continue;
        return r;
    }
}

class FileSource : public Source {
public:
    FileSource(int fd, uint64_t size) : fd_(fd), size_(size) {}
    ~FileSource() override { close(fd_); }
    ssize_t read(void* buf, size_t n) override {
        ssize_t r = readFd(fd_, buf, n);
        if (r < 0) error = strerror(errno);
        else { pos_ += r; produced += r; }
        return r;
    }
    float progress() const override { return size_ ? float(pos_) / size_ : -1; }

private:
    int fd_;
    uint64_t size_, pos_ = 0;
};

// Shared helper: buffered compressed input from a file descriptor.
struct InBuf {
    int fd = -1;
    uint64_t size = 0, pos = 0;
    std::vector<uint8_t> buf = std::vector<uint8_t>(1 << 18);
    size_t off = 0, len = 0;
    bool eof = false;
    bool fill() {
        if (off < len) return true;
        if (eof) return false;
        ssize_t r = readFd(fd, buf.data(), buf.size());
        if (r <= 0) { eof = true; return false; }
        off = 0;
        len = r;
        pos += r;
        return true;
    }
    float progress() const { return size ? float(pos - (len - off)) / size : -1; }
};

class GzipSource : public Source {
public:
    GzipSource(int fd, uint64_t size) {
        in_.fd = fd;
        in_.size = size;
    }
    ~GzipSource() override {
        if (inited_) mz_inflateEnd(&zs_);
        close(in_.fd);
    }
    float progress() const override { return in_.progress(); }

    ssize_t read(void* out, size_t n) override {
        if (done_) return 0;
        if (!inited_ && !startMember()) return done_ ? 0 : -1;
        zs_.next_out = (unsigned char*)out;
        zs_.avail_out = (unsigned)std::min<size_t>(n, 1u << 30);
        while (zs_.avail_out == (unsigned)std::min<size_t>(n, 1u << 30)) {
            if (zs_.avail_in == 0) {
                in_.off = in_.len;  // everything handed to zlib was consumed
                if (!in_.fill()) { error = "gzip: unexpected end of file"; return -1; }
                zs_.next_in = in_.buf.data() + in_.off;
                zs_.avail_in = unsigned(in_.len - in_.off);
            }
            int r = mz_inflate(&zs_, MZ_NO_FLUSH);
            if (r == MZ_STREAM_END) {
                size_t got = (unsigned char*)zs_.next_out - (unsigned char*)out;
                crc_ = mz_crc32(crc_, (unsigned char*)out, got);
                in_.off = in_.len - zs_.avail_in;
                if (!finishMember()) return -1;
                produced += got;
                return got ? (ssize_t)got : read(out, n);
            }
            if (r != MZ_OK && r != MZ_BUF_ERROR) { error = "gzip: corrupt data"; return -1; }
        }
        size_t got = (unsigned char*)zs_.next_out - (unsigned char*)out;
        crc_ = mz_crc32(crc_, (unsigned char*)out, got);
        produced += got;
        return got;
    }

private:
    bool getByte(uint8_t& b) {
        if (!in_.fill()) return false;
        b = in_.buf[in_.off++];
        return true;
    }
    bool startMember() {
        uint8_t h[10];
        for (int i = 0; i < 10; i++)
            if (!getByte(h[i])) { if (i == 0 && members_ > 0) { done_ = true; return false; } error = "gzip: truncated header"; return false; }
        if (h[0] != 0x1f || h[1] != 0x8b || h[2] != 8) {
            if (members_ > 0 && h[0] == 0) { done_ = true; return false; }  // zero padding after the last member
            error = "gzip: bad header";
            return false;
        }
        uint8_t flg = h[3], b;
        if (flg & 4) {
            uint8_t a, c;
            if (!getByte(a) || !getByte(c)) return false;
            for (int k = 0; k < (a | (c << 8)); k++) if (!getByte(b)) return false;
        }
        if (flg & 8) do { if (!getByte(b)) return false; } while (b);
        if (flg & 16) do { if (!getByte(b)) return false; } while (b);
        if (flg & 2) { getByte(b); getByte(b); }
        memset(&zs_, 0, sizeof(zs_));
        if (mz_inflateInit2(&zs_, -MZ_DEFAULT_WINDOW_BITS) != MZ_OK) { error = "gzip: init failed"; return false; }
        in_.fill();
        zs_.next_in = in_.buf.data() + in_.off;
        zs_.avail_in = unsigned(in_.len - in_.off);
        inited_ = true;
        crc_ = mz_crc32(0, nullptr, 0);
        return true;
    }
    bool finishMember() {
        mz_inflateEnd(&zs_);
        inited_ = false;
        uint8_t t[8];
        for (int i = 0; i < 8; i++)
            if (!getByte(t[i])) { error = "gzip: truncated trailer"; return false; }
        uint32_t crc = t[0] | (t[1] << 8) | (t[2] << 16) | ((uint32_t)t[3] << 24);
        if (crc != (uint32_t)crc_) { error = "gzip: CRC mismatch - the file is corrupt"; return false; }
        members_++;
        if (!in_.fill()) done_ = true;
        return true;
    }

    InBuf in_;
    mz_stream zs_{};
    bool inited_ = false, done_ = false;
    int members_ = 0;
    mz_ulong crc_ = 0;
};

class XzSource : public Source {
public:
    XzSource(int fd, uint64_t size) {
        in_.fd = fd;
        in_.size = size;
        static bool crcInit = false;
        if (!crcInit) { xz_crc32_init(); xz_crc64_init(); crcInit = true; }
        dec_ = xz_dec_init(XZ_DYNALLOC, 1u << 30);
    }
    ~XzSource() override {
        if (dec_) xz_dec_end(dec_);
        close(in_.fd);
    }
    float progress() const override { return in_.progress(); }

    ssize_t read(void* out, size_t n) override {
        if (done_) return 0;
        if (!dec_) { error = "xz: out of memory"; return -1; }
        xz_buf b{};
        b.out = (uint8_t*)out;
        b.out_size = n;
        while (b.out_pos == 0) {
            if (in_.off >= in_.len) in_.fill();
            b.in = in_.buf.data() + in_.off;
            b.in_size = in_.len - in_.off;
            b.in_pos = 0;
            bool finish = in_.eof && in_.off >= in_.len;
            enum xz_ret r = xz_dec_catrun(dec_, &b, finish);
            in_.off += b.in_pos;
            if (r == XZ_STREAM_END) { done_ = true; break; }
            if (r == XZ_OK) {
                if (finish && b.out_pos == 0) { error = "xz: truncated file"; return -1; }
                continue;
            }
            switch (r) {
                case XZ_MEM_ERROR: case XZ_MEMLIMIT_ERROR: error = "xz: not enough memory for this file's dictionary"; break;
                case XZ_FORMAT_ERROR: error = "xz: not an xz file"; break;
                case XZ_OPTIONS_ERROR: error = "xz: unsupported options (filters/check)"; break;
                case XZ_DATA_ERROR: case XZ_BUF_ERROR: error = "xz: corrupt or truncated data"; break;
                default: error = "xz: decode error"; break;
            }
            return -1;
        }
        produced += b.out_pos;
        return b.out_pos;
    }

private:
    InBuf in_;
    xz_dec* dec_ = nullptr;
    bool done_ = false;
};

class ZipSource : public Source {
public:
    ~ZipSource() override {
        if (it_) mz_zip_reader_extract_iter_free(it_);
        if (open_) mz_zip_reader_end(&z_);
    }
    bool init(const std::string& path, const std::string& entry, std::string& err) {
        if (!mz_zip_reader_init_file(&z_, path.c_str(), 0)) { err = "not a readable zip"; return false; }
        open_ = true;
        int idx = mz_zip_reader_locate_file(&z_, entry.c_str(), nullptr, 0);
        if (idx < 0) { err = "entry " + entry + " not found in zip"; return false; }
        mz_zip_archive_file_stat st;
        mz_zip_reader_file_stat(&z_, idx, &st);
        total_ = st.m_uncomp_size;
        it_ = mz_zip_reader_extract_iter_new(&z_, idx, 0);
        if (!it_) { err = "cannot read entry " + entry; return false; }
        return true;
    }
    ssize_t read(void* buf, size_t n) override {
        if (produced >= total_) return 0;
        size_t r = mz_zip_reader_extract_iter_read(it_, buf, n);
        if (r == 0) { error = "zip: corrupt entry"; return -1; }
        produced += r;
        return r;
    }
    float progress() const override { return total_ ? float(produced) / total_ : -1; }

private:
    mz_zip_archive z_{};
    mz_zip_reader_extract_iter_state* it_ = nullptr;
    bool open_ = false;
    uint64_t total_ = 0;
};

// External decompressor (zstd / lz4): the file is the child's stdin, so its
// shared file offset tells us how far it got.
class ProcSource : public Source {
public:
    ProcSource(int fd, uint64_t size, const std::string& cmd) : fd_(fd), size_(size) {
        int p[2];
        if (pipe2(p, O_CLOEXEC) != 0) return;
        pid_ = fork();
        if (pid_ == 0) {
            dup2(fd, 0);
            dup2(p[1], 1);
            int devnull = ::open("/dev/null", O_WRONLY);
            if (devnull >= 0) dup2(devnull, 2);
            std::string sh = util::shellPath();
            execl(sh.c_str(), "sh", "-c", cmd.c_str(), (char*)nullptr);
            _exit(127);
        }
        close(p[1]);
        out_ = p[0];
    }
    ~ProcSource() override {
        if (out_ >= 0) close(out_);
        if (pid_ > 0) { kill(pid_, SIGTERM); waitpid(pid_, nullptr, 0); }
        close(fd_);
    }
    ssize_t read(void* buf, size_t n) override {
        if (out_ < 0) { error = "cannot start decompressor"; return -1; }
        ssize_t r = readFd(out_, buf, n);
        if (r == 0 && pid_ > 0) {
            int st = 0;
            waitpid(pid_, &st, 0);
            pid_ = -1;
            if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
                error = WIFEXITED(st) && WEXITSTATUS(st) == 127 ? "decompressor not found in this ramdisk" : "decompressor failed";
                return -1;
            }
        }
        if (r > 0) produced += r;
        return r;
    }
    float progress() const override {
        off_t p = lseek(fd_, 0, SEEK_CUR);
        return size_ && p >= 0 ? float(p) / size_ : -1;
    }

private:
    int fd_, out_ = -1;
    pid_t pid_ = -1;
    uint64_t size_;
};

Compression sniff(const uint8_t* h, size_t n) {
    if (n >= 2 && h[0] == 0x1f && h[1] == 0x8b) return Compression::Gzip;
    if (n >= 6 && !memcmp(h, "\xFD" "7zXZ\0", 6)) return Compression::Xz;
    if (n >= 4 && !memcmp(h, "\x28\xB5\x2F\xFD", 4)) return Compression::Zstd;
    if (n >= 4 && !memcmp(h, "\x04\x22\x4D\x18", 4)) return Compression::Lz4;
    if (n >= 4 && !memcmp(h, "PK\x03\x04", 4)) return Compression::Zip;
    return Compression::None;
}

uint64_t le(const uint8_t* p, int n) {
    uint64_t v = 0;
    for (int i = n - 1; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

// Sum of uncompressed sizes from the xz index(es) at the end of the file.
uint64_t xzUncompressedSize(int fd, uint64_t fileSize) {
    uint64_t pos = fileSize, total = 0;
    auto rd = [&](uint64_t off, void* b, size_t n) { return pread(fd, b, n, off) == (ssize_t)n; };
    for (int streams = 0; pos >= 24 && streams < 64; streams++) {
        uint8_t ft[12];
        // skip stream padding
        while (pos >= 4) {
            uint8_t z[4];
            if (!rd(pos - 4, z, 4)) return 0;
            if (le(z, 4) != 0) break;
            pos -= 4;
        }
        if (!rd(pos - 12, ft, 12) || ft[10] != 'Y' || ft[11] != 'Z') return 0;
        uint64_t backward = (le(ft + 4, 4) + 1) * 4;
        if (backward > pos - 12 || backward > (64u << 20)) return 0;
        uint64_t idxStart = pos - 12 - backward;
        std::vector<uint8_t> idx(backward);
        if (!rd(idxStart, idx.data(), backward) || idx[0] != 0) return 0;
        size_t k = 1;
        auto varint = [&](uint64_t& v) {
            v = 0;
            for (int s = 0; s < 63 && k < idx.size(); s += 7) {
                uint8_t b = idx[k++];
                v |= uint64_t(b & 0x7f) << s;
                if (!(b & 0x80)) return true;
            }
            return false;
        };
        uint64_t recs, blocks = 0;
        if (!varint(recs)) return 0;
        for (uint64_t r = 0; r < recs; r++) {
            uint64_t unpadded, uncomp;
            if (!varint(unpadded) || !varint(uncomp)) return 0;
            total += uncomp;
            blocks += (unpadded + 3) & ~3ull;
        }
        uint64_t streamStart = idxStart - blocks - 12;
        if (blocks + 12 > idxStart) return 0;
        pos = streamStart;
        if (pos == 0) return total;
    }
    return pos == 0 ? total : 0;
}

uint64_t zstdContentSize(const uint8_t* h, size_t n) {
    if (n < 6) return 0;
    uint8_t fhd = h[4];
    int fcsFlag = fhd >> 6, single = (fhd >> 5) & 1, did = fhd & 3;
    size_t off = 5 + (single ? 0 : 1) + (did == 0 ? 0 : did == 1 ? 1 : did == 2 ? 2 : 4);
    int fcsSize = fcsFlag == 0 ? (single ? 1 : 0) : fcsFlag == 1 ? 2 : fcsFlag == 2 ? 4 : 8;
    if (!fcsSize || off + fcsSize > n) return 0;
    uint64_t v = le(h + off, fcsSize);
    return fcsSize == 2 ? v + 256 : v;
}

std::string shq(const std::string& s) { return util::shellQuote(s); }

}  // namespace

std::unique_ptr<Source> open(const std::string& path, const std::string& zipEntry, std::string& err) {
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) { err = path + ": " + strerror(errno); return nullptr; }
    uint64_t size = util::fileSize(path);
    uint8_t h[8] = {};
    ssize_t n = pread(fd, h, sizeof(h), 0);
    Compression c = sniff(h, n > 0 ? n : 0);
    switch (c) {
        case Compression::Gzip: return std::make_unique<GzipSource>(fd, size);
        case Compression::Xz: return std::make_unique<XzSource>(fd, size);
        case Compression::Zstd: return std::make_unique<ProcSource>(fd, size, "zstd -dc 2>/dev/null || toybox zstd -dc");
        case Compression::Lz4: return std::make_unique<ProcSource>(fd, size, "lz4 -dc");
        case Compression::Zip: {
            close(fd);
            auto z = std::make_unique<ZipSource>();
            std::string entry = zipEntry;
            if (entry.empty()) {
                Info i = inspect(path);
                if (i.zipImages.empty()) { err = "this zip has no .img inside (is it a flashable zip? use Install > Zip)"; return nullptr; }
                entry = i.zipImages[0];
            }
            if (!z->init(path, entry, err)) return nullptr;
            return z;
        }
        default: return std::make_unique<FileSource>(fd, size);
    }
}

// ---- inspection --------------------------------------------------------------

#pragma pack(push, 1)
struct SparseHeader {
    uint32_t magic;
    uint16_t major, minor, fileHdrSz, chunkHdrSz;
    uint32_t blkSz, totalBlks, totalChunks, checksum;
};
struct ChunkHeader {
    uint16_t type, reserved;
    uint32_t chunkSz, totalSz;
};
#pragma pack(pop)
static const uint32_t kSparseMagic = 0xED26FF3A;

static Kind kindOf(const uint8_t* d, size_t n, std::string& label) {
    if (n >= 8 && !memcmp(d, "ANDROID!", 8)) return Kind::Boot;
    if (n >= 8 && !memcmp(d, "VNDRBOOT", 8)) return Kind::VendorBoot;
    if (n >= 4 && !memcmp(d, "AVB0", 4)) return Kind::Vbmeta;
    if (n >= 4 && d[0] == 0xd7 && d[1] == 0xb7 && d[2] == 0xab && d[3] == 0x1e) return Kind::Dtbo;
    if (n >= 1082 && d[1080] == 0x53 && d[1081] == 0xEF) {
        if (n >= 1160) {
            label.assign((const char*)d + 1144, strnlen((const char*)d + 1144, 16));
        }
        return Kind::Ext4;
    }
    if (n >= 1028 && le(d + 1024, 4) == 0xE0F5E1E2) {
        if (n >= 1104) label.assign((const char*)d + 1088, strnlen((const char*)d + 1088, 16));
        return Kind::Erofs;
    }
    if (n >= 1028 && le(d + 1024, 4) == 0xF2F52010) return Kind::F2fs;
    if (n >= 4100 && le(d + 4096, 4) == 0x616c4467) return Kind::Super;
    if (n >= 512 && d[510] == 0x55 && d[511] == 0xAA && (n > 90 && (!memcmp(d + 54, "FAT", 3) || !memcmp(d + 82, "FAT", 3))))
        return Kind::Fat;
    return Kind::Unknown;
}

Info inspect(const std::string& path, const std::string& zipEntry) {
    Info info;
    info.path = path;
    info.fileSize = util::fileSize(path);
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) { info.error = strerror(errno); return info; }
    uint8_t h[64] = {};
    ssize_t hn = pread(fd, h, sizeof(h), 0);
    info.comp = sniff(h, hn > 0 ? hn : 0);
    uint64_t streamSize = info.fileSize;
    switch (info.comp) {
        case Compression::Xz:
            streamSize = xzUncompressedSize(fd, info.fileSize);
            info.sizeExact = streamSize != 0;
            break;
        case Compression::Gzip: {
            uint8_t t[4];
            if (pread(fd, t, 4, info.fileSize - 4) == 4) {
                streamSize = le(t, 4);  // ISIZE is modulo 4 GiB
                while (streamSize && streamSize < info.fileSize) streamSize += 1ull << 32;
            }
            info.sizeExact = info.fileSize < (1ull << 32) / 2;
            break;
        }
        case Compression::Zstd:
            streamSize = zstdContentSize(h, hn);
            info.sizeExact = streamSize != 0;
            break;
        case Compression::Lz4:
            streamSize = (hn >= 14 && (h[4] & 0x08)) ? le(h + 6, 8) : 0;
            info.sizeExact = streamSize != 0;
            break;
        case Compression::Zip: {
            mz_zip_archive z{};
            if (mz_zip_reader_init_file(&z, path.c_str(), 0)) {
                int n = (int)mz_zip_reader_get_num_files(&z);
                for (int i = 0; i < n; i++) {
                    mz_zip_archive_file_stat st;
                    if (!mz_zip_reader_file_stat(&z, i, &st) || st.m_is_directory) continue;
                    std::string name = st.m_filename;
                    if (util::endsWith(util::toLower(name), ".img")) info.zipImages.push_back(name);
                }
                info.zipEntry = zipEntry.empty() && !info.zipImages.empty() ? info.zipImages[0] : zipEntry;
                if (!info.zipEntry.empty()) {
                    int idx = mz_zip_reader_locate_file(&z, info.zipEntry.c_str(), nullptr, 0);
                    mz_zip_archive_file_stat st;
                    if (idx >= 0 && mz_zip_reader_file_stat(&z, idx, &st)) streamSize = st.m_uncomp_size;
                }
                mz_zip_reader_end(&z);
            }
            if (info.zipImages.empty()) {
                close(fd);
                info.error = "No .img inside this zip. Flashable zips go in Install > Zip.";
                return info;
            }
            break;
        }
        default: break;
    }
    close(fd);

    // Peek at the expanded content.
    std::string err;
    auto src = open(path, info.zipEntry, err);
    if (!src) { info.error = err; return info; }
    std::vector<uint8_t> head(8192);
    size_t got = 0;
    while (got < head.size()) {
        ssize_t r = src->read(head.data() + got, head.size() - got);
        if (r <= 0) break;
        got += r;
    }
    if (got == 0) { info.error = src->error.empty() ? "empty file" : src->error; return info; }
    SparseHeader sh{};
    memcpy(&sh, head.data(), std::min(got, sizeof(sh)));
    if (got >= sizeof(sh) && sh.magic == kSparseMagic) {
        info.sparse = true;
        info.imageSize = uint64_t(sh.totalBlks) * sh.blkSz;
        info.sizeExact = true;
        // First chunk's data, if it is a raw chunk, holds the superblock.
        size_t off = sh.fileHdrSz;
        if (off + sizeof(ChunkHeader) <= got) {
            ChunkHeader ch{};
            memcpy(&ch, head.data() + off, sizeof(ch));
            off += sh.chunkHdrSz;
            if (ch.type == 0xCAC1 && off < got) info.kind = kindOf(head.data() + off, got - off, info.fsLabel);
        }
    } else {
        info.imageSize = streamSize;
        info.kind = kindOf(head.data(), got, info.fsLabel);
    }
    std::string lname = util::toLower(util::basename(path) + " " + info.zipEntry);
    bool sysName = lname.find("gsi") != std::string::npos || lname.find("system") != std::string::npos ||
                   lname.find("treble") != std::string::npos || lname.find("-ab-") != std::string::npos ||
                   lname.find("arm64_b") != std::string::npos;
    bool sysFs = info.kind == Kind::Ext4 || info.kind == Kind::Erofs;
    info.looksLikeGsi = sysFs && (sysName || info.imageSize >= (700ull << 20) || info.fsLabel == "/" || info.fsLabel == "system");
    info.ok = true;
    return info;
}

std::string Info::describe() const {
    std::string s;
    auto add = [&](const std::string& p) { s += (s.empty() ? "" : "  \xE2\x80\xA2  ") + p; };
    if (comp == Compression::Zip) add("in zip: " + util::basename(zipEntry));
    else if (comp != Compression::None) add(compressionName(comp));
    if (sparse) add("sparse");
    add(kindName(kind));
    if (imageSize) add((sizeExact ? "" : "~") + util::humanSize(imageSize));
    return s;
}

// ---- flashing -------------------------------------------------------------------

namespace {

struct Writer {
    int fd = -1;
    Task* t = nullptr;
    Source* src = nullptr;
    uint64_t limit = 0, expected = 0, written = 0;
    bool verify = false;
    Sha256 sha;
    std::vector<std::pair<uint64_t, uint64_t>> ranges;
    std::vector<char> buf = std::vector<char>(4 << 20);
    int64_t t0 = util::nowMs(), lastUi = 0;
    std::string err;

    void tick() {
        int64_t now = util::nowMs();
        if (now - lastUi < 100) return;
        lastUi = now;
        float p = src->progress();
        if (p < 0 && expected) p = float(written) / expected;
        if (p >= 0) t->setSubProgress(std::min(1.f, p));
        int64_t el = now - t0;
        if (el > 0)
            t->setDetail(util::humanSize(written) + "  \xE2\x80\xA2  " + util::humanSize(written * 1000 / el) + "/s");
    }
    bool put(uint64_t pos, const char* data, size_t n) {
        if (limit && pos + n > limit) {
            err = "image is larger than the partition (" + util::humanSize(limit) + ")";
            return false;
        }
        size_t done = 0;
        while (done < n) {
            ssize_t r = pwrite(fd, data + done, n - done, pos + done);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) { err = std::string("write failed: ") + strerror(errno); return false; }
            done += r;
        }
        if (verify) {
            sha.update(data, n);
            if (!ranges.empty() && ranges.back().first + ranges.back().second == pos) ranges.back().second += n;
            else ranges.push_back({pos, n});
        }
        written += n;
        tick();
        return true;
    }
    // Copy `len` bytes from the source to `pos`.
    bool copy(uint64_t pos, uint64_t len) {
        while (len) {
            size_t n = std::min<uint64_t>(len, buf.size());
            if (!src->readFull(buf.data(), n)) { err = src->error.empty() ? "unexpected end of image" : src->error; return false; }
            if (!put(pos, buf.data(), n)) return false;
            pos += n;
            len -= n;
            if (t->cancelled()) { err = "cancelled"; return false; }
        }
        return true;
    }
};

}  // namespace

static bool flashImpl(const std::string& path, const std::string& zipEntry, const std::string& dev, const FlashOptions& o, Task& t) {
    Info info = inspect(path, zipEntry);
    if (!info.ok) { t.print("Cannot read %s: %s", util::basename(path).c_str(), info.error.c_str()); return false; }
    t.print("Image: %s", util::basename(path).c_str());
    t.print("  %s", info.describe().c_str());
    t.print("Target: %s", dev.c_str());
    uint64_t limit = o.limit ? o.limit : (device::isSim() ? 0 : Partitions::blockSize(dev));
    if (limit && info.imageSize && info.sizeExact && info.imageSize > limit) {
        t.print("Image (%s) does not fit in the partition (%s).", util::humanSize(info.imageSize).c_str(), util::humanSize(limit).c_str());
        if (util::startsWith(dev, "/dev/block/mapper/") || dev.find("/mapper/") != std::string::npos)
            t.print("This is a dynamic partition: resize it in Advanced > Super partitions, or use Install > GSI.");
        return false;
    }
    if (o.precheck && info.comp != Compression::None) {
        t.setStatus("Checking archive integrity");
        std::string perr;
        auto chk = open(path, info.zipEntry, perr);
        if (!chk) { t.print("%s", perr.c_str()); return false; }
        std::vector<char> b(4 << 20);
        float base = t.rangeStart(), len = t.rangeLen();
        t.setRange(base, len * 0.4f);
        for (;;) {
            ssize_t r = chk->read(b.data(), b.size());
            if (r < 0) { t.print("Archive is corrupt: %s. Nothing was written.", chk->error.c_str()); return false; }
            if (r == 0) break;
            if (chk->progress() >= 0) t.setSubProgress(chk->progress());
            if (t.cancelled()) return false;
        }
        t.print("Archive OK (%s decompressed)", util::humanSize(chk->produced).c_str());
        t.setRange(base + len * 0.4f, len * 0.6f);
    }
    std::string werr;
    if (!lp::ensureWritable(dev, werr)) {
        t.print("%s is read-only and could not be remapped: %s", dev.c_str(), werr.c_str());
        return false;
    }
    std::string err;
    auto src = open(path, info.zipEntry, err);
    if (!src) { t.print("%s", err.c_str()); return false; }
    int fd = ::open(dev.c_str(), O_WRONLY | O_CLOEXEC | (device::isSim() ? O_CREAT : 0), 0644);
    if (fd < 0) { t.print("Cannot open %s: %s", dev.c_str(), strerror(errno)); return false; }
    t.setStatus("Flashing " + util::basename(path));

    Writer w;
    w.fd = fd;
    w.t = &t;
    w.src = src.get();
    w.limit = limit;
    w.expected = info.imageSize;
    w.verify = o.verify;
    bool ok = true;

    SparseHeader sh{};
    char head[sizeof(SparseHeader)];
    size_t hn = 0;
    while (hn < sizeof(head)) {
        ssize_t r = src->read(head + hn, sizeof(head) - hn);
        if (r <= 0) break;
        hn += r;
    }
    memcpy(&sh, head, std::min(hn, sizeof(sh)));
    if (hn == sizeof(sh) && sh.magic == kSparseMagic) {
        if (sh.fileHdrSz < sizeof(sh) || sh.chunkHdrSz < sizeof(ChunkHeader) || sh.blkSz == 0 || sh.blkSz % 4) {
            t.print("Corrupt sparse header");
            close(fd);
            return false;
        }
        t.print("Sparse image: %u chunks, %s expanded", sh.totalChunks, util::humanSize(uint64_t(sh.totalBlks) * sh.blkSz).c_str());
        ok = src->skip(sh.fileHdrSz - sizeof(sh));
        uint64_t pos = 0;
        for (uint32_t c = 0; c < sh.totalChunks && ok; c++) {
            ChunkHeader ch{};
            if (!src->readFull(&ch, sizeof(ch)) || !src->skip(sh.chunkHdrSz - sizeof(ch))) { w.err = "truncated sparse image"; ok = false; break; }
            uint64_t len = uint64_t(ch.chunkSz) * sh.blkSz;
            switch (ch.type) {
                case 0xCAC1: ok = w.copy(pos, len); break;
                case 0xCAC2: {
                    uint32_t v;
                    if (!src->readFull(&v, 4)) { ok = false; w.err = "truncated fill chunk"; break; }
                    for (size_t i = 0; i + 4 <= w.buf.size(); i += 4) memcpy(&w.buf[i], &v, 4);
                    uint64_t left = len, p = pos;
                    while (left && ok) {
                        size_t n = std::min<uint64_t>(left, w.buf.size());
                        ok = w.put(p, w.buf.data(), n);
                        p += n;
                        left -= n;
                    }
                    break;
                }
                case 0xCAC3: w.tick(); break;
                case 0xCAC4: ok = src->skip(4); len = 0; break;
                default: w.err = "unknown sparse chunk type"; ok = false;
            }
            pos += len;
            if (t.cancelled()) { w.err = "cancelled"; ok = false; }
        }
    } else {
        ok = hn == 0 || w.put(0, head, hn);
        uint64_t pos = hn;
        while (ok && hn == sizeof(head)) {
            ssize_t r = src->read(w.buf.data(), w.buf.size());
            if (r < 0) { w.err = src->error; ok = false; break; }
            if (r == 0) break;
            ok = w.put(pos, w.buf.data(), r);
            pos += r;
            if (t.cancelled()) { w.err = "cancelled"; ok = false; }
        }
    }
    fsync(fd);
    ioctl(fd, BLKFLSBUF, 0);
    close(fd);
    if (!ok) {
        t.print("FAILED after %s: %s", util::humanSize(w.written).c_str(), w.err.c_str());
        return false;
    }
    t.setSubProgress(1);
    t.print("Wrote %s in %s", util::humanSize(w.written).c_str(), util::humanDuration(util::nowMs() - w.t0).c_str());

    if (o.verify) {
        t.setStatus("Verifying what was written");
        std::string want = w.sha.hexDigest();
        int rfd = ::open(dev.c_str(), O_RDONLY | O_CLOEXEC);
        if (rfd < 0) { t.print("Cannot reopen for verification"); return false; }
        Sha256 back;
        uint64_t done = 0;
        for (auto& [pos, len] : w.ranges) {
            uint64_t left = len, p = pos;
            while (left) {
                size_t n = std::min<uint64_t>(left, w.buf.size());
                ssize_t r = pread(rfd, w.buf.data(), n, p);
                if (r <= 0) { close(rfd); t.print("Read-back failed"); return false; }
                back.update(w.buf.data(), r);
                p += r;
                left -= r;
                done += r;
                t.setSubProgress(float(done) / std::max<uint64_t>(1, w.written));
            }
        }
        close(rfd);
        if (back.hexDigest() != want) { t.print("VERIFY FAILED: data on %s does not match the image!", dev.c_str()); return false; }
        t.print("Verified: SHA-256 of written data matches (%s...)", want.substr(0, 16).c_str());
    }
    return true;
}

bool flash(const std::string& path, const std::string& zipEntry, const std::string& dev, const FlashOptions& o, Task& t) {
    bool ok = flashImpl(path, zipEntry, dev, o, t);
    history::record("Flash image", util::basename(dev) + " <- " + util::basename(zipEntry.empty() ? path : zipEntry), ok);
    return ok;
}

// ---- dd command generation ----------------------------------------------------

DdCommand ddCommand(const Info& info, const std::string& dev) {
    DdCommand d;
    std::string p = shq(info.path), out = shq(dev);
    std::string cat;
    switch (info.comp) {
        case Compression::Xz: cat = "xz -dc " + p; break;
        case Compression::Gzip: cat = "gzip -dc " + p; break;
        case Compression::Zstd: cat = "zstd -dc " + p; break;
        case Compression::Lz4: cat = "lz4 -dc " + p; break;
        case Compression::Zip: cat = "unzip -p " + p + " " + shq(info.zipEntry.empty() ? "system.img" : info.zipEntry); break;
        default: break;
    }
    bool mapper = dev.find("/mapper/") != std::string::npos;
    if (mapper) d.lines.push_back("blockdev --setrw " + out);
    if (info.sparse) {
        d.needsSimg2img = true;
        d.notes.push_back("This is an Android SPARSE image: dd would write it scrambled. simg2img expands it while writing.");
        if (cat.empty()) {
            d.lines.push_back("simg2img " + p + " " + out);
        } else {
            std::string tmp = "/data/local/tmp/" + util::basename(info.zipEntry.empty() ? info.path : info.zipEntry);
            for (auto ext : {".xz", ".gz", ".zst", ".lz4"})
                if (util::endsWith(tmp, ext)) tmp = tmp.substr(0, tmp.size() - strlen(ext));
            d.lines.push_back(cat + " > " + shq(tmp));
            d.lines.push_back("simg2img " + shq(tmp) + " " + out);
            d.lines.push_back("rm " + shq(tmp));
            d.notes.push_back("Needs free space in /data for the decompressed file (up to " +
                              util::humanSize(info.imageSize ? info.imageSize : info.fileSize) + ").");
        }
        d.notes.push_back("Or skip all this: the built-in flasher streams sparse + compressed images directly.");
    } else if (cat.empty()) {
        d.lines.push_back("dd if=" + p + " of=" + out + " bs=4M conv=fsync status=progress");
    } else {
        d.lines.push_back(cat + " | dd of=" + out + " bs=4M conv=fsync");
        if (info.comp == Compression::Xz) d.notes.push_back("No xz binary? Use 'xzcat' (toybox) or 'busybox xz -dc' instead of 'xz -dc'.");
        if (info.comp == Compression::Gzip) d.notes.push_back("No gzip binary? Use 'zcat' (toybox) instead of 'gzip -dc'.");
    }
    d.lines.push_back("sync");
    if (mapper)
        d.notes.push_back("Logical partition: recovery maps it read-only. 'blockdev --setrw' clears that on most kernels; if dd still "
                          "says 'Read-only file system', use the built-in flasher - it reloads the mapping read-write.");
    if (info.imageSize) {
        uint64_t sz = device::isSim() ? 0 : Partitions::blockSize(dev);
        if (sz && info.imageSize > sz)
            d.notes.push_back("WARNING: the image (" + util::humanSize(info.imageSize) + ") is bigger than " + util::basename(dev) + " (" +
                              util::humanSize(sz) + "). Resize it first (Advanced > Super partitions) or dd will fail half-way.");
    }
    return d;
}

}  // namespace tw::img
