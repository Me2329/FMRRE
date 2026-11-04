#include "sys/zipsig.h"

#include <condition_variable>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <sys/mount.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

#include "core/sha1.h"
#include "core/sha256.h"
#include "core/util.h"
#include "platform/device.h"
#include "sys/partitions.h"

namespace tw::zipsig {

namespace {

constexpr size_t kFooter = 6, kEocd = 22;

Result finish(Result r, State s, const std::string& why) {
    r.state = s;
    r.reason = why;
    return r;
}

bool hasEocdMagic(const uint8_t* p) { return p[0] == 0x50 && p[1] == 0x4B && p[2] == 0x05 && p[3] == 0x06; }

}  // namespace

// ---- texts -------------------------------------------------------------------------------------------

std::string Result::signer() const {
    if (testKey) return "AOSP test key";
    std::string n = cert.name();
    return n.empty() ? "unnamed key" : n;
}

std::string Result::headline() const {
    switch (state) {
        case State::Unsigned: return "Not signed";
        case State::Bad: return "The signature doesn't match the file";
        case State::Error: return "The signature couldn't be checked";
        case State::Valid: break;
    }
    switch (trust) {
        case Trust::System: return "Signed with the installed system's key";
        case Trust::Recovery: return "Signed with a key this recovery trusts";
        case Trust::User: return "Signed with a key you trusted";
        case Trust::Unknown: break;
    }
    return testKey ? "Signed with the public AOSP test key" : "Signed with a key FMRRE doesn't know";
}

std::string Result::detail() const {
    if (state != State::Valid) return reason;
    std::string d = signer() + "  \xE2\x80\xA2  " + cert.keyDesc() + ", " + hash;
    std::string id = cert.keyId();
    if (!id.empty()) d += "  \xE2\x80\xA2  key " + id.substr(0, 16);
    return d;
}

std::string Result::brief() const {
    switch (state) {
        case State::Unsigned: return "unsigned";
        case State::Bad: return "BAD signature (" + reason + ")";
        case State::Error: return "signature not checked (" + reason + ")";
        case State::Valid: break;
    }
    std::string whose = trust == Trust::System     ? "the installed system's key"
                        : trust == Trust::Recovery ? "a key this recovery trusts"
                        : trust == Trust::User     ? "a key you trusted"
                        : testKey                  ? "the public test key"
                        : systemKeysKnown          ? "another key than the installed system's"
                                                   : "an unknown key";
    return "signed: " + signer() + " (" + whose + ")";
}

std::string Result::badge() const {
    switch (state) {
        case State::Unsigned: return "Unsigned";
        case State::Bad: return "BAD";
        case State::Error: return "?";
        case State::Valid: break;
    }
    if (trust == Trust::System) return "\xE2\x9C\x93 your ROM";
    if (trust != Trust::Unknown) return "\xE2\x9C\x93 trusted";
    return testKey ? "Test key" : "Other key";
}

// ---- keys --------------------------------------------------------------------------------------------

Sources Sources::current() {
    Sources s;
    auto& P = Partitions::get();
    Partition* sys = P.find("/system_root");
    if (!sys || !sys->present) sys = P.find("/system");
    bool systemOnSystem = false;
    if (sys && sys->present) {
        bool mounted = P.isMounted(*sys);
        if (device::isSim() || mounted) s.systemRoot = sys->contentPath();
        else s.systemDev = sys->blockDev, s.systemFs = sys->fsType;
        systemOnSystem = mounted && sys->mountPoint == "/system";
    }
    // The ramdisk's own /system/etc/security/otacerts.zip - unless the system partition is mounted over it.
    if (!systemOnSystem) s.recoveryZip = device::path("/system/etc/security/otacerts.zip");
    s.recoveryResKeys = device::path("/res/keys");
    s.userDir = P.appDir() + "/keys";
    return s;
}

Keys loadKeys(const Sources& s) {
    Keys k;
    auto fromRoot = [&](const std::string& root) {
        for (auto rel : {"/system/etc/security/otacerts.zip", "/etc/security/otacerts.zip"}) {  // system-as-root, older
            std::string p = root + rel;
            if (!util::exists(p)) continue;
            std::string e;
            k.system = x509::loadZip(p, &e);
            k.systemFrom = "system: " + std::string(rel);
            if (k.system.empty()) k.systemNote = "the installed system's otacerts.zip has no usable key (" + e + ")";
            return;
        }
        k.systemNote = "the installed system has no otacerts.zip";
    };
    if (!s.systemRoot.empty()) {
        fromRoot(s.systemRoot);
    } else if (!s.systemDev.empty() && util::exists(s.systemDev)) {
        // Read-only, without replaying a journal (ext4 noload, f2fs norecovery): nothing is written to system.
        const std::string mp = "/tmp/fmrre/sysro";
        util::mkdirs(mp);
        std::vector<std::string> types;
        if (!s.systemFs.empty() && s.systemFs != "auto") types.push_back(s.systemFs);
        for (auto t : {"erofs", "ext4", "f2fs"})
            if (s.systemFs != t) types.push_back(t);
        bool mounted = false;
        for (auto& t : types) {
            const char* opts = t == "ext4" ? "noload" : t == "f2fs" ? "norecovery" : nullptr;
            if (::mount(s.systemDev.c_str(), mp.c_str(), t.c_str(), MS_RDONLY, opts) == 0 ||
                (opts && ::mount(s.systemDev.c_str(), mp.c_str(), t.c_str(), MS_RDONLY, nullptr) == 0)) {
                mounted = true;
                break;
            }
        }
        if (mounted) {
            fromRoot(mp);
            umount2(mp.c_str(), MNT_DETACH);
        } else {
            k.systemNote = "the system partition couldn't be mounted";
        }
    } else {
        k.systemNote = "there is no system partition";
    }
    if (!s.recoveryZip.empty() && util::exists(s.recoveryZip)) {
        k.recovery = x509::loadZip(s.recoveryZip);
        if (!k.recovery.empty()) k.recoveryFrom = "recovery: /system/etc/security/otacerts.zip";
    }
    std::string rk;
    if (!s.recoveryResKeys.empty() && util::readFile(s.recoveryResKeys, rk, 1 << 20)) {
        auto keys = x509::loadMincrypt(rk);
        if (!keys.empty()) {
            k.recoveryFrom += k.recoveryFrom.empty() ? "recovery: /res/keys" : " + /res/keys";
            for (auto& c : keys) k.recovery.push_back(std::move(c));
        }
    }
    if (!s.userDir.empty() && util::isDir(s.userDir)) {
        for (auto& e : util::listDir(s.userDir)) {
            if (e.dir) continue;
            std::string p = util::joinPath(s.userDir, e.name), low = util::toLower(e.name);
            std::vector<x509::Cert> got;
            if (util::endsWith(low, ".zip")) {
                got = x509::loadZip(p);
            } else if (util::endsWith(low, ".pem") || util::endsWith(low, ".crt") || util::endsWith(low, ".cer") ||
                       util::endsWith(low, ".der")) {
                std::string b;
                if (util::readFile(p, b, 1 << 20)) got = x509::load(b);
            }
            for (auto& c : got) k.user.push_back(std::move(c));
        }
        if (!k.user.empty()) k.userFrom = "FMRRE/keys";
    }
    return k;
}

// ---- verification ------------------------------------------------------------------------------------

Result verify(const std::string& zip, const Keys& keys, const Progress& progress) {
    Result r;
    r.systemKeysKnown = !keys.system.empty();
    r.systemNote = keys.systemNote;
    int fd = open(zip.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return finish(r, State::Error, "the file can't be opened");
    struct Closer {
        int fd;
        ~Closer() { close(fd); }
    } closer{fd};
    struct stat st;
    if (fstat(fd, &st) != 0) return finish(r, State::Error, "the file can't be read");
    const uint64_t length = uint64_t(st.st_size);
    auto readAt = [&](void* buf, size_t n, uint64_t off) {
        size_t got = 0;
        while (got < n) {
            ssize_t k = pread(fd, (uint8_t*)buf + got, n - got, off_t(off + got));
            if (k <= 0) return false;
            got += size_t(k);
        }
        return true;
    };

    // An archive with a whole-file signature ends in (signature start) FF FF (comment size), all 16-bit LE;
    // the signature block is the end of the zip comment.
    uint8_t footer[kFooter];
    if (length < kFooter + kEocd || !readAt(footer, kFooter, length - kFooter) || footer[2] != 0xFF || footer[3] != 0xFF)
        return finish(r, State::Unsigned, "there is no signature at the end of the zip");
    size_t commentSize = footer[4] | size_t(footer[5]) << 8, sigStart = footer[0] | size_t(footer[1]) << 8;
    if (sigStart > commentSize || sigStart <= kFooter) return finish(r, State::Bad, "the signature footer is damaged");
    size_t eocdSize = commentSize + kEocd;
    if (length < eocdSize) return finish(r, State::Bad, "the file is shorter than its signature says");
    std::vector<uint8_t> eocd(eocdSize);
    if (!readAt(eocd.data(), eocdSize, length - eocdSize)) return finish(r, State::Error, "the file can't be read");
    if (!hasEocdMagic(eocd.data()) || (eocd[20] | size_t(eocd[21]) << 8) != commentSize)
        return finish(r, State::Bad, "the end of the zip doesn't match its signature");
    // A second end-of-zip record after the real one would be the one zip readers find: refused, like Android does.
    for (size_t i = 4; i + 3 < eocdSize; i++)
        if (hasEocdMagic(eocd.data() + i))
            return finish(r, State::Bad, "something was added after the signed end of the zip");
    // Signed: everything but the comment and its 2-byte length.
    const uint64_t signedLen = length - commentSize - 2;
    x509::SignedData sd = x509::parseSignedData(eocd.data() + eocdSize - sigStart, sigStart - kFooter);
    if (!sd.ok) return finish(r, State::Bad, "the signature block can't be read: " + sd.error);
    bool sha1 = sd.digestAlg == x509::kSha1;
    if (!sha1 && sd.digestAlg != x509::kSha256) return finish(r, State::Bad, "it is signed with an unsupported hash");

    Sha1 h1;
    Sha256 h2;
    std::vector<uint8_t> buf(1 << 20);
    for (uint64_t done = 0; done < signedLen;) {
        size_t n = size_t(std::min<uint64_t>(buf.size(), signedLen - done));
        if (!readAt(buf.data(), n, done)) return finish(r, State::Error, "the file can't be read");
        if (sha1) h1.update(buf.data(), n);
        else h2.update(buf.data(), n);
        done += n;
        if (progress && !progress(done, signedLen)) return finish(r, State::Error, "cancelled");
    }
    const std::string digest = sha1 ? h1.digest() : h2.digest();
    const pk::Hash ph = sha1 ? pk::Hash::Sha1 : pk::Hash::Sha256;
    auto verifyWith = [&](const x509::Cert& c) {
        if (c.key == x509::Cert::Key::Rsa) return pk::rsaVerify(c.rsa, ph, digest, sd.signature.data(), sd.signature.size());
        if (c.key == x509::Cert::Key::Ec) return !sha1 && pk::ecdsaVerify(c.ec, digest, sd.signature.data(), sd.signature.size());
        return false;
    };
    bool ok = false;
    if (const x509::Cert* c = sd.signer()) {
        if (verifyWith(*c)) ok = true, r.cert = *c;
    }
    if (!ok) {
        // Android's recovery doesn't look at the certificates in the block: it tries its own keys. So does FMRRE.
        for (auto* list : {&keys.system, &keys.recovery, &keys.user}) {
            for (auto& c : *list)
                if (c.hashLen == int(digest.size()) && verifyWith(c)) {
                    ok = true;
                    r.cert = c;
                    break;
                }
            if (ok) break;
        }
    }
    if (!ok) return finish(r, State::Bad, "it was changed after it was signed, or it is damaged");
    r.state = State::Valid;
    r.hash = sha1 ? "SHA-1" : "SHA-256";
    r.testKey = r.cert.isAospTestKey();
    auto inList = [&](const std::vector<x509::Cert>& l) {
        for (auto& c : l)
            if (c.sameKey(r.cert)) return true;
        return false;
    };
    if (inList(keys.system)) r.trust = Trust::System, r.trustedBy = keys.systemFrom;
    else if (inList(keys.recovery)) r.trust = Trust::Recovery, r.trustedBy = keys.recoveryFrom;
    else if (inList(keys.user)) r.trust = Trust::User, r.trustedBy = keys.userFrom;
    return r;
}

std::vector<std::string> report(const Result& r) {
    std::vector<std::string> v{"Signature: " + r.headline()};
    if (r.valid()) {
        v.push_back("  signer: " + (r.cert.subject().empty() ? r.signer() : r.cert.subject()));
        v.push_back("  key: " + r.cert.keyDesc() + ", " + r.hash + ", id " + r.cert.keyId().substr(0, 16));
        if (r.trusted()) v.push_back("  trusted by " + r.trustedBy);
        if (r.testKey) v.push_back("  The AOSP test key is public: anyone can sign with it (unofficial builds use it).");
        if (r.otherThanSystem())
            v.push_back("  The installed system takes updates signed with another key: this is another ROM, or another "
                        "build of it signed by someone else.");
    } else {
        v.push_back("  " + r.reason);
    }
    if (!r.systemKeysKnown && !r.systemNote.empty()) v.push_back("  (not compared with the installed system: " + r.systemNote + ")");
    return v;
}

// ---- background checks -------------------------------------------------------------------------------

namespace {

struct Entry {
    uint64_t size = 0;
    int64_t mtime = 0;
    uint64_t gen = 0;
    Check c;
};

struct Bg {
    std::mutex m;
    std::map<std::string, Entry> entries;
    std::deque<std::pair<std::string, uint64_t>> queue;  // path, generation
    Sources sources;
    uint64_t gen = 0;
    bool running = false;
};

// Never destroyed: the worker is detached and may still be busy when the program exits.
Bg& bg() {
    static Bg* b = new Bg;
    return *b;
}

void worker() {
    Bg& b = bg();
    for (;;) {
        std::string path;
        uint64_t gen;
        Sources src;
        {
            std::lock_guard<std::mutex> l(b.m);
            if (b.queue.empty()) {
                b.running = false;
                return;
            }
            path = b.queue.front().first;
            gen = b.queue.front().second;
            b.queue.pop_front();
            src = b.sources;
        }
        Keys keys = loadKeys(src);
        Result res = verify(path, keys, [&](uint64_t d, uint64_t t) {
            std::lock_guard<std::mutex> l(b.m);
            auto it = b.entries.find(path);
            if (it == b.entries.end() || it->second.gen != gen) return false;  // forgotten or changed: stop
            it->second.c.progress = t ? float(double(d) / double(t)) : 1.0f;
            return true;
        });
        std::lock_guard<std::mutex> l(b.m);
        auto it = b.entries.find(path);
        if (it != b.entries.end() && it->second.gen == gen) {
            it->second.c.done = true;
            it->second.c.progress = 1;
            it->second.c.result = res;
        }
    }
}

}  // namespace

Check check(const std::string& zip) {
    struct stat st;
    if (stat(zip.c_str(), &st) != 0) {
        Check c;
        c.done = true;
        c.result.state = State::Error;
        c.result.reason = "the file can't be opened";
        return c;
    }
    Bg& b = bg();
    std::lock_guard<std::mutex> l(b.m);
    auto it = b.entries.find(zip);
    if (it != b.entries.end() && it->second.size == uint64_t(st.st_size) && it->second.mtime == int64_t(st.st_mtime))
        return it->second.c;
    Entry& e = b.entries[zip];
    e = Entry{};
    e.size = uint64_t(st.st_size);
    e.mtime = int64_t(st.st_mtime);
    e.gen = ++b.gen;
    b.queue.push_back({zip, e.gen});
    b.sources = Sources::current();
    if (!b.running) {
        b.running = true;
        std::thread(worker).detach();
    }
    return e.c;
}

bool cached(const std::string& zip, Result& r) {
    struct stat st;
    if (stat(zip.c_str(), &st) != 0) return false;
    Bg& b = bg();
    std::lock_guard<std::mutex> l(b.m);
    auto it = b.entries.find(zip);
    if (it == b.entries.end() || !it->second.c.done || it->second.size != uint64_t(st.st_size) ||
        it->second.mtime != int64_t(st.st_mtime) || it->second.c.result.state == State::Error)
        return false;
    r = it->second.c.result;
    return true;
}

bool trust(const Result& r, std::string& err, std::string* savedTo) {
    if (!r.valid() || r.cert.der.empty()) {
        err = "there is no valid signature to trust";
        return false;
    }
    std::string dir = Partitions::get().appDir() + "/keys";
    util::mkdirs(dir);
    std::string name;
    for (char ch : r.signer()) name += (isalnum((unsigned char)ch) || ch == '-' || ch == '_') ? ch : '_';
    if (name.empty()) name = "key";
    std::string path = dir + "/" + name.substr(0, 40) + "-" + r.cert.keyId().substr(0, 8) + ".x509.pem";
    if (!util::writeFile(path, r.cert.pem())) {
        err = "couldn't write " + path;
        return false;
    }
    {  // the next check of a zip must see the new key
        Bg& b = bg();
        std::lock_guard<std::mutex> l(b.m);
        b.entries.clear();
    }
    if (savedTo) *savedTo = path;
    return true;
}

}  // namespace tw::zipsig
