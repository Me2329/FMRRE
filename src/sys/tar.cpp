#include "sys/tar.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <map>
#include <set>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/xattr.h>
#include <unistd.h>

#include "core/util.h"

namespace tw::tar {

namespace {

constexpr size_t kBlock = 512;
constexpr const char* kSelinux = "security.selinux";

void note(Stats& st, const std::string& s) {
    if (st.notes.size() < 8) st.notes.push_back(s);
}

void octal(char* f, size_t width, uint64_t v) {  // width - 1 digits, then NUL
    size_t digits = width - 1;
    for (size_t i = digits; i-- > 0;) {
        f[i] = char('0' + (v & 7));
        v >>= 3;
    }
    f[digits] = 0;
}
bool fits(uint64_t v, size_t digits) { return digits >= 21 || v < (uint64_t(1) << (3 * digits)); }

std::string paxRecord(const std::string& key, const std::string& value) {
    size_t base = key.size() + value.size() + 3, len = base + 1;  // "LEN key=value\n", LEN counts itself
    for (;;) {
        size_t next = base + std::to_string(len).size();
        if (next == len) break;
        len = next;
    }
    return std::to_string(len) + " " + key + "=" + value + "\n";
}

void fillHeader(char* h, const std::string& name, const std::string& link, char type, uint64_t size, uint32_t mode,
                uint64_t uid, uint64_t gid, int64_t mtime, uint32_t maj, uint32_t min) {
    memset(h, 0, kBlock);
    memcpy(h, name.data(), std::min<size_t>(name.size(), 100));
    octal(h + 100, 8, mode & 07777);
    octal(h + 108, 8, fits(uid, 7) ? uid : 0);
    octal(h + 116, 8, fits(gid, 7) ? gid : 0);
    octal(h + 124, 12, fits(size, 11) ? size : 0);
    octal(h + 136, 12, mtime > 0 && fits(uint64_t(mtime), 11) ? uint64_t(mtime) : 0);
    h[156] = type;
    memcpy(h + 157, link.data(), std::min<size_t>(link.size(), 100));
    memcpy(h + 257, "ustar", 6);
    memcpy(h + 263, "00", 2);
    octal(h + 329, 8, maj);
    octal(h + 337, 8, min);
    memset(h + 148, ' ', 8);
    unsigned sum = 0;
    for (size_t i = 0; i < kBlock; i++) sum += (unsigned char)h[i];
    octal(h + 148, 7, sum);
    h[155] = ' ';
}

std::vector<std::pair<std::string, std::string>> readXattrs(const std::string& path, Stats& st) {
    std::vector<std::pair<std::string, std::string>> out;
    ssize_t n = llistxattr(path.c_str(), nullptr, 0);
    if (n <= 0) return out;
    std::string names(size_t(n), '\0');
    n = llistxattr(path.c_str(), &names[0], names.size());
    if (n <= 0) return out;
    for (size_t i = 0; i < size_t(n);) {
        std::string key(&names[i]);
        i += key.size() + 1;
        if (key.empty() || util::startsWith(key, "system.")) continue;  // ACLs etc. are kept by mode bits here
        ssize_t vn = lgetxattr(path.c_str(), key.c_str(), nullptr, 0);
        std::string v(vn > 0 ? size_t(vn) : 0, '\0');
        if (vn < 0 || (vn > 0 && lgetxattr(path.c_str(), key.c_str(), &v[0], v.size()) != vn)) {
            st.xattrFailed++;
            continue;
        }
        out.push_back({key, v});
    }
    return out;
}

// ---- writing ---------------------------------------------------------------------------------------

struct Writer {
    const Sink& out;
    Stats& st;
    std::string& err;
    const std::function<bool()>& cancelled;
    std::string root;
    std::set<std::string> exclude;
    dev_t rootDev = 0;
    std::map<std::pair<dev_t, ino_t>, std::string> seen;  // hard links
    std::vector<char> buf = std::vector<char>(1 << 20);

    bool put(const void* p, size_t n) {
        if (!out(p, n)) return err = "writing the archive failed", false;
        return true;
    }
    bool pad(uint64_t size) {
        static const char zeros[kBlock] = {0};
        size_t r = size_t(size % kBlock);
        return !r || put(zeros, kBlock - r);
    }

