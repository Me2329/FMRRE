#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <sys/types.h>
#include <vector>

namespace tw {

class Task;

// Images as people actually download them: raw or Android-sparse .img, possibly
// wrapped in .xz / .gz / .zst / .lz4 or inside a .zip. Everything is streamed:
// nothing is decompressed to a temporary file first.
namespace img {

enum class Compression { None, Gzip, Xz, Zstd, Lz4, Zip };
enum class Kind { Unknown, Ext4, Erofs, F2fs, Boot, VendorBoot, Vbmeta, Super, Dtbo, Fat };

const char* compressionName(Compression c);
const char* kindName(Kind k);

// A forward-only byte stream of the (decompressed) image.
class Source {
public:
    virtual ~Source() = default;
    // Returns bytes read, 0 at the end, -1 on error (see error).
    virtual ssize_t read(void* buf, size_t n) = 0;
    // Progress through the file on disk, 0..1 (or -1 if unknown).
    virtual float progress() const = 0;
    bool readFull(void* buf, size_t n);  // false on short read
    bool skip(uint64_t n);
    std::string error;
    uint64_t produced = 0;
};

std::unique_ptr<Source> open(const std::string& path, const std::string& zipEntry, std::string& err);

struct Info {
    bool ok = false;
    std::string error;
    std::string path, zipEntry;
    std::vector<std::string> zipImages;  // .img entries found in a zip
    Compression comp = Compression::None;
    Kind kind = Kind::Unknown;           // of the expanded content
    bool sparse = false;
    uint64_t fileSize = 0;               // on disk
    uint64_t imageSize = 0;              // bytes once written to a partition; 0 = unknown
    bool sizeExact = true;
    bool looksLikeGsi = false;           // system image (ext4/erofs) big enough to be a GSI
    std::string fsLabel;                 // ext4 volume name / erofs volume name when present
    std::string describe() const;        // "xz · sparse · ext4 · 3.2 GB"
};
Info inspect(const std::string& path, const std::string& zipEntry = "");

// Recognise compressed/image extensions people use for flashable images.
bool isImageName(const std::string& name);
std::vector<std::string> imageExtensions();

struct FlashOptions {
    bool verify = false;      // read back and compare SHA-256 of what was written
    bool precheck = false;    // compressed images: decompress once to check integrity before writing anything
    uint64_t limit = 0;       // partition size; 0 = don't check
};
// Stream `path` (any supported format) onto `dev`. Sparse images are expanded.
bool flash(const std::string& path, const std::string& zipEntry, const std::string& dev, const FlashOptions& o, Task& t);

// Decompress `path` (.xz .gz .lz4 .zst, or the .img inside a .zip) to the file `out`, as it is: an Android sparse
// image stays sparse. summary: "boot.img.lz4 -> boot.img: 64 MB (lz4)".
bool unpack(const std::string& path, const std::string& out, Task& t, std::string* summary = nullptr);
// Where unpack() puts it by default: next to the file, the compression extension dropped ("boot.img.lz4" ->
// "boot.img"; a zip -> the name of the image inside it), with "-unpacked" when that name is taken.
std::string unpackedName(const std::string& path);

// Build the shell command that does the same as flash() with standard tools
// (dd, xz, gzip, zstd, unzip, simg2img). Returned lines are ready to paste into
// a terminal; `notes` explains caveats (e.g. sparse images).
struct DdCommand {
    std::vector<std::string> lines;
    std::vector<std::string> notes;
    bool needsSimg2img = false;
};
DdCommand ddCommand(const Info& info, const std::string& dev);

}  // namespace img
}  // namespace tw
