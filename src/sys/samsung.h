#pragma once
// Samsung firmware (Odin) AP packages on the phone itself: list the partition images inside an AP_*.tar / .tar.md5,
// check the appended MD5, and pull one out so it can go through FMRRE's normal, verified image-flash path. This is
// how you put the stock boot / recovery / vbmeta / dtbo back on a Samsung (an S8, say) without a PC or Odin.
//
// AP tars are plain ustar; the images are Android sparse, usually LZ4-compressed (boot.img.lz4, recovery.img.lz4,
// super.img.lz4 ...). Nothing here writes to a partition - it only reads the tar and copies one entry out.
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace tw {
class Task;
namespace samsung {

struct Image {
    std::string name;       // entry name, e.g. "boot.img.lz4"
    std::string partition;  // the partition it belongs on, e.g. "boot"
    uint64_t offset = 0;    // byte offset of the data in the tar
    uint64_t size = 0;      // compressed size on tar
    bool lz4 = false;
};

enum class Md5State { None, Ok, Mismatch, Bad };  // None: a plain .tar with no checksum

struct Package {
    std::vector<Image> images;
    Md5State md5 = Md5State::None;
    std::string md5name;  // the .tar name the checksum line referred to
};

// Reads the AP tar's directory and, for a .tar.md5, checks the appended MD5 of the tar. false + err on a damaged or
// non-tar file. The MD5 result is in pkg.md5 (a mismatch is reported, not fatal - the caller decides).
bool read(const std::string& path, Package& pkg, std::string& err);

// Copies one image entry out of the AP tar to `out` (kept compressed - the flash path decodes it). Progress on t.
bool extract(const std::string& path, const Image& img, const std::string& out, Task& t);

// The partition an AP image name belongs on ("boot.img.lz4" -> "boot", "" if it isn't a flashable image).
std::string partitionFor(const std::string& entryName);

}  // namespace samsung
}  // namespace tw