    bool entry(const std::string& arc, const std::string& path, const struct stat& s) {
        char type;
        uint64_t size = 0;
        std::string link;
        int fd = -1;
        if (S_ISREG(s.st_mode)) {
            if (s.st_nlink > 1) {
                auto key = std::make_pair(s.st_dev, s.st_ino);
                auto it = seen.find(key);
                if (it != seen.end()) link = it->second;
                else seen[key] = arc;
            }
            if (!link.empty()) {
                type = '1';
            } else {
                type = '0';
                fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
                struct stat fs;
                if (fd < 0 || fstat(fd, &fs) != 0) {
                    if (fd >= 0) close(fd);
                    st.skipped++;
                    note(st, "can't read " + arc + ": " + strerror(errno));
                    return true;
                }
                size = uint64_t(fs.st_size);
            }
        } else if (S_ISDIR(s.st_mode)) {
            type = '5';
        } else if (S_ISLNK(s.st_mode)) {
            type = '2';
            std::vector<char> t(PATH_MAX + 1);
            ssize_t n = readlink(path.c_str(), t.data(), PATH_MAX);
            if (n < 0) {
                st.skipped++;
                return true;
            }
            link.assign(t.data(), size_t(n));
        } else if (S_ISCHR(s.st_mode)) {
            type = '3';
        } else if (S_ISBLK(s.st_mode)) {
            type = '4';
        } else if (S_ISFIFO(s.st_mode)) {
            type = '6';
        } else {  // sockets: tar can't hold them (they are made again by whoever listens)
            st.skipped++;
            return true;
        }
        std::string pax;
        for (auto& kv : readXattrs(path, st)) {
            if (kv.first == kSelinux) {
                std::string ctx = kv.second;
                while (!ctx.empty() && ctx.back() == '\0') ctx.pop_back();
                pax += paxRecord("RHT.security.selinux", ctx);              // toybox / TWRP / GNU tar --selinux
                pax += paxRecord("SCHILY.xattr.security.selinux", kv.second);  // GNU tar --xattrs
                st.labels++;
            } else {
                pax += paxRecord("SCHILY.xattr." + kv.first, kv.second);
            }
        }
        if (arc.size() > 100) pax += paxRecord("path", arc);
        if (link.size() > 100) pax += paxRecord("linkpath", link);
        if (!fits(size, 11)) pax += paxRecord("size", std::to_string(size));
        if (!fits(s.st_uid, 7)) pax += paxRecord("uid", std::to_string(s.st_uid));
        if (!fits(s.st_gid, 7)) pax += paxRecord("gid", std::to_string(s.st_gid));
        char h[kBlock];
        if (!pax.empty()) {
            std::string base = util::basename(arc.back() == '/' ? arc.substr(0, arc.size() - 1) : arc);
            fillHeader(h, ("./PaxHeaders/" + base).substr(0, 100), "", 'x', pax.size(), 0644, 0, 0, s.st_mtime, 0, 0);
            if (!put(h, kBlock) || !put(pax.data(), pax.size()) || !pad(pax.size())) return fd >= 0 && close(fd), false;
        }
        fillHeader(h, arc, link, type, size, s.st_mode, s.st_uid, s.st_gid, s.st_mtime, major(s.st_rdev), minor(s.st_rdev));
        if (!put(h, kBlock)) return fd >= 0 && close(fd), false;
        if (type == '0') {
            uint64_t done = 0;
            while (done < size) {
                ssize_t r = read(fd, buf.data(), size_t(std::min<uint64_t>(buf.size(), size - done)));
                if (r < 0 && errno == EINTR) continue;
                if (r <= 0) {  // it shrank while we read it: keep the archive whole
                    note(st, arc + " changed while it was archived");
                    std::fill(buf.begin(), buf.end(), 0);
                    while (done < size) {
                        size_t k = size_t(std::min<uint64_t>(buf.size(), size - done));
                        if (!put(buf.data(), k)) return close(fd), false;
                        done += k;
                    }
                    break;
                }
                if (!put(buf.data(), size_t(r))) return close(fd), false;
                done += uint64_t(r);
            }
            close(fd);
            st.bytes += size;
            if (!pad(size)) return false;
        }
        if (type == '0') st.files++;
        else if (type == '5') st.dirs++;
        else if (type == '1' || type == '2') st.links++;
        else st.other++;
        return true;
    }

