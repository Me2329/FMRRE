// Host self-tests for the parts of TWRP v2 that touch data:
//   - SHA-256
//   - real update-binary protocol (ui_print / progress / set_progress) - non-simulated path
//   - Android sparse image flashing
//   - backup -> wipe -> restore round trip (tar + gzip + sha256) in the simulator sandbox
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "core/log.h"
#include "core/sha256.h"
#include "core/task.h"
#include "core/util.h"
#include "platform/device.h"
#include "sim.h"
#include "sys/avb.h"
#include "sys/backup.h"
#include "sys/bootimg.h"
#include "sys/imagesrc.h"
#include "sys/installer.h"
#include "sys/partitions.h"
#include "sys/ors.h"
#include "sys/pathcheck.h"
#include "sys/super.h"
#include "miniz.h"

#include <fcntl.h>

using namespace tw;

static int g_fail = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_fail++;                                                     \
        }                                                                 \
    } while (0)

static bool runTask(const std::string& name, Task::Fn fn) {
    Task::get().start(name, fn);
    Task::get().join();
    return Task::get().success();
}

static void testSha() {
    CHECK(Sha256::hexOf("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(Sha256::hexOf("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

static void testInstaller(const std::string& dir) {
    device::init(false, "");
    std::string zip = dir + "/test.zip";
    mz_zip_archive z{};
    CHECK(mz_zip_writer_init_file(&z, zip.c_str(), 0));
    std::string bin =
        "#!/bin/sh\n"
        "OUTFD=$2\n"
        "echo \"ui_print Hello from update-binary\" >&$OUTFD\n"
        "echo \"progress 0.5 0\" >&$OUTFD\n"
        "echo \"set_progress 1.0\" >&$OUTFD\n"
        "echo \"ui_print zip=$3\" >&$OUTFD\n"
        "echo 'stdout line'\n"
        "exit 0\n";
    mz_zip_writer_add_mem(&z, "META-INF/com/google/android/update-binary", bin.data(), bin.size(), MZ_BEST_COMPRESSION);
    mz_zip_writer_finalize_archive(&z);
    mz_zip_writer_end(&z);
    installer::InstallOptions o;
    bool ok = runTask("install", [&](Task& t) { return installer::installZip(zip, o, t); });
    CHECK(ok);
    bool sawHello = false, sawZip = false;
    for (auto& l : Task::get().lines(100)) {
        if (l == "Hello from update-binary") sawHello = true;
        if (l == "zip=" + zip) sawZip = true;
    }
    CHECK(sawHello);
    CHECK(sawZip);
    CHECK(Task::get().progress() >= 0.5f);

    // failing update-binary must report failure
    std::string zip2 = dir + "/fail.zip";
    mz_zip_archive z2{};
    mz_zip_writer_init_file(&z2, zip2.c_str(), 0);
    std::string bad = "#!/bin/sh\necho \"ui_print boom\" >&$2\nexit 7\n";
    mz_zip_writer_add_mem(&z2, "META-INF/com/google/android/update-binary", bad.data(), bad.size(), 0);
    mz_zip_writer_finalize_archive(&z2);
    mz_zip_writer_end(&z2);
    CHECK(!runTask("install-fail", [&](Task& t) { return installer::installZip(zip2, o, t); }));
}

static void testSparse(const std::string& dir) {
    // Build a sparse image: raw(1 blk) + fill(2 blk) + dontcare(1 blk) + raw(1 blk)
    const uint32_t bs = 4096;
    std::string img;
    auto u16 = [&](uint16_t v) { img.append((const char*)&v, 2); };
    auto u32 = [&](uint32_t v) { img.append((const char*)&v, 4); };
    u32(0xED26FF3A); u16(1); u16(0); u16(28); u16(12); u32(bs); u32(5); u32(4); u32(0);
    std::string blkA(bs, 'A'), blkB(bs, 'B');
    u16(0xCAC1); u16(0); u32(1); u32(12 + bs); img += blkA;
    u16(0xCAC2); u16(0); u32(2); u32(16); u32(0x5A5A5A5A);
    u16(0xCAC3); u16(0); u32(1); u32(12);
    u16(0xCAC1); u16(0); u32(1); u32(12 + bs); img += blkB;
    util::writeFile(dir + "/sparse.img", img);
    std::string target = dir + "/target.bin";
    util::writeFile(target, std::string(5 * bs, '\x01'));
    CHECK(installer::isSparseImage(dir + "/sparse.img"));
    CHECK(runTask("sparse", [&](Task& t) { return installer::flashImage(dir + "/sparse.img", target, t); }));
    std::string out;
    util::readFile(target, out);
    CHECK(out.size() == 5 * bs);
    CHECK(out.substr(0, bs) == blkA);
    CHECK(out.substr(bs, 2 * bs) == std::string(2 * bs, 'Z'));
    CHECK(out.substr(3 * bs, bs) == std::string(bs, '\x01'));  // don't care keeps old content
    CHECK(out.substr(4 * bs, bs) == blkB);
}

static void testBackupRestore(const std::string& dir) {
    std::string root = dir + "/sim";
    sim::ensureRoot(root);
    device::init(true, root);
    Partitions::get().load();
    Partitions::get().mountAllStorage();
    Partitions::get().refresh();
    std::string marker = root + "/data/data/com.example.app3/databases/main.db";
    std::string before;
    CHECK(util::readFile(marker, before));
    std::string bootBefore;
    util::readFile(root + "/dev/block/by-name/boot_a", bootBefore);

    backup::Options o;
    o.parts = {"/data", "/boot"};
    o.name = "selftest";
    o.compress = true;
    o.digest = true;
    CHECK(runTask("backup", [&](Task& t) { return backup::create(o, t); }));
    auto sets = backup::list();
    CHECK(sets.size() == 1);
    if (sets.empty()) return;
    CHECK(sets[0].items.size() == 2);
    CHECK(runTask("verify", [&](Task& t) { return backup::verify(sets[0], t); }));

    // Damage data + boot, then restore.
    util::removeTree(root + "/data/data");
    util::writeFile(root + "/dev/block/by-name/boot_a", std::string(bootBefore.size(), '\0'));
    CHECK(!util::exists(marker));
    CHECK(runTask("restore", [&](Task& t) { return backup::restore(sets[0], {"data", "boot"}, true, t); }));
    std::string after, bootAfter;
    CHECK(util::readFile(marker, after));
    CHECK(after == before);
    util::readFile(root + "/dev/block/by-name/boot_a", bootAfter);
    CHECK(bootAfter == bootBefore);
    // internal storage must survive the restore wipe
    CHECK(util::exists(root + "/data/media/0/Download/notes.txt"));

    // Corrupt the archive: verification must catch it and restore must refuse.
    std::string f = sets[0].path + "/" + sets[0].items[0].file;
    std::string data;
    util::readFile(f, data);
    data[data.size() / 2] ^= 0x55;
    util::writeFile(f, data);
    CHECK(!runTask("verify-bad", [&](Task& t) { return backup::verify(sets[0], t); }));
    CHECK(!runTask("restore-bad", [&](Task& t) { return backup::restore(sets[0], {"data"}, true, t); }));
}


// ---- dynamic partitions (LP metadata) -----------------------------------------
static void testSuper(const std::string& dir) {
    device::init(false, "");
    std::string dev = dir + "/super.img";
    const uint64_t size = 1ull << 30;
    int fd = open(dev.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0 && ftruncate(fd, size) == 0);
    close(fd);
    lp::Metadata m;
    m.slotCount = 3;
    m.minor = 2;
    m.headerSize = 256;
    m.devs.push_back({2048, 1 << 20, 0, size, "super", 0});
    m.groups.push_back({"default", 0, 0});
    m.groups.push_back({"main_a", 0, 900ull << 20});
    std::string err;
    CHECK(m.add("system_a", "main_a", lp::kAttrReadonly, err));
    CHECK(m.add("vendor_a", "main_a", lp::kAttrReadonly, err));
    CHECK(m.add("product_a", "main_a", 0, err));
    CHECK(m.resize("system_a", 300ull << 20, err));
    CHECK(m.resize("vendor_a", 100ull << 20, err));
    CHECK(m.resize("product_a", 200ull << 20, err));
    CHECK(lp::format(dev, m, err));
    lp::Metadata r;
    CHECK(lp::read(dev, 0, r, err));
    CHECK(r.serialize() == m.serialize());
    CHECK(r.find("vendor_a") && r.find("vendor_a")->bytes() == (100ull << 20));
    CHECK(r.find("system_a")->attrs == lp::kAttrReadonly);
    uint64_t free0 = r.freeBytes();
    CHECK(free0 > (300ull << 20) && free0 < (1ull << 30));
    // grow system past vendor: needs a second extent
    CHECK(r.resize("system_a", 450ull << 20, err));
    CHECK(r.find("system_a")->bytes() == (450ull << 20));
    CHECK(r.find("system_a")->extents.size() == 2);
    // the group limit (900 MiB) must be enforced
    lp::Metadata over = r;
    CHECK(!over.resize("product_a", 400ull << 20, err));
    CHECK(r.validate(err));
    CHECK(lp::write(dev, r, err));
    lp::Metadata r2;
    CHECK(lp::read(dev, 2, r2, err));  // every slot was written
    CHECK(r2.find("system_a")->bytes() == (450ull << 20));
    // shrink + delete
    CHECK(r2.resize("system_a", 10ull << 20, err));
    CHECK(r2.find("system_a")->extents.size() == 1);
    CHECK(r2.remove("product_a", err));
    CHECK(lp::write(dev, r2, err));
    lp::Metadata r3;
    CHECK(lp::read(dev, 0, r3, err));
    CHECK(!r3.find("product_a") && r3.parts.size() == 2);
    CHECK(r3.freeBytes() > free0);
    // corrupt the primary copy of slot 0: the backup must be used
    std::string saved = dir + "/meta.bin";
    CHECK(lp::saveMetadataRegion(dev, saved, err));
    fd = open(dev.c_str(), O_RDWR);
    char junk[64];
    memset(junk, 0x5a, sizeof(junk));
    CHECK(pwrite(fd, junk, sizeof(junk), 4096 * 3 + 40) == (ssize_t)sizeof(junk));
    close(fd);
    lp::Metadata r4;
    CHECK(lp::read(dev, 0, r4, err));
    CHECK(r4.serialize() == r3.serialize());
    CHECK(lp::restoreMetadataRegion(dev, saved, err));
    // a bad geometry must be rejected, not misread
    std::string notSuper = dir + "/notsuper.img";
    util::writeFile(notSuper, std::string(1 << 20, '\0'));
    lp::Metadata bad;
    CHECK(!lp::read(notSuper, 0, bad, err));
}

// ---- streaming image flasher ---------------------------------------------------
static std::string makeSparse(uint32_t bs, std::string& expect) {
    std::string img;
    auto u16 = [&](uint16_t v) { img.append((const char*)&v, 2); };
    auto u32 = [&](uint32_t v) { img.append((const char*)&v, 4); };
    u32(0xED26FF3A); u16(1); u16(0); u16(28); u16(12); u32(bs); u32(4); u32(3); u32(0);
    std::string a(bs, '\0');
    a[1080] = 0x53; a[1081] = (char)0xEF;
    for (uint32_t i = 2048; i < bs; i++) a[i] = char('a' + i % 26);
    u16(0xCAC1); u16(0); u32(1); u32(12 + bs); img += a;
    u16(0xCAC2); u16(0); u32(2); u32(16); u32(0x41414141);
    u16(0xCAC1); u16(0); u32(1); u32(12 + bs); img += std::string(bs, 'Z');
    expect = a + std::string(2 * bs, 'A') + std::string(bs, 'Z');
    return img;
}

static void gzipTo(const std::string& path, const std::string& data) {
    size_t clen = 0;
    void* c = tdefl_compress_mem_to_heap(data.data(), data.size(), &clen, TDEFL_DEFAULT_MAX_PROBES);
    std::string out("\x1f\x8b\x08\0\0\0\0\0\0\x03", 10);
    out.append((const char*)c, clen);
    mz_free(c);
    uint32_t crc = (uint32_t)mz_crc32(MZ_CRC32_INIT, (const unsigned char*)data.data(), data.size()), sz = (uint32_t)data.size();
    out.append((const char*)&crc, 4);
    out.append((const char*)&sz, 4);
    util::writeFile(path, out);
}

static void testImages(const std::string& dir) {
    device::init(false, "");
    const uint32_t bs = 4096;
    std::string expect;
    std::string sparse = makeSparse(bs, expect);
    std::string raw = std::string(3 * bs + 123, 'r');
    for (size_t i = 0; i < raw.size(); i++) raw[i] = char(i * 2654435761u >> 24);
    struct Case { std::string file, entry; const std::string* want; img::Compression comp; bool sparse; };
    std::vector<Case> cases;
    util::writeFile(dir + "/a.img", sparse);
    cases.push_back({dir + "/a.img", "", &expect, img::Compression::None, true});
    util::writeFile(dir + "/b.img", raw);
    cases.push_back({dir + "/b.img", "", &raw, img::Compression::None, false});
    gzipTo(dir + "/c.img.gz", sparse);
    cases.push_back({dir + "/c.img.gz", "", &expect, img::Compression::Gzip, true});
    gzipTo(dir + "/d.img.gz", raw);
    cases.push_back({dir + "/d.img.gz", "", &raw, img::Compression::Gzip, false});
    mz_zip_archive z{};
    mz_zip_writer_init_file(&z, (dir + "/e.zip").c_str(), 0);
    mz_zip_writer_add_mem(&z, "README.txt", "hi", 2, 0);
    mz_zip_writer_add_mem(&z, "system.img", sparse.data(), sparse.size(), MZ_BEST_COMPRESSION);
    mz_zip_writer_finalize_archive(&z);
    mz_zip_writer_end(&z);
    cases.push_back({dir + "/e.zip", "", &expect, img::Compression::Zip, true});
    if (util::haveBinary("xz")) {
        util::writeFile(dir + "/f.img", sparse);
        util::writeFile(dir + "/g.img", raw);
        util::run("xz -f " + dir + "/f.img && xz -f --check=sha256 " + dir + "/g.img");
        cases.push_back({dir + "/f.img.xz", "", &expect, img::Compression::Xz, true});
        cases.push_back({dir + "/g.img.xz", "", &raw, img::Compression::Xz, false});
    } else {
        printf("  (xz not installed on the host: skipping .xz cases)\n");
    }
    for (auto& c : cases) {
        img::Info info = img::inspect(c.file, c.entry);
        CHECK(info.ok);
        CHECK(info.comp == c.comp);
        CHECK(info.sparse == c.sparse);
        CHECK(info.imageSize == c.want->size());
        if (c.sparse) CHECK(info.kind == img::Kind::Ext4);
        std::string target = dir + "/target.bin";
        util::writeFile(target, std::string(8 * bs, '\x01'));
        img::FlashOptions o;
        o.verify = true;
        bool ok = runTask("flash " + util::basename(c.file), [&](Task& t) { return img::flash(c.file, c.entry, target, o, t); });
        CHECK(ok);
        std::string out;
        util::readFile(target, out);
        CHECK(out.substr(0, c.want->size()) == *c.want);
        if (!ok || out.substr(0, c.want->size()) != *c.want) fprintf(stderr, "   case: %s\n", c.file.c_str());
    }
    // too big for the partition: refused before writing anything
    std::string small = dir + "/small.bin";
    util::writeFile(small, std::string(bs, '\x02'));
    CHECK(!runTask("too-big", [&](Task& t) { return img::flash(dir + "/a.img", "", small, {}, t); }));
    std::string s2;
    util::readFile(small, s2);
    CHECK(s2 == std::string(bs, '\x02'));
    // corrupt gzip must fail
    std::string gz;
    util::readFile(dir + "/d.img.gz", gz);
    gz[gz.size() / 2] ^= 0xFF;
    util::writeFile(dir + "/bad.img.gz", gz);
    util::writeFile(dir + "/target.bin", std::string(8 * bs, '\x01'));
    CHECK(!runTask("bad-gz", [&](Task& t) { return img::flash(dir + "/bad.img.gz", "", dir + "/target.bin", {}, t); }));
    // dd command generation
    img::Info gi = img::inspect(dir + "/d.img.gz");
    auto dd = img::ddCommand(gi, "/dev/block/mapper/system_a");
    std::string all;
    for (auto& l : dd.lines) all += l + "\n";
    CHECK(all.find("blockdev --setrw '/dev/block/mapper/system_a'") != std::string::npos);
    CHECK(all.find("gzip -dc") != std::string::npos && all.find("| dd of=") != std::string::npos);
    auto dds = img::ddCommand(img::inspect(dir + "/a.img"), "/dev/block/by-name/boot_a");
    CHECK(dds.needsSimg2img && dds.lines[0].find("simg2img") == 0);
    CHECK(img::isImageName("system-td-arm64-ab-vanilla.img.xz") && !img::isImageName("rom.zip"));
}

static void testAvb(const std::string& dir) {
    std::string vb(4096, '\0');
    memcpy(&vb[0], "AVB0", 4);
    vb[123] = 0;
    util::writeFile(dir + "/vbmeta.img", vb);
    avb::Info i = avb::read(dir + "/vbmeta.img");
    CHECK(i.ok && i.flags == 0);
    std::string err;
    CHECK(avb::setFlags(dir + "/vbmeta.img", 3, err));
    std::string out;
    util::readFile(dir + "/vbmeta.img", out);
    CHECK(out.size() == 4096 && (uint8_t)out[123] == 3 && out[120] == 0);
    CHECK(!avb::read(dir + "/a.img").ok);
}

static void testPathcheck(const std::string& dir) {
    std::string root = dir + "/sim";
    device::init(true, root);
    Partitions::get().load();
    Partitions::get().mountAllStorage();
    std::string media = root + "/data/media/0";
    CHECK(pathcheck::resolve("/sdcard/Download/x.img") == media + "/Download/x.img");
    CHECK(pathcheck::resolve("/storage/emulated/0/Download/x.img") == media + "/Download/x.img");
    CHECK(pathcheck::resolve("Download/x.img") == media + "/Download/x.img");
    util::writeFile(media + "/Download/System.img", "x");
    auto r = pathcheck::check("/sdcard/Download/system.img");
    CHECK(!r.exists && !r.suggestions.empty());
    auto r2 = pathcheck::check("/sdcard/Download/GSI_A15_download(1)");
    CHECK(r2.usable() && !r2.shownInImagePicker);
    auto r3 = pathcheck::check("/sdcard/Download/pixel-gsi.img.crdownload");
    bool flagged = false;
    for (auto& f : r3.findings) if (f.level == pathcheck::Level::Error) flagged = true;
    CHECK(flagged);
    // sim super is consistent with the mapper files
    lp::Metadata m;
    std::string err;
    CHECK(lp::read(lp::superDevice(), 0, m, err));
    CHECK(m.find("system_a") && util::fileSize(root + "/dev/block/mapper/system_a") == m.find("system_a")->bytes());

    // AOSP recovery arguments -> ORS
    auto l = ors::fromAospArgs({"--update_package=/data/ota.zip", "--wipe_cache", "--reason=foo", "--update_package=@/cache/recovery/block.map"});
    CHECK(l.size() == 3 && l[0] == "install /data/ota.zip" && l[1] == "wipe cache" && l[2].find("print") == 0);
    // bootloader message in misc is read and cleared
    std::string misc(4096, '\0');
    memcpy(&misc[0], "boot-recovery", 13);
    std::string rec = "recovery\n--wipe_data\n";
    memcpy(&misc[64], rec.data(), rec.size());
    misc[2048] = 'X';  // A/B slot metadata after the message must survive
    util::writeFile(root + "/dev/block/by-name/misc", misc);
    ors::Script sc;
    CHECK(ors::takePending(sc));
    CHECK(sc.lines.size() == 1 && sc.lines[0] == "wipe data");
    std::string after;
    util::readFile(root + "/dev/block/by-name/misc", after);
    CHECK(after.size() == 4096 && after[0] == 0 && after[64] == 0 && after[2048] == 'X');
    ors::Script none;
    CHECK(!ors::takePending(none));
    unlink((root + "/dev/block/by-name/misc").c_str());
    // ORS script runs and stops at the first failure
    CHECK(runTask("ors", [](Task& t) { return ors::run({"print one", "mkdir /tmp/ors-test-dir", "print two"}, t); }));
    CHECK(util::isDir(root + "/tmp/ors-test-dir"));
    CHECK(!runTask("ors-fail", [](Task& t) { return ors::run({"print a", "bogus", "mkdir /tmp/never"}, t); }));
    CHECK(!util::exists(root + "/tmp/never"));

    // Installing TWRP v2 permanently: A/B phone without a recovery partition -> boot, stock saved first
    auto bootImg = [](const std::string& f, uint32_t hv, uint32_t kernel, char fill) {
        std::string b(2 << 20, fill);
        memcpy(&b[0], "ANDROID!", 8);
        memcpy(&b[8], &kernel, 4);
        memcpy(&b[40], &hv, 4);
        util::writeFile(f, b);
    };
    std::string bootA = root + "/dev/block/by-name/boot_a", stockBoot;
    bootImg(bootA, 4, 1 << 20, 'S');
    util::readFile(bootA, stockBoot);
    std::string twrp = root + "/data/media/0/Download/twrpv2-test.img", junk = root + "/data/media/0/Download/junk.img";
    bootImg(twrp, 4, 1 << 20, 'T');
    CHECK(bootimg::recoveryTarget(twrp).name == "boot");
    CHECK(runTask("install-recovery", [&](Task& t) { return bootimg::installRecovery(twrp, t); }));
    std::string now, img;
    util::readFile(bootA, now);
    util::readFile(twrp, img);
    CHECK(now == img);
    auto saved = bootimg::imagesIn(bootimg::stockDir());
    std::string savedData;
    CHECK(saved.size() == 1 && util::readFile(saved[0], savedData) && savedData == stockBoot);
    // header mismatch with the phone's boot is refused, and boot stays untouched
    bootImg(root + "/data/media/0/Download/v2.img", 2, 1 << 20, 'X');
    CHECK(!runTask("install-mismatch", [&](Task& t) { return bootimg::installRecovery(root + "/data/media/0/Download/v2.img", t); }));
    util::readFile(bootA, now);
    CHECK(now == img);
    util::writeFile(junk, std::string(8192, 'j'));
    CHECK(!runTask("install-junk", [&](Task& t) { return bootimg::installRecovery(junk, t); }));
    // with a recovery partition the image goes there instead
    util::writeFile(root + "/dev/block/by-name/recovery_a", std::string(2 << 20, 'R'));
    CHECK(bootimg::recoveryTarget(twrp).name == "recovery");
    CHECK(runTask("install-recovery-part", [&](Task& t) { return bootimg::installRecovery(twrp, t); }));
    util::readFile(root + "/dev/block/by-name/recovery_a", now);
    CHECK(now == img);
    CHECK(bootimg::imagesIn(bootimg::stockDir()).size() == 2);
    unlink((root + "/dev/block/by-name/recovery_a").c_str());
}

int main() {
    char tmpl[] = "/tmp/twrpv2-selftest-XXXXXX";
    std::string dir = mkdtemp(tmpl);
    log::init(dir + "/test.log");
    testSha();
    testInstaller(dir);
    testSparse(dir);
    testSuper(dir);
    testImages(dir);
    testAvb(dir);
    testBackupRestore(dir);
    testPathcheck(dir);
    util::removeTree(dir);
    if (g_fail) {
        fprintf(stderr, "%d check(s) FAILED\n", g_fail);
        return 1;
    }
    printf("All self-tests passed\n");
    return 0;
}
