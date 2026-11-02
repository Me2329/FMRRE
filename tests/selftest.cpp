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
#include "sys/backup.h"
#include "sys/installer.h"
#include "sys/partitions.h"
#include "miniz.h"

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

int main() {
    char tmpl[] = "/tmp/twrpv2-selftest-XXXXXX";
    std::string dir = mkdtemp(tmpl);
    log::init(dir + "/test.log");
    testSha();
    testInstaller(dir);
    testSparse(dir);
    testBackupRestore(dir);
    util::removeTree(dir);
    if (g_fail) {
        fprintf(stderr, "%d check(s) FAILED\n", g_fail);
        return 1;
    }
    printf("All self-tests passed\n");
    return 0;
}
