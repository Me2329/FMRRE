#include "sys/pathcheck.h"

#include <cerrno>
#include <algorithm>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "core/util.h"
#include "platform/device.h"
#include "sys/imagesrc.h"
#include "sys/partitions.h"

namespace tw::pathcheck {

namespace {

std::string internalRoot() {
    std::string m = device::path("/data/media/0");
    return util::isDir(m) ? m : Partitions::get().storagePath();
}

size_t editDistance(const std::string& a, const std::string& b) {
    std::vector<size_t> prev(b.size() + 1), cur(b.size() + 1);
    for (size_t j = 0; j <= b.size(); j++) prev[j] = j;
    for (size_t i = 1; i <= a.size(); i++) {
        cur[0] = i;
        for (size_t j = 1; j <= b.size(); j++)
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1)});
        std::swap(prev, cur);
    }
    return prev[b.size()];
}

// A file is an image if its content says so, whatever its name.
bool contentIsImage(const std::string& path) {
    if (util::fileSize(path) < 4096) return false;
    img::Info i = img::inspect(path);
    return i.ok && (i.sparse || i.kind != img::Kind::Unknown);
}

std::string stripQuotes(std::string s) {
    s = util::trim(s);
    if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') || (s.front() == '\'' && s.back() == '\''))) s = s.substr(1, s.size() - 2);
    return s;
}

// The longest of these prefixes that matches is replaced by the internal storage path.
const char* kInternalAliases[] = {"/storage/emulated/0", "/storage/self/primary", "/storage/emulated/legacy", "/mnt/sdcard",
                                  "/mnt/user/0/primary", "/sdcard", "/storage/sdcard0"};

std::string removableRoot(std::string& volumeHint) {
    for (auto* p : Partitions::get().storages())
        if (p->removable && p->mounted) { volumeHint = p->displayName; return p->contentPath(); }
    return "";
}

}  // namespace

std::string resolve(const std::string& inputIn) {
    std::string in = stripQuotes(inputIn);
    if (in.empty()) return in;
    if (in[0] != '/') in = "/sdcard/" + in;  // bare file names are almost always on internal storage
    for (auto a : kInternalAliases) {
        std::string al = a;
        if (in == al || util::startsWith(in, al + "/")) return internalRoot() + in.substr(al.size());
    }
    // /storage/ABCD-1234/...  -> mounted microSD / USB
    if (util::startsWith(in, "/storage/") && in.size() > 18 && in[13] == '-') {
        std::string hint, root = removableRoot(hint);
        size_t slash = in.find('/', 9);
        if (!root.empty()) return root + (slash == std::string::npos ? "" : in.substr(slash));
    }
    if (device::isSim() && !util::startsWith(in, device::simRoot())) return device::path(in);
    return in;
}

std::string pretty(const std::string& real) {
    std::string root = internalRoot();
    if (util::startsWith(real, root)) return "/sdcard" + real.substr(root.size());
    if (device::isSim() && util::startsWith(real, device::simRoot())) {
        std::string r = real.substr(device::simRoot().size());
        return r.empty() ? "/" : r;
    }
    return real;
}

