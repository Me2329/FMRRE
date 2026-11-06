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
    auto payloadKey = [&](const std::string& root, std::vector<x509::Cert>& into) {  // update_engine's key for A/B OTAs
        for (auto rel : {"/system/etc/update_engine/update-payload-key.pub.pem", "/etc/update_engine/update-payload-key.pub.pem"}) {
            std::string b;
            if (!util::readFile(root + rel, b, 1 << 16)) continue;
            for (auto& c : x509::loadPublicKeys(b)) {
                bool dup = false;
                for (auto& have : into) dup |= have.sameKey(c);
                if (!dup) into.push_back(std::move(c));
            }
            return;
        }
    };
    auto fromRoot = [&](const std::string& root) {
        payloadKey(root, k.system);
        for (auto rel : {"/system/etc/security/otacerts.zip", "/etc/security/otacerts.zip"}) {  // system-as-root, older
            std::string p = root + rel;
            if (!util::exists(p)) continue;
            std::string e;
            for (auto& c : x509::loadZip(p, &e)) k.system.push_back(std::move(c));
            k.systemFrom = "system: " + std::string(rel);
            if (k.system.empty()) k.systemNote = "the installed system's otacerts.zip has no usable key (" + e + ")";
            return;
        }
        if (!k.system.empty()) k.systemFrom = "system: update_engine's payload key";
        else k.systemNote = "the installed system has no otacerts.zip";
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
    if (!s.recoveryResKeys.empty()) {  // recovery's own update_engine key (its ramdisk's /etc)
        size_t before = k.recovery.size();
        payloadKey(util::dirname(util::dirname(s.recoveryResKeys)), k.recovery);
        if (k.recovery.size() > before && k.recoveryFrom.empty()) k.recoveryFrom = "recovery: update_engine's payload key";
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

// ---- payload.bin signatures --------------------------------------------------------------------------

namespace {

struct RawSig {
    std::string data;
    uint32_t unpadded = 0;
};

// update_engine's Signatures message: repeated Signature { uint32 version = 1; bytes data = 2;
// fixed32 unpadded_signature_size = 3; }
bool parseSignatures(const std::string& blob, std::vector<RawSig>& out) {
    struct Pb {
        const uint8_t *p, *e;
        bool varint(uint64_t& v) {
            v = 0;
            for (int sh = 0; sh < 64; sh += 7) {
                if (p >= e) return false;
                uint8_t b = *p++;
                v |= uint64_t(b & 0x7F) << sh;
                if (!(b & 0x80)) return true;
            }
            return false;
        }
        bool skip(uint32_t wire) {
            uint64_t v;
            if (wire == 0) return varint(v);
            if (wire == 1) return e - p >= 8 ? (p += 8, true) : false;
            if (wire == 5) return e - p >= 4 ? (p += 4, true) : false;
            if (wire == 2) return varint(v) && uint64_t(e - p) >= v ? (p += v, true) : false;
            return false;
        }
    };
    Pb top{(const uint8_t*)blob.data(), (const uint8_t*)blob.data() + blob.size()};
    while (top.p < top.e) {
        uint64_t key, len;
        if (!top.varint(key)) return false;
        if ((key >> 3) != 1 || (key & 7) != 2) {
            if (!top.skip(uint32_t(key & 7))) return false;
            continue;
        }
        if (!top.varint(len) || uint64_t(top.e - top.p) < len) return false;
        Pb s{top.p, top.p + len};
        top.p += len;
        RawSig rs;
        while (s.p < s.e) {
            uint64_t k, n;
            if (!s.varint(k)) return false;
            if ((k >> 3) == 2 && (k & 7) == 2) {
                if (!s.varint(n) || uint64_t(s.e - s.p) < n) return false;
                rs.data.assign((const char*)s.p, n);
                s.p += n;
            } else if ((k >> 3) == 3 && (k & 7) == 5) {
                if (s.e - s.p < 4) return false;
                rs.unpadded = uint32_t(s.p[0]) | uint32_t(s.p[1]) << 8 | uint32_t(s.p[2]) << 16 | uint32_t(s.p[3]) << 24;
                s.p += 4;
            } else if (!s.skip(uint32_t(k & 7))) {
                return false;
            }
        }
        out.push_back(rs);
    }
    return true;
}

}  // namespace

std::string PayloadSig::line() const {
    auto whose = [&]() -> std::string {
        if (trust == Trust::System) return "the installed system's key";
        if (trust == Trust::Recovery) return "a key this recovery trusts";
        if (trust == Trust::User) return "a key you trusted";
        return zipKey ? "the key that signed the zip" : "an unknown key";
    };
    std::string s;
    switch (metadata) {
        case PState::None: s = "the payload has no metadata signature"; break;
        case PState::Valid: s = "payload metadata signed with " + whose(); break;
        case PState::UnknownKey: s = "the payload is signed with a key FMRRE doesn't know, so it can't be checked"; break;
        case PState::Bad: s = "the payload's metadata signature is damaged (" + reason + ")"; break;
        case PState::Error: s = "the payload's signature couldn't be checked (" + reason + ")"; break;
    }
    if (payload == PState::Valid) s += "; payload signature valid";
    else if (payload == PState::UnknownKey && metadata == PState::Valid)
        s += "; the payload signature DOESN'T match - the payload was changed after signing";
    else if (payload == PState::Bad) s += "; the payload signature is damaged";
    return s;
}

PayloadSig verifyPayload(const PayloadRef& p, const Keys& keys, const x509::Cert* zipCert, bool full, const Progress& progress) {
    PayloadSig r;
    int fd = open(p.path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        r.metadata = PState::Error;
        r.reason = "the file can't be opened";
        return r;
    }
    struct Closer {
        int fd;
        ~Closer() { close(fd); }
    } closer{fd};
    auto readAt = [&](void* buf, size_t n, uint64_t off) {
        size_t got = 0;
        while (got < n) {
            ssize_t k = pread(fd, (uint8_t*)buf + got, n - got, off_t(off + got));
            if (k <= 0) return false;
            got += size_t(k);
        }
        return true;
    };
    // Hash [base, base + len) in pieces.
    auto hashRange = [&](uint64_t len, std::string& digest) {
        Sha256 h;
        std::vector<uint8_t> buf(1 << 20);
        for (uint64_t done = 0; done < len;) {
            size_t n = size_t(std::min<uint64_t>(buf.size(), len - done));
            if (!readAt(buf.data(), n, p.base + done)) return false;
            h.update(buf.data(), n);
            done += n;
            if (progress && !progress(done, len)) return false;
        }
        digest = h.digest();
        return true;
    };
    // Which known key made one of these signatures over this hash?
    auto match = [&](const std::string& digest, const std::vector<RawSig>& sigs, bool record) {
        struct Cand {
            const x509::Cert* c;
            Trust t;
            const std::string* from;
        };
        std::vector<Cand> cands;
        for (auto& c : keys.system) cands.push_back({&c, Trust::System, &keys.systemFrom});
        for (auto& c : keys.recovery) cands.push_back({&c, Trust::Recovery, &keys.recoveryFrom});
        for (auto& c : keys.user) cands.push_back({&c, Trust::User, &keys.userFrom});
        if (zipCert) cands.push_back({zipCert, Trust::Unknown, nullptr});
        for (auto& sg : sigs)
            for (auto& cd : cands) {
                bool ok = false;
                if (cd.c->key == x509::Cert::Key::Rsa) {
                    ok = pk::rsaVerify(cd.c->rsa, pk::Hash::Sha256, digest, (const uint8_t*)sg.data.data(), sg.data.size());
                } else if (cd.c->key == x509::Cert::Key::Ec) {
                    size_t n = sg.unpadded && sg.unpadded <= sg.data.size() ? sg.unpadded : sg.data.size();
                    ok = pk::ecdsaVerify(cd.c->ec, digest, (const uint8_t*)sg.data.data(), n);
                }
                if (!ok) continue;
                if (record) {
                    r.cert = *cd.c;
                    r.trust = cd.t;
                    r.trustedBy = cd.from ? *cd.from : "";
                    r.zipKey = zipCert && cd.c->sameKey(*zipCert);
                    if (r.trust == Trust::Unknown && zipCert && !zipCert->sameKey(*cd.c)) r.zipKey = false;
                }
                return true;
            }
        return false;
    };
    auto readSigs = [&](uint64_t off, uint64_t size, std::vector<RawSig>& sigs) {
        if (size > (1u << 20)) return false;
        std::string blob(size_t(size), '\0');
        return readAt(&blob[0], blob.size(), p.base + off) && parseSignatures(blob, sigs) && !sigs.empty();
    };

    const uint64_t metaLen = 24 + p.manifestSize;
    if (p.metadataSigSize) {
        std::string digest;
        std::vector<RawSig> sigs;
        if (!hashRange(metaLen, digest)) {
            r.metadata = PState::Error;
            r.reason = "the payload can't be read";
        } else if (!readSigs(metaLen, p.metadataSigSize, sigs)) {
            r.metadata = PState::Bad;
            r.reason = "its signature block can't be read";
        } else {
            r.metadata = match(digest, sigs, true) ? PState::Valid : PState::UnknownKey;
        }
    }
    if (full && p.sigSize) {
        std::string digest;
        std::vector<RawSig> sigs;
        if (!hashRange(p.dataStart + p.sigOffset, digest)) r.payload = PState::Error;
        else if (!readSigs(p.dataStart + p.sigOffset, p.sigSize, sigs)) r.payload = PState::Bad;
        else r.payload = match(digest, sigs, r.metadata != PState::Valid) ? PState::Valid : PState::UnknownKey;
    }
    return r;
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