    bool walk(const std::string& rel) {
        if (cancelled && cancelled()) return err = "cancelled", false;
        std::string path = rel.empty() ? root : root + "/" + rel;
        struct stat s;
        if (lstat(path.c_str(), &s) != 0) {
            if (rel.empty()) return err = root + ": " + strerror(errno), false;
            st.skipped++;  // vanished meanwhile
            return true;
        }
        if (rel.empty()) rootDev = s.st_dev;
        bool dir = S_ISDIR(s.st_mode);
        if (!entry(rel.empty() ? "./" : "./" + rel + (dir ? "/" : ""), path, s)) return false;
        if (!dir) return true;
        if (!rel.empty() && s.st_dev != rootDev) {  // another file system mounted here: not part of this partition
            note(st, "./" + rel + " is another file system - not archived");
            return true;
        }
        std::vector<std::string> names;
        if (DIR* d = opendir(path.c_str())) {
            while (dirent* e = readdir(d))
                if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) names.push_back(e->d_name);
            closedir(d);
        }
        std::sort(names.begin(), names.end());
        for (auto& n : names) {
            std::string r = rel.empty() ? n : rel + "/" + n;
            if (exclude.count(r)) continue;
            if (!walk(r)) return false;
        }
        return true;
    }
};

uint64_t parseNum(const char* f, size_t n) {
    if ((unsigned char)f[0] & 0x80) {  // GNU base-256
        uint64_t v = (unsigned char)f[0] & 0x7F;
        for (size_t i = 1; i < n; i++) v = (v << 8) | (unsigned char)f[i];
        return v;
    }
    uint64_t v = 0;
    size_t i = 0;
    while (i < n && (f[i] == ' ' || f[i] == 0)) i++;
    for (; i < n && f[i] >= '0' && f[i] <= '7'; i++) v = (v << 3) | uint64_t(f[i] - '0');
    return v;
}

std::string field(const char* f, size_t n) { return std::string(f, strnlen(f, n)); }

}  // namespace

bool create(const std::string& root, const std::vector<std::string>& exclude, const Sink& out, Stats& st, std::string& err,
            const std::function<bool()>& cancelled) {
    Writer w{out, st, err, cancelled, root, std::set<std::string>(exclude.begin(), exclude.end())};
    if (!w.walk("")) return false;
    static const char zeros[2 * kBlock] = {0};
    return w.put(zeros, sizeof zeros);  // end of archive
}

// ---- extraction ------------------------------------------------------------------------------------

struct Meta {
    uint32_t mode = 0;
    uint64_t uid = 0, gid = 0;
    int64_t mtime = 0;
    std::vector<std::pair<std::string, std::string>> xattrs;
};

struct Extractor::Impl {
    std::string root;
    std::vector<std::string> absPrefixes;
    enum State { Header, Data, Pax, GlobalPax, LongName, LongLink, Skip, Pad, End } state = Header;
    char hdr[kBlock];
    size_t hdrFill = 0;
    uint64_t remaining = 0, padLeft = 0;
    std::string text, longName, longLink;
    std::map<std::string, std::string> pax, globalPax;
    int fd = -1;
    std::string curPath;
    Meta cur;
    std::vector<std::pair<std::string, Meta>> dirs;  // their own mode / times last: files go in first
    int zeros = 0;