Report check(const std::string& input) {
    Report r;
    r.input = stripQuotes(input);
    r.resolved = resolve(r.input);
    auto add = [&](Level l, const std::string& s) { r.findings.push_back({l, s}); };
    if (r.input.empty()) { add(Level::Error, "Type the full path of the file, e.g. /sdcard/Download/system.img"); return r; }
    if (r.resolved != r.input && !device::isSim())
        add(Level::Info, r.input + " is " + r.resolved + " in recovery (Android's /sdcard doesn't exist here).");

    std::string root = internalRoot();
    bool onInternal = util::startsWith(r.resolved, device::path("/data"));
    if (onInternal) {
        Partition* d = Partitions::get().find("/data");
        if (d && !d->mounted && !Partitions::get().isMounted(*d))
            add(Level::Error, "Data is not mounted, so internal storage is unavailable. Mount it in Mount, or copy the file to a microSD/USB drive.");
        else if (Partitions::get().dataEncrypted())
            add(Level::Error, "Internal storage is encrypted (FBE) and can't be read in recovery yet. Copy the file to a microSD / USB OTG drive, "
                              "or run: adb push <file> /tmp/");
    }
    bool onRemovable = util::startsWith(r.input, "/storage/") && !onInternal;
    if (onRemovable && r.resolved == r.input && !device::isSim())
        add(Level::Error, "That looks like a microSD/USB path but no removable storage is mounted. Plug it in and mount it in Mount > USB OTG / MicroSD.");

    struct stat st{};
    if (stat(r.resolved.c_str(), &st) == 0) {
        r.exists = true;
        r.isFile = S_ISREG(st.st_mode);
        r.size = st.st_size;
        if (S_ISDIR(st.st_mode)) {
            add(Level::Error, "This is a folder. Add the file name at the end.");
            for (auto& e : util::listDir(r.resolved, true))
                if (!e.dir && (img::isImageName(e.name) || util::endsWith(util::toLower(e.name), ".zip")))
                    r.suggestions.push_back(util::joinPath(r.resolved, e.name));
            return r;
        }
        if (S_ISBLK(st.st_mode)) { add(Level::Warn, "This is a block device, not a file. dd can read it, but it isn't an image file."); r.isFile = true; }
        int fd = open(r.resolved.c_str(), O_RDONLY | O_CLOEXEC);
        r.readable = fd >= 0;
        if (fd < 0) add(Level::Error, std::string("Exists but cannot be opened: ") + strerror(errno));
        else close(fd);
        if (r.isFile && r.size == 0 && !S_ISBLK(st.st_mode)) add(Level::Error, "The file is empty (0 bytes) - the download or copy probably failed.");
        std::string name = util::basename(r.resolved), ln = util::toLower(name);
        for (auto ext : {".crdownload", ".part", ".partial", ".download", ".tmp", ".opdownload"})
            if (util::endsWith(ln, ext)) add(Level::Error, "This is an unfinished download. Let the browser finish, then use the final file.");
        r.shownInImagePicker = img::isImageName(name);
        if (name[0] == '.') add(Level::Info, "Hidden file (name starts with a dot).");
        if (!r.shownInImagePicker) {
            if (util::endsWith(ln, ".zip"))
                add(Level::Info, "Zips appear in Install > Zip. If it contains an .img, use 'Flash .img inside zip' in Files.");
            else
                add(Level::Warn, "Image pickers hide this file because '" + name + "' doesn't end in .img/.img.xz/.img.gz/... "
                                 "Use 'All files' in the picker - or flash it from here.");
        } else if (r.usable()) {
            add(Level::Ok, "Found: " + util::humanSize(r.size) + ". It shows up in the image picker too.");
        }
        if (util::startsWith(r.resolved, device::path("/tmp")))
            add(Level::Info, "/tmp is RAM: the file disappears when the phone reboots.");
        char real[4096];
        if (realpath(r.resolved.c_str(), real) && std::string(real) != r.resolved)
            add(Level::Info, std::string("Symbolic link to ") + pretty(real));
        return r;
    }

    // Missing: find the nearest existing folder and look for look-alikes there.
    std::string dir = util::dirname(r.resolved), name = util::basename(r.resolved);
    std::string probe = dir;
    while (!probe.empty() && probe != "/" && !util::isDir(probe)) probe = util::dirname(probe);
    if (!util::isDir(dir)) {
        add(Level::Error, "Folder " + pretty(dir) + " doesn't exist. Closest existing folder: " + pretty(probe.empty() ? "/" : probe));
        return r;
    }
    add(Level::Error, "No file called '" + name + "' in " + pretty(dir) + ".");
    std::string ln = util::toLower(name), stem = ln;
    for (auto ext : {".img.xz", ".img.gz", ".img.zst", ".img.lz4", ".img", ".xz", ".gz", ".zip", ".zst"})
        if (util::endsWith(stem, ext)) { stem = stem.substr(0, stem.size() - strlen(ext)); break; }
    struct Cand { std::string path; int score; };
    std::vector<Cand> cands;
    bool anyImageByContent = false;
    for (auto& e : util::listDir(dir, true)) {
        if (e.dir) continue;
        std::string en = util::toLower(e.name), full = util::joinPath(dir, e.name);
        std::string estem = en;
        for (auto ext : {".img.xz", ".img.gz", ".img.zst", ".img.lz4", ".img", ".xz", ".gz", ".zip", ".zst", ".crdownload", ".part"})
            if (util::endsWith(estem, ext)) { estem = estem.substr(0, estem.size() - strlen(ext)); break; }
        size_t common = 0;
        while (common < stem.size() && common < estem.size() && stem[common] == estem[common]) common++;
        size_t dist = editDistance(stem, estem.substr(0, std::min(estem.size(), stem.size() + 3)));
        bool partial = (util::endsWith(en, ".crdownload") || util::endsWith(en, ".part") || util::endsWith(en, ".download") ||
                        util::endsWith(en, ".tmp")) && common >= 4;
        int score = 0;
        if (en == ln) { score = 100; add(Level::Warn, "Found '" + e.name + "' - paths are case-sensitive."); }
        else if (partial) { score = 40; add(Level::Warn, "'" + e.name + "' is still downloading or the download failed."); }
        else if (!stem.empty() && util::startsWith(estem, stem)) score = 90;
        else if (stem.size() >= 4 && dist <= std::max<size_t>(2, stem.size() / 5)) score = 80 - (int)dist;
        else if (common >= 5) score = 50 + (int)std::min<size_t>(common, 20);
        bool byName = img::isImageName(e.name) || util::endsWith(en, ".zip");
        if (!byName && !partial && contentIsImage(full)) {
            anyImageByContent = true;
            score = std::max(score, 30);
            if (score >= 50) add(Level::Warn, "'" + e.name + "' is a disk image by its content, but its name has no .img extension - that's why pickers hide it.");
        } else if (score == 0 && byName) {
            score = 10;
        }
        if (score > 0) cands.push_back({full, score});
    }
    std::stable_sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.score > b.score; });
    for (auto& c : cands) r.suggestions.push_back(c.path);
    if (!cands.empty() && cands[0].score < 50) add(Level::Info, "Image files in that folder are listed below" +
                                                   std::string(anyImageByContent ? " (including files detected by content)." : "."));
    return r;
}

}  // namespace tw::pathcheck