    // The name relative to root ("" = root itself); false: must not be extracted.
    bool sanitize(std::string name, std::string& rel) {
        if (!name.empty() && name[0] == '/') {
            bool inside = false;
            for (auto& p : absPrefixes)
                if (name == p || util::startsWith(name, p + "/")) {
                    name = name.substr(p.size());
                    inside = true;
                    break;
                }
            if (!inside) return false;
        }
        rel.clear();
        for (auto& c : util::split(name, '/')) {
            if (c.empty() || c == ".") continue;
            if (c == "..") return false;
            rel += (rel.empty() ? "" : "/") + c;
        }
        return true;
    }
    // Every folder on the way must be a real folder (made if missing), never a symlink - nothing is written
    // through a link the archive itself put there.
    bool parentsOk(const std::string& rel) {
        std::string p = root;
        auto comps = util::split(rel, '/');
        for (size_t i = 0; i + 1 < comps.size(); i++) {
            p += "/" + comps[i];
            struct stat s;
            if (lstat(p.c_str(), &s) != 0) {
                if (mkdir(p.c_str(), 0755) != 0) return false;
            } else if (!S_ISDIR(s.st_mode)) {
                return false;
            }
        }
        return true;
    }
    void apply(const std::string& path, const Meta& m, bool link, Stats& st) {
        if (lchown(path.c_str(), uid_t(m.uid), gid_t(m.gid)) != 0) {}
        if (!link && chmod(path.c_str(), m.mode & 07777) != 0) {}
        for (auto& kv : m.xattrs) {
            std::string v = kv.second;
            if (kv.first == kSelinux && (v.empty() || v.back() != '\0')) v.push_back('\0');
            if (lsetxattr(path.c_str(), kv.first.c_str(), v.data(), v.size(), 0) == 0) {
                if (kv.first == kSelinux) st.labels++;
            } else {
                st.xattrFailed++;
                note(st, "couldn't set " + kv.first + " on " + path.substr(root.size()) + ": " + strerror(errno));
            }
        }
        timespec ts[2] = {{m.mtime, 0}, {m.mtime, 0}};
        utimensat(AT_FDCWD, path.c_str(), ts, AT_SYMLINK_NOFOLLOW);
    }
};

Extractor::Extractor(std::string root, std::vector<std::string> absPrefixes) : d_(new Impl) {
    d_->root = std::move(root);
    for (auto& p : absPrefixes) {
        while (p.size() > 1 && p.back() == '/') p.pop_back();
        if (!p.empty() && p[0] == '/') d_->absPrefixes.push_back(p);
    }
}

Extractor::~Extractor() {
    if (d_->fd >= 0) close(d_->fd);
    delete d_;
}

bool Extractor::feed(const void* data, size_t n) {
    Impl& d = *d_;
    const char* p = (const char*)data;
    auto fail = [&](const std::string& e) {
        err_ = e;
        return false;
    };
    while (n) {
        switch (d.state) {
            case Impl::End: return true;  // anything after the end-of-archive blocks is ignored
            case Impl::Header: {
                size_t k = std::min(n, kBlock - d.hdrFill);
                memcpy(d.hdr + d.hdrFill, p, k);
                d.hdrFill += k;
                p += k;
                n -= k;
                if (d.hdrFill < kBlock) break;
                d.hdrFill = 0;
                const char* h = d.hdr;
                bool zero = true;
                for (size_t i = 0; i < kBlock && zero; i++) zero = h[i] == 0;
                if (zero) {
                    if (++d.zeros >= 2) d.state = Impl::End;
                    break;
                }
                d.zeros = 0;
                unsigned sum = 0;
                int ssum = 0;
                for (size_t i = 0; i < kBlock; i++) {
                    unsigned char c = (i >= 148 && i < 156) ? ' ' : (unsigned char)h[i];
                    sum += c;
                    ssum += (signed char)c;
                }
                uint64_t stored = parseNum(h + 148, 8);
                if (stored != sum && stored != uint64_t(unsigned(ssum))) return fail("damaged archive (header checksum)");
                char type = h[156];
                uint64_t size = parseNum(h + 124, 12);
                std::string name = field(h, 100), link = field(h + 157, 100);
                if (!memcmp(h + 257, "ustar", 5)) {
                    std::string prefix = field(h + 345, 155);
                    if (!prefix.empty()) name = prefix + "/" + name;
                }
                if (type == 'x' || type == 'g' || type == 'L' || type == 'K') {
                    if (size > (16u << 20)) return fail("damaged archive (extended header too big)");
                    d.text.clear();
                    d.remaining = size;
                    d.padLeft = (kBlock - size % kBlock) % kBlock;
                    d.state = type == 'x' ? Impl::Pax : type == 'g' ? Impl::GlobalPax : type == 'L' ? Impl::LongName : Impl::LongLink;
                    if (!size) d.state = d.padLeft ? Impl::Pad : Impl::Header;  // (an empty one: nothing to apply)
                    break;
                }
                Meta m;
                m.mode = uint32_t(parseNum(h + 100, 8));
                m.uid = parseNum(h + 108, 8);
                m.gid = parseNum(h + 116, 8);
                m.mtime = int64_t(parseNum(h + 136, 12));
                std::map<std::string, std::string> px = d.globalPax;
                for (auto& kv : d.pax) px[kv.first] = kv.second;
                if (!d.longName.empty()) name = d.longName;
                if (!d.longLink.empty()) link = d.longLink;
                for (auto& kv : px) {
                    if (kv.first == "path") name = kv.second;
                    else if (kv.first == "linkpath") link = kv.second;
                    else if (kv.first == "size") size = strtoull(kv.second.c_str(), nullptr, 10);
                    else if (kv.first == "uid") m.uid = strtoull(kv.second.c_str(), nullptr, 10);
                    else if (kv.first == "gid") m.gid = strtoull(kv.second.c_str(), nullptr, 10);
                    else if (kv.first == "mtime") m.mtime = strtoll(kv.second.c_str(), nullptr, 10);
                    else if (kv.first == "RHT.security.selinux") m.xattrs.push_back({kSelinux, kv.second});
                    else if (util::startsWith(kv.first, "SCHILY.xattr.")) {
                        std::string key = kv.first.substr(13);
                        bool dup = false;
                        for (auto& x : m.xattrs) dup |= x.first == key;
                        if (!dup) m.xattrs.push_back({key, kv.second});
                    }
                }
                d.pax.clear();
                d.longName.clear();
                d.longLink.clear();
                bool hasData = type == '0' || type == '\0' || type == '7';
                d.remaining = hasData || (type != '1' && type != '2' && type != '3' && type != '4' && type != '5' && type != '6') ? size : 0;
                d.padLeft = (kBlock - d.remaining % kBlock) % kBlock;
                std::string rel;
                bool ok = d.sanitize(name, rel) && (rel.empty() || d.parentsOk(rel));
                std::string path = rel.empty() ? d.root : d.root + "/" + rel;
                if (!ok || (rel.empty() && type != '5')) {
                    st_.refused++;
                    note(st_, "not extracted (outside the folder): " + name);
                    d.state = d.remaining ? Impl::Skip : (d.padLeft ? Impl::Pad : Impl::Header);
                    break;
                }
                struct stat old;
                if (type != '5' && !rel.empty() && lstat(path.c_str(), &old) == 0) {  // replace what is there
                    if (S_ISDIR(old.st_mode) ? rmdir(path.c_str()) != 0 : unlink(path.c_str()) != 0) {
                        st_.refused++;
                        note(st_, "can't replace " + rel + ": " + strerror(errno));
                        d.state = d.remaining ? Impl::Skip : (d.padLeft ? Impl::Pad : Impl::Header);
                        break;
                    }
                }
                if (hasData) {
                    d.fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
                    if (d.fd < 0) return fail("can't create " + rel + ": " + strerror(errno));
                    d.curPath = path;
                    d.cur = m;
                    st_.files++;
                    st_.bytes += size;
                    if (!size) {
                        close(d.fd);
                        d.fd = -1;
                        d.apply(path, m, false, st_);
                    }
                    d.state = size ? Impl::Data : Impl::Header;
                    break;
                }
                switch (type) {
                    case '5':
                        if (!rel.empty() && mkdir(path.c_str(), 0700) != 0 && errno != EEXIST) return fail("can't create " + rel + ": " + strerror(errno));
                        d.dirs.push_back({path, m});
                        st_.dirs++;
                        break;
                    case '2':
                        if (symlink(link.c_str(), path.c_str()) != 0) return fail("can't create the link " + rel + ": " + strerror(errno));
                        d.apply(path, m, true, st_);
                        st_.links++;
                        break;
                    case '1': {
                        std::string target;
                        if (!d.sanitize(link, target) || target.empty() || ::link((d.root + "/" + target).c_str(), path.c_str()) != 0) {
                            st_.refused++;
                            note(st_, "hard link " + rel + " -> " + link + " not made");
                        } else {
                            st_.links++;
                        }
                        break;
                    }
                    case '3':
                    case '4':
                    case '6': {
                        mode_t kind = type == '3' ? S_IFCHR : type == '4' ? S_IFBLK : S_IFIFO;
                        dev_t dev = makedev(unsigned(parseNum(h + 329, 8)), unsigned(parseNum(h + 337, 8)));
                        if (mknod(path.c_str(), kind | 0600, type == '6' ? 0 : dev) != 0) {
                            st_.refused++;
                            note(st_, "can't create " + rel + ": " + strerror(errno));
                        } else {
                            d.apply(path, m, false, st_);
                            st_.other++;
                        }
                        break;
                    }
                    default:  // GNU sparse files, volume headers ...: not something a partition backup has
                        st_.skipped++;
                        note(st_, std::string("skipped ") + rel + " (entry type '" + type + "')");
                }
                d.state = d.remaining ? Impl::Skip : (d.padLeft ? Impl::Pad : Impl::Header);
                break;
            }
            case Impl::Data: {
                size_t k = size_t(std::min<uint64_t>(n, d.remaining));
                for (size_t done = 0; done < k;) {
                    ssize_t w = write(d.fd, p + done, k - done);
                    if (w < 0 && errno == EINTR) continue;
                    if (w <= 0) return fail("writing " + d.curPath.substr(d.root.size()) + " failed: " + strerror(errno));
                    done += size_t(w);
                }
                p += k;
                n -= k;
                d.remaining -= k;
                if (!d.remaining) {
                    close(d.fd);
                    d.fd = -1;
                    d.apply(d.curPath, d.cur, false, st_);
                    d.state = d.padLeft ? Impl::Pad : Impl::Header;
                }
                break;
            }
            case Impl::Pax:
            case Impl::GlobalPax:
            case Impl::LongName:
            case Impl::LongLink: {
                size_t k = size_t(std::min<uint64_t>(n, d.remaining));
                d.text.append(p, k);
                p += k;
                n -= k;
                d.remaining -= k;
                if (d.remaining) break;
                if (d.state == Impl::LongName || d.state == Impl::LongLink) {
                    std::string s = d.text.substr(0, strnlen(d.text.data(), d.text.size()));
                    (d.state == Impl::LongName ? d.longName : d.longLink) = s;
                } else {
                    auto& into = d.state == Impl::Pax ? d.pax : d.globalPax;
                    for (size_t i = 0; i < d.text.size();) {  // "LEN key=value\n"
                        size_t sp = d.text.find(' ', i);
                        if (sp == std::string::npos) return fail("damaged archive (extended header)");
                        size_t len = strtoull(d.text.substr(i, sp - i).c_str(), nullptr, 10);
                        if (len <= sp - i + 1 || i + len > d.text.size()) return fail("damaged archive (extended header)");
                        std::string rec = d.text.substr(sp + 1, len - (sp - i) - 2);  // without the newline
                        size_t eq = rec.find('=');
                        if (eq != std::string::npos) into[rec.substr(0, eq)] = rec.substr(eq + 1);
                        i += len;
                    }
                }
                d.state = d.padLeft ? Impl::Pad : Impl::Header;
                break;
            }
            case Impl::Skip: {
                size_t k = size_t(std::min<uint64_t>(n, d.remaining));
                p += k;
                n -= k;
                d.remaining -= k;
                if (!d.remaining) d.state = d.padLeft ? Impl::Pad : Impl::Header;
                break;
            }
            case Impl::Pad: {
                size_t k = size_t(std::min<uint64_t>(n, d.padLeft));
                p += k;
                n -= k;
                d.padLeft -= k;
                if (!d.padLeft) d.state = Impl::Header;
                break;
            }
        }
    }
    return true;
}

bool Extractor::nextArchive() {
    Impl& d = *d_;
    if (d.state != Impl::End && (d.state != Impl::Header || d.hdrFill)) {
        err_ = "the archive is cut off";
        return false;
    }
    d.state = Impl::Header;
    d.zeros = 0;
    return true;
}

bool Extractor::finish() {
    Impl& d = *d_;
    if (d.state != Impl::End && (d.state != Impl::Header || d.hdrFill)) {
        err_ = "the archive is cut off";
        return false;
    }
    for (auto it = d.dirs.rbegin(); it != d.dirs.rend(); ++it) d.apply(it->first, it->second, false, st_);
    d.dirs.clear();
    return true;
}

}  // namespace tw::tar
