// Host self-tests for the parts of FMRRE that touch data:
//   - SHA-256
//   - real update-binary protocol (ui_print / progress / set_progress) - non-simulated path
//   - Android sparse image flashing
//   - backup -> wipe -> restore round trip (tar + gzip + sha256) in the simulator sandbox
//   - bzip2 / xz decoding, Reed-Solomon, dm-verity hash tree + FEC, A/B OTA payload extraction
#include <algorithm>
#include <cstdio>
#include <map>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "core/bunzip2.h"
#include "core/crypt.h"
#include "core/i18n.h"
#include "core/log.h"
#include "core/settings.h"
#include "core/sha256.h"
#include "core/task.h"
#include "core/util.h"
#include "platform/device.h"
#include "sim.h"
#include "sys/autofix.h"
#include "sys/avb.h"
#include "sys/backup.h"
#include "sys/bootimg.h"
#include "sys/history.h"
#include "sys/imagesrc.h"
#include "sys/installer.h"
#include "sys/partitions.h"
#include "sys/ors.h"
#include "sys/pathcheck.h"
#include "sys/rescue.h"
#include "sys/payload.h"
#include "sys/super.h"
#include "sys/verity.h"
#include "miniz.h"

#include <fcntl.h>
#include <vector>

using namespace tw;

// bzip2 -9 and xz (CRC64) of the same 16 KiB test pattern (made with Python's bz2 / lzma modules).
static const unsigned char kBz2Fixture[] = {
    0x42,0x5a,0x68,0x39,0x31,0x41,0x59,0x26,0x53,0x59,0xf7,0x57,0x8e,0x0b,0x00,0x09,0xc9,0xff,0xff,0xff,
    0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
    0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xd0,0x03,0xde,0x00,0x00,0x00,0x02,0x4c,0x00,
    0x13,0x00,0x01,0x30,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x26,0x00,0x09,0x80,0x00,0x98,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x93,0x00,0x04,0xc0,0x00,0x4c,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x49,0x80,0x02,0x60,0x00,0x26,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x05,0x54,0xa5,0x13,0x01,0x19,
    0x3d,0x00,0x13,0x00,0x00,0x4d,0x30,0x08,0xcd,0x00,0x09,0xe8,0x98,0x27,0xa2,0x69,0x88,0xf4,0x9e,0x80,
    0x98,0x87,0xa9,0x84,0x60,0xd4,0xc0,0x11,0x89,0x82,0x6d,0x41,0x89,0x91,0x82,0x69,0x9a,0x4c,0x11,0x89,
    0x84,0xd3,0x09,0x80,0x10,0xc4,0x0d,0x34,0xd1,0x88,0xcd,0x41,0x80,0x03,0x44,0xf4,0xe7,0x4a,0xc2,0x09,
    0x69,0x7a,0x08,0x44,0x84,0x21,0x49,0x97,0x41,0x08,0x4c,0x02,0x62,0x13,0x20,0x99,0x84,0xd0,0x26,0xa1,
    0x36,0x0f,0x04,0x3c,0x20,0x9b,0x84,0xe0,0x3c,0x30,0x8a,0x09,0xc8,0x4e,0x82,0x76,0x13,0xc0,0x9e,0x84,
    0xf8,0x27,0xe1,0x40,0x0a,0x08,0x50,0x83,0xc4,0x0a,0x18,0x51,0x02,0x8a,0x14,0x60,0xa3,0x85,0x20,0x2f,
    0x94,0xa0,0x89,0x08,0x42,0x10,0x84,0x21,0x5a,0x00,0x46,0x80,0x29,0x21,0x4d,0x0a,0x70,0x53,0xc2,0xa0,
    0x15,0x10,0xa9,0x05,0x4c,0x3c,0x50,0xaa,0x07,0x8c,0x15,0x50,0xab,0x05,0x5c,0x2b,0x01,0x59,0x0a,0xd0,
    0x61,0x80,0x15,0xc0,0x9d,0x87,0x8e,0x1e,0x40,0x57,0x42,0xbc,0x11,0x61,0x5f,0x0b,0x00,0x58,0x42,0xc4,
    0x16,0x30,0xb2,0x05,0x94,0x2c,0xc1,0x67,0x0b,0x40,0x5a,0x43,0xc9,0x08,0xc0,0xb5,0x05,0xac,0x3c,0xa0,
    0xb6,0x05,0xb4,0x2d,0xc1,0x6f,0x0b,0x80,0x79,0x61,0x1a,0x1e,0x60,0x02,0xe4,0x15,0x60,0xbb,0x00,0x2e,
    0x50,0xdc,0x4c,0x07,0x9a,0x11,0x08,0x2e,0x81,0x5b,0x84,0x21,0x08,0x42,0x10,0x8f,0xbb,0x07,0x9c,0x11,
    0x00,0x1e,0x78,0x4d,0x42,0x38,0x2e,0xe1,0xe8,0x04,0x74,0x78,0x4d,0x02,0xf0,0x17,0x90,0xbd,0x05,0xec,
    0x30,0x60,0x0c,0x20,0x03,0xd1,0x0b,0xf0,0x44,0x00,0x5f,0xc2,0x30,0x30,0x01,0xe9,0x07,0xa6,0x1e,0xa0,
    0x48,0x07,0xaa,0x18,0x10,0x91,0x00,0x60,0xc3,0x08,0x11,0x00,0x1e,0xb0,0x51,0x40,0x11,0x41,0x14,0x1e,
    0xb8,0x61,0x43,0x0c,0x18,0x70,0x98,0x80,0x31,0x01,0x18,0x18,0x90,0x9f,0x00,0x24,0x42,0x70,0x1e,0xc0,
    0x62,0x83,0x16,0x18,0xc0,0xc6,0x86,0x38,0x31,0xe1,0x90,0x0c,0x88,0x64,0x83,0xd9,0x0f,0x68,0x32,0x61,
    0x94,0x0c,0xa8,0x7b,0x61,0x96,0x0c,0xb8,0x66,0x03,0x32,0x19,0xa0,0xf7,0x03,0x36,0x12,0x41,0xee,0x87,
    0xbc,0x1e,0xf8,0x7c,0x01,0x9c,0x0c,0xe8,0x67,0x83,0xe1,0x0c,0xf8,0x68,0x03,0xe2,0x0f,0x8c,0x34,0x21,
    0xa2,0x0d,0x18,0x69,0x03,0x4a,0x1f,0x20,0x69,0x83,0xe5,0x0d,0x38,0x6a,0x02,0x4c,0x35,0x21,0xf3,0x06,
    0xa8,0x35,0x61,0xac,0x0d,0x68,0x6b,0x83,0xe7,0x0f,0xa0,0x35,0xe1,0xb0,0x0d,0x88,0x7d,0x21,0xb2,0x0d,
    0x98,0x6d,0x03,0x6a,0x1f,0x50,0x6d,0x83,0xeb,0x0d,0xb8,0x5e,0x77,0x01,0x20,0x1b,0x90,0xdd,0x07,0xd8,
    0x1b,0xb0,0xde,0x06,0xf4,0x37,0xc1,0xbf,0x0e,0x00,0x70,0x43,0x84,0x1c,0x30,0xe2,0x07,0xda,0x1f,0x70,
    0x7d,0xe1,0xc5,0x0e,0x30,0x71,0xc3,0xf0,0x0f,0xc4,0x3f,0x20,0xfc,0xc3,0xf4,0x0e,0x40,0x7e,0xa1,0xc9,
    0x0f,0xd8,0x39,0x41,0xfb,0x87,0xf0,0x1c,0xb0,0xe6,0x07,0x34,0x3f,0x90,0xfe,0x83,0x9c,0x1c,0xf0,0x92,
    0x94,0x09,0x40,0xfe,0xc3,0xfc,0x0f,0xf4,0x3f,0xe0,0xe8,0x07,0x44,0x3a,0x41,0xd3,0x0e,0xa0,0x75,0x43,
    0xac,0x1d,0x70,0xec,0x07,0x64,0x3b,0x41,0xdb,0x0e,0xe0,0x4a,0x84,0x5c,0x20,0x9e,0x43,0xba,0x12,0x61,
    0xde,0x0f,0xfc,0x25,0x83,0xbe,0x2e,0xe4,0x8a,0x70,0xa1,0x21,0xee,0xaf,0x1c,0x16,
};
static const unsigned char kXzFixture[] = {
    0xfd,0x37,0x7a,0x58,0x5a,0x00,0x00,0x04,0xe6,0xd6,0xb4,0x46,0x02,0x00,0x21,0x01,0x16,0x00,0x00,0x00,
    0x74,0x2f,0xe5,0xa3,0xe0,0x3f,0xff,0x01,0x36,0x5d,0x00,0x2a,0x15,0xc6,0x6c,0x0d,0x7a,0x3e,0xbc,0x10,
    0xfa,0x6e,0xa9,0x25,0xa9,0x64,0x66,0x6c,0x4e,0xf6,0xf0,0x19,0x04,0xa0,0x11,0xf6,0x76,0xcc,0x1b,0xcf,
    0x25,0xdb,0x5d,0x8e,0x10,0xa6,0x52,0x3d,0x2b,0x48,0x51,0x47,0x37,0x48,0x2f,0xcc,0x85,0x68,0xd3,0xf0,
    0x7e,0xb9,0xa5,0xae,0xce,0xd0,0xdb,0x99,0x03,0x96,0xc4,0xfe,0xc4,0xf6,0xd3,0x4c,0x62,0xec,0x98,0x51,
    0x0d,0x76,0x69,0x2c,0x60,0xa4,0x4a,0xe1,0x7c,0x2b,0x96,0x9c,0x91,0xe3,0x06,0xdf,0x0d,0x2f,0x58,0xea,
    0xe5,0x90,0xce,0x13,0xdf,0x7d,0x29,0xff,0x7a,0x60,0x57,0x8c,0xec,0x05,0xc7,0x52,0xdf,0xe0,0xd9,0x1b,
    0xfb,0x41,0xa7,0xa0,0xee,0x70,0x7f,0xe4,0xbf,0x93,0xbc,0x1e,0x48,0x00,0x7c,0x39,0xa5,0xf1,0x7a,0xfc,
    0x9e,0x8f,0xf4,0x49,0x35,0x21,0x1b,0x25,0x35,0x4f,0xf8,0x67,0xaf,0xfd,0x33,0xed,0x12,0x28,0x5c,0x0c,
    0xcf,0x63,0x2c,0x19,0x27,0xf8,0x9e,0x0a,0x12,0x47,0x5a,0x6c,0xd3,0xfa,0x67,0x23,0xc4,0x3b,0x10,0x7c,
    0x39,0x76,0xfa,0xbb,0x70,0x73,0xfa,0xf2,0x7d,0x28,0x9c,0x31,0x4b,0xe9,0x0f,0x53,0x29,0x60,0x8f,0xc9,
    0x98,0xff,0x70,0x1f,0xf5,0xbd,0x99,0xbf,0xbb,0xd3,0xe2,0x2a,0x9a,0xd2,0x9d,0x87,0x9a,0x16,0xa7,0x6f,
    0xac,0xcf,0x51,0x85,0x2f,0x89,0x1b,0x32,0x1f,0x9a,0x90,0xcc,0x12,0xe0,0x8e,0x34,0x02,0xb5,0xbf,0x5f,
    0x2a,0x9f,0xee,0x5c,0xdf,0x30,0x96,0x46,0xac,0x64,0x6e,0x05,0xfd,0x52,0xbb,0xf4,0x34,0x27,0xc1,0xb6,
    0x87,0xa7,0x1d,0xf4,0x1d,0xcb,0x3c,0xae,0x06,0x38,0x2d,0x57,0x24,0x9d,0x56,0x80,0x55,0x28,0x22,0x64,
    0x02,0x2b,0x00,0x87,0x82,0x32,0x7d,0xc4,0x4a,0x12,0x06,0xa7,0x53,0x93,0x46,0x23,0xf4,0xad,0x0f,0xa2,
    0xb1,0xc8,0x86,0x06,0x09,0xaa,0xe5,0x0f,0x24,0x28,0x7c,0x2c,0xef,0xa1,0xa6,0x1f,0xef,0xb3,0xcf,0x08,
    0x00,0x00,0x00,0x00,0x7f,0x2f,0x31,0x47,0x50,0xdd,0xd7,0x01,0x00,0x01,0xd2,0x02,0x80,0x80,0x01,0x00,
    0x63,0xa6,0x14,0xf3,0xb1,0xc4,0x67,0xfb,0x02,0x00,0x00,0x00,0x00,0x04,0x59,0x5a,
};

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
    printf("SHA-256 backend: %s\n", Sha256::backend());
    CHECK(Sha256::hexOf("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(Sha256::hexOf("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(Sha256::hexOf("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    // The hardware path (ARMv8 / Intel SHA) must agree with the portable one for every length and split.
    std::string data(70000, '\0');
    uint32_t x = 12345;
    for (auto& ch : data) { x = x * 1103515245 + 12345; ch = char(x >> 16); }
    for (size_t len : {0, 1, 55, 56, 63, 64, 65, 127, 128, 1000, 4096, 65536, 70000})
        for (size_t split : {0, 1, 63, 64, 100}) {
            auto run = [&](bool soft) {
                Sha256::forceSoftware(soft);
                Sha256 h;
                size_t a = std::min(split, len);
                h.update(data.data(), a);
                h.update(data.data() + a, len - a);
                return h.hexDigest();
            };
            std::string hw = run(false), sw = run(true);
            CHECK(hw == sw);
        }
    Sha256::forceSoftware(false);
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

    // Encrypted backup: AES-256-CTR + HMAC, the password checked before anything is wiped.
    backup::Options e = o;
    e.name = "selftest-enc";
    e.password = "c0rrect horse";
    CHECK(runTask("backup-enc", [&](Task& t) { return backup::create(e, t); }));
    backup::Set es;
    for (auto& x : backup::list())
        if (x.name == "selftest-enc") es = x;
    CHECK(es.items.size() == 2 && backup::encrypted(es));
    for (auto& it : es.items) {
        std::string raw;
        util::readFile(es.path + "/" + it.file, raw);
        CHECK(it.encrypted && util::endsWith(it.file, ".enc") && raw.compare(0, 8, "TWV2ENC1") == 0);
        CHECK(raw.find("com.example.app") == std::string::npos);  // no plaintext (tar names) in the file
    }
    std::string perr;
    CHECK(!backup::checkPassword(es, "wrong", perr) && perr.find("wrong password") != std::string::npos);
    CHECK(backup::checkPassword(es, e.password, perr));
    CHECK(runTask("verify-enc", [&](Task& t) { return backup::verify(es, t); }));  // SHA-256 needs no password
    util::removeTree(root + "/data/data");
    util::writeFile(root + "/dev/block/by-name/boot_a", std::string(bootBefore.size(), '\0'));
    util::writeFile(root + "/data/media/0/keep.txt", "keep");
    CHECK(!runTask("restore-enc-wrong", [&](Task& t) { return backup::restore(es, {"data", "boot"}, false, t, "nope"); }));
    CHECK(util::exists(root + "/data/media/0/keep.txt"));  // nothing was wiped
    CHECK(!runTask("restore-enc-none", [&](Task& t) { return backup::restore(es, {"boot"}, false, t); }));
    CHECK(runTask("restore-enc", [&](Task& t) { return backup::restore(es, {"data", "boot"}, true, t, e.password); }));
    after.clear();
    CHECK(util::readFile(marker, after) && after == before);
    bootAfter.clear();
    util::readFile(root + "/dev/block/by-name/boot_a", bootAfter);
    CHECK(bootAfter == bootBefore);
    // Tampering with the ciphertext: the HMAC catches it even without the digest check.
    for (auto& it : es.items)
        if (it.part == "boot") {
            std::string fe = es.path + "/" + it.file, d2;
            util::readFile(fe, d2);
            d2[d2.size() / 2] ^= 0x01;
            util::writeFile(fe, d2);
        }
    CHECK(!runTask("restore-enc-tampered", [&](Task& t) { return backup::restore(es, {"boot"}, false, t, e.password); }));
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

// Renamed from TWRP v2: FMRRE/ is home, and backups in TWRP/BACKUPS (classic TWRP, or TWRP v2) still restore.
static void testLegacyFolders() {
    Partitions& p = Partitions::get();
    CHECK(p.appDir() == p.storagePath() + "/FMRRE" && p.legacyDir() == p.storagePath() + "/TWRP");
    std::string old = p.legacyDir() + "/BACKUPS/" + device::info().serial + "/2024-05-01--10-00-00";
    CHECK(util::mkdirs(old));
    CHECK(util::writeFile(old + "/boot.emmc.win", std::string(4096, 'b')));
    bool found = false, ours = false;
    for (auto& s : backup::list()) {
        found |= util::basename(s.path) == "2024-05-01--10-00-00" && !s.hasManifest && s.items.size() == 1 && s.items[0].part == "boot";
        ours |= s.name == "selftest" && util::startsWith(s.path, p.appDir() + "/BACKUPS/");
    }
    CHECK(found && ours);
    util::removeTree(p.legacyDir());
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
    // `adb reboot fastboot` / `fastboot reboot fastboot` put --fastboot in the bootloader message.
    CHECK(ors::fromAospArgs({"--fastboot"}) == std::vector<std::string>{"fastboot"});
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

    // Installing FMRRE permanently: A/B phone without a recovery partition -> boot, stock saved first
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
    std::string twrp = root + "/data/media/0/Download/fmrre-test.img", junk = root + "/data/media/0/Download/junk.img";
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

// The expanded fixture: "TWRP v2 payload " x 512, bytes 0..255 x 16, zeros and a run of 'R', 16 KiB.
// (The project's old name stays here: the bzip2 / xz fixtures above were compressed from exactly this text.)
static std::string fixturePlain() {
    std::string d;
    for (int i = 0; i < 512; i++) d += "TWRP v2 payload ";
    for (int k = 0; k < 16; k++)
        for (int i = 0; i < 256; i++) d.push_back(char(i));
    d += std::string(3000, '\0') + std::string(1000, 'R');
    d.resize(16384, '\0');
    return d;
}

// Stored formats that must never change (backups and settings made before the rename are still used):
// expected values computed independently with Python's hashlib / hmac.
static void testFrozenFormats() {
    uint8_t h[crypt::kHeaderSize] = {'T', 'W', 'V', '2', 'E', 'N', 'C', '1'};
    for (int i = 0; i < 16; i++) h[8 + i] = uint8_t(i);  // salt
    h[24] = 0xe8, h[25] = 0x03;                         // 1000 iterations
    const char* check = "03f4073da437dd14e7bbcacc8b76cfd8";  // HMAC(pbkdf2("pw")[32:], "TWRP v2 backup password check")[:16]
    for (int i = 0; i < 16; i++) h[28 + i] = uint8_t(std::stoi(std::string(check + 2 * i, 2), nullptr, 16));
    std::string err;
    crypt::Stream a, b;
    CHECK(a.beginDecrypt("pw", h, err));
    CHECK(!b.beginDecrypt("px", h, err) && err == "wrong password");
    CHECK(Sha256::hexOf(std::string("1234") + kPinSalt) == "3c8d4d5d0b4aee3a4e6ffa93abc3300e91839309772c1869ad44f4e7946e7a25");
}

static void testCrypt() {
    uint8_t key[32], pt[16] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff}, ct[16];
    for (int i = 0; i < 32; i++) key[i] = uint8_t(i);
    crypt::Aes256(key).encryptBlock(pt, ct);
    CHECK(Sha256::toHex(std::string((const char*)ct, 16)) == "8ea2b7ca516745bfeafc49904b496089");  // FIPS-197 C.3
    uint8_t k1[20];
    memset(k1, 0x0b, 20);
    crypt::HmacSha256 h1(k1, 20);
    h1.update("Hi There", 8);
    CHECK(Sha256::toHex(h1.final()) == "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");  // RFC 4231 #1
    uint8_t dk[40];
    crypt::pbkdf2Sha256("password", (const uint8_t*)"salt", 4, 4096, dk, 40);
    CHECK(Sha256::toHex(std::string((const char*)dk, 40)) ==
          "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134af7ad98c1b458ce3f");
    // Stream round trip in odd-sized pieces, and the tag binds the data.
    crypt::Stream enc;
    std::string hdr;
    CHECK(enc.beginEncrypt("pw", hdr, 2000) && hdr.size() == crypt::kHeaderSize);
    std::string plain(5000, '\0');
    for (size_t i = 0; i < plain.size(); i++) plain[i] = char(i * 13);
    std::string c = plain;
    for (size_t off = 0, n = 1; off < c.size(); off += n, n = n * 3 % 37 + 1) enc.process((uint8_t*)&c[off], std::min(n, c.size() - off));
    std::string tag = enc.tag();
    CHECK(c != plain);
    crypt::Stream dec;
    std::string err;
    CHECK(!crypt::Stream().beginDecrypt("PW", (const uint8_t*)hdr.data(), err) && err == "wrong password");
    CHECK(dec.beginDecrypt("pw", (const uint8_t*)hdr.data(), err));
    std::string d = c;
    dec.process((uint8_t*)&d[0], d.size());
    CHECK(d == plain && dec.tag() == tag);
}

static void testDecoders(const std::string& dir) {
    std::string out, err;
    CHECK(Bunzip2::decode(kBz2Fixture, sizeof kBz2Fixture, out, err));
    CHECK(out == fixturePlain());
    CHECK(Sha256::hexOf(out) == "de8d08c87cc612c30308640fed4ca8225480c3bbc3071f87cdadc67cb173c71f");
    std::vector<unsigned char> bad(kBz2Fixture, kBz2Fixture + sizeof kBz2Fixture);
    bad[300] ^= 0x10;
    CHECK(!Bunzip2::decode(bad.data(), bad.size(), out, err));  // CRC or format error, never garbage
    // Reed-Solomon (libfec parameters): parity makes a valid codeword, one flipped byte doesn't.
    for (uint32_t roots : {2u, 8u}) {
        uint8_t cw[255];
        for (uint32_t i = 0; i < 255 - roots; i++) cw[i] = uint8_t(i * 31 + roots);
        verity::rsEncode(roots, cw, cw + 255 - roots);
        CHECK(verity::rsCheck(roots, cw));
        cw[17] ^= 0x5a;
        CHECK(!verity::rsCheck(roots, cw));
    }
    // dm-verity hash tree + FEC, known answers from an independent implementation (tests/payload_test.py).
    std::string img(3 * 4096 + 4096 + 8192, '\0');
    for (int i = 0; i < 3 * 4096; i++) img[i] = char((i * 7) & 0xff);
    std::string f = dir + "/verity.img";
    CHECK(util::writeFile(f, img));
    int fd = open(f.c_str(), O_RDWR);
    std::string root;
    CHECK(verity::hashTreeSize(3 * 4096, 4096) == 4096 && verity::fecSize(4 * 4096, 2, 4096) == 8192);
    CHECK(verity::buildHashTree(fd, 0, 3 * 4096, 3 * 4096, 4096, 4096, "sha256", "twrp", root, err));
    CHECK(Sha256::toHex(root) == "277d21eea9b022fdfb33a47cfc28ac0a664c7b4893cf4e97f2253588b026ffe4");
    CHECK(verity::encodeFec(fd, 0, 4 * 4096, 4 * 4096, 8192, 2, 4096, err));
    close(fd);
    CHECK(util::readFile(f, img));
    CHECK(Sha256::hexOf(img.substr(3 * 4096, 4096)) == "ac0f55b0b7b0520eab99bb014b631725b3fa6e3e88009cb8f8d5fe44997703c7");
    CHECK(Sha256::hexOf(img.substr(4 * 4096)) == "7059e48a298a302332ad48dc7c9eff639ab5f4111ba6d25eaa40a7925e6723d0");
}

static void testPayload(const std::string& dir) {
    // boot: bzip2 + xz + zero operations; system: data + hash tree + FEC computed on "device".
    std::string plain = fixturePlain(), image = plain + plain + std::string(8192, '\0');
    sim::OtaPart boot;
    boot.name = "boot";
    boot.size = image.size();
    Sha256 h;
    h.update(image.data(), image.size());
    boot.hash = h.digest();
    boot.ops.push_back({payload::kReplaceBz, 0, 4, std::string((const char*)kBz2Fixture, sizeof kBz2Fixture)});
    boot.ops.push_back({payload::kReplaceXz, 4, 4, std::string((const char*)kXzFixture, sizeof kXzFixture)});
    boot.ops.push_back({payload::kZero, 8, 2, ""});
    sim::OtaPart sys;
    std::string sdata(64 * 4096, '\0');
    for (size_t i = 0; i < sdata.size() / 3; i++) sdata[i] = char('a' + i % 19);
    std::string err;
    CHECK(sim::otaPartFromImage("system", sdata, true, dir + "/ota.tmp", sys, err));
    CHECK(sys.treeCount == 1 && sys.fecCount == 2);
    std::string pl = sim::encodePayload({boot, sys}, "main", 1ull << 30, {"system"}, "2026-11-05");
    CHECK(util::writeFile(dir + "/payload.bin", pl));
    payload::Payload p;
    CHECK(payload::open(dir + "/payload.bin", p, err));
    CHECK(p.full() && p.parts.size() == 2 && p.isDynamic("system") && !p.isDynamic("boot"));
    CHECK(p.groups.size() == 1 && p.groups[0].name == "main" && p.securityPatch == "2026-11-05");
    CHECK(p.find("system") && p.find("system")->hashTree.count == 1 && p.find("system")->fecRoots == 2);
    CHECK(runTask("payload-extract", [&](Task& t) { return payload::extract(p, {}, dir + "/pout", true, t); }));
    std::string got;
    CHECK(util::readFile(dir + "/pout/boot.img", got) && got == image);
    CHECK(util::readFile(dir + "/pout/system.img", got) && got.size() == sys.size);
    Sha256 h2;
    h2.update(got.data(), got.size());
    CHECK(h2.digest() == sys.hash);
    // Inside a zip (stored), with metadata; truncated and delta payloads are refused.
    CHECK(sim::writeOtaZip(dir + "/ota.zip", pl, "ota-type=AB\npre-device=husky\n"));
    payload::Payload z;
    CHECK(payload::open(dir + "/ota.zip", z, err) && z.inZip && z.device() == "husky" && z.parts.size() == 2);
    CHECK(payload::isOtaZip(dir + "/ota.zip"));
    CHECK(util::writeFile(dir + "/trunc.bin", pl.substr(0, pl.size() - 100)));
    payload::Payload tr;
    CHECK(!payload::open(dir + "/trunc.bin", tr, err));
    sim::OtaPart delta = boot;
    delta.ops = {{payload::kSourceCopy, 0, 10, ""}};
    CHECK(util::writeFile(dir + "/delta.bin", sim::encodePayload({delta}, "", 0, {}, "")));
    payload::Payload d;
    CHECK(payload::open(dir + "/delta.bin", d, err) && !d.full() && !payload::plan(d, payload::InstallOptions()).ok);
}

static void testRescue() {
    auto has = [](const std::vector<rescue::Finding>& v, rescue::Fix f) {
        for (auto& x : v)
            if (x.fix == f) return true;
        return false;
    };
    auto v = rescue::scan("[ 5.1] device-mapper: verity: 252:1: data block 12345 is corrupted\n", "");
    CHECK(v.size() == 1 && v[0].fix == rescue::Fix::DisableVerity && v[0].severity == 2 && v[0].evidence.find("252:1") != std::string::npos);
    v = rescue::scan("init: Failed to mount required partitions early ...\n", "");
    CHECK(has(v, rescue::Fix::ReflashSystem));
    v = rescue::scan("[ 9.0] Kernel panic - not syncing: Attempted to kill init! exitcode=0x0000000b\n", "kernel_panic");
    CHECK(v.size() == 1 && v[0].fix == rescue::Fix::RestoreBoot);
    v = rescue::scan("init: Service 'zygote' (pid 1012) crashed 4 times\ninit: Received sys.powerctl='reboot,rescueparty'\n", "");
    CHECK(v.size() == 2 && v[0].severity == 2 && has(v, rescue::Fix::DisableModules) && has(v, rescue::Fix::FormatData));
    v = rescue::scan("vold: Failed to unlock user 0\n", "");
    CHECK(has(v, rescue::Fix::FormatData));
    CHECK(rescue::scan("", "watchdog").size() == 1);
    CHECK(rescue::scan("[ 1.0] all good\n", "reboot,shell").empty());
    // pstore pmsg: logcat records, events and noise in between are skipped
    auto rec = [](uint8_t id, uint8_t prio, const std::string& tag, const std::string& msg) {
        std::string body = std::string(1, char(prio)) + tag + '\0' + msg + '\0';
        uint16_t len = uint16_t(18 + body.size());
        std::string h = {'l', char(len & 0xff), char(len >> 8), 0, 0, 0, 0, char(id), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        return h + body;
    };
    std::string pm = std::string("\x00\xffjunk", 6) + rec(0, 4, "Zygote", "info is dropped") + rec(4, 6, "AndroidRuntime", "*** FATAL EXCEPTION IN SYSTEM PROCESS: main") +
                     rec(4, 6, "AndroidRuntime", "java.lang.IllegalStateException: boom\n\tat Foo.bar(Foo.java:1)") + rec(2, 4, "ev", "\x01");
    std::string lc = rescue::decodePmsg(pm);
    CHECK(lc.find("info is dropped") == std::string::npos && lc.find("E/AndroidRuntime: *** FATAL EXCEPTION IN SYSTEM PROCESS") != std::string::npos);
    CHECK(lc.find("E/AndroidRuntime: \tat Foo.bar") != std::string::npos);
    auto f2 = rescue::scan(lc, "");
    CHECK(!f2.empty() && f2[0].evidence.find("IllegalStateException: boom") != std::string::npos);
}

// Auto repair on the simulator (its pstore holds a Rescue Party bootloop, /data has 3 Magisk modules).
static void testAutofix(const std::string& dir) {
    std::string root = dir + "/sim";
    auto find = [](const autofix::Plan& p, const std::string& id) -> const autofix::Step* {
        for (auto& s : p.steps)
            if (s.id == id) return &s;
        return nullptr;
    };
    // Android asked recovery to *offer* a factory reset: never erase on our own.
    CHECK(ors::fromAospArgs({"--prompt_and_wipe_data", "--reason=RescueParty", "--locale=en-US"}) == std::vector<std::string>{"autofix --prompt"});
    CHECK(ors::fromAospArgs({"--wipe_data"}) == std::vector<std::string>{"wipe data"});

    autofix::Plan p = autofix::diagnose();
    CHECK(p.bootFailed && p.headline.find("Rescue Party") != std::string::npos);
    const autofix::Step* mods = find(p, "disable_modules");
    CHECK(mods && mods->recommended && mods->risk == autofix::Risk::Undoable);
    CHECK(find(p, "wipe_caches") && find(p, "wipe_caches")->recommended);
    for (auto& s : p.steps) CHECK(!(s.risk == autofix::Risk::ErasesData && s.recommended));  // never on the first run
    CHECK(!p.checks.empty() && p.checks[0].title.find("boot") != std::string::npos);

    // What Android said counts: /data that won't unlock -> repair the file system, offer Format Data.
    rescue::setAndroidReason("init_user0_failed");
    p = autofix::diagnose();
    CHECK(p.report.androidReason == "init_user0_failed");
    CHECK(find(p, "repair_data_fs") && find(p, "repair_data_fs")->recommended && find(p, "format_data") && !find(p, "format_data")->recommended);
    rescue::setAndroidReason("");

    // The last change was a boot flash and boot is now damaged: restore the stock copy saved before.
    std::string bn = root + "/dev/block/by-name/", stock;
    CHECK(util::readFile(bn + "boot_a", stock));
    std::string sdir = bootimg::backupDir();
    util::mkdirs(sdir);
    CHECK(util::writeFile(sdir + "/boot_a_stock_2026-10.img", stock));
    history::record("Flash image", "boot_a <- custom-kernel.img", true);
    {
        int fd = open((bn + "boot_a").c_str(), O_WRONLY);
        CHECK(fd >= 0 && pwrite(fd, "BROKEN!!", 8, 0) == 8);
        close(fd);
    }
    p = autofix::diagnose();
    const autofix::Step* rb = find(p, "restore_boot");
    CHECK(rb && rb->recommended && util::basename(rb->file) == "boot_a_stock_2026-10.img");
    CHECK(p.lastChange.find("custom-kernel.img") != std::string::npos);
    bool damagedShown = false;
    for (auto& c : p.checks) damagedShown |= c.title == "Boot image (boot_a)" && c.level == 2;
    CHECK(damagedShown);
    autofix::Options o;
    o.only = {"restore_boot"};
    CHECK(runTask("autofix", [&](Task& t) { return autofix::apply(p, o, t); }));
    std::string after;
    CHECK(util::readFile(bn + "boot_a", after) && after == stock);
    auto h = history::recent(3);
    CHECK(h.size() >= 2 && h[0].action == "Auto repair" && h[0].detail.find("[restore_boot]") != std::string::npos);
    CHECK(h[1].detail.find("(by auto repair)") != std::string::npos);  // its own flash isn't "the last change"
    // Next run: escalation - what was tried is not planned again.
    p = autofix::diagnose();
    CHECK(p.lastChange.find("custom-kernel.img") != std::string::npos);
    rb = find(p, "restore_boot");
    CHECK(rb && rb->triedBefore && !rb->recommended);
    CHECK(p.earlierFixes >= 1);
    // After the safe fixes: the system partitions from the newest unencrypted backup ("selftest": boot + data),
    // only with permission, and never /data.
    const autofix::Step* rbk = find(p, "restore_backup");
    CHECK(rbk && rbk->recommended && rbk->risk == autofix::Risk::Replaces && rbk->parts == std::vector<std::string>{"boot"});
    o.only = {"restore_backup"};
    CHECK(!runTask("autofix", [&](Task& t) { return autofix::apply(p, o, t); }));  // not allowed
    o.allowRestore = true;
    CHECK(runTask("autofix", [&](Task& t) { return autofix::apply(p, o, t); }));
    CHECK(history::recent(1)[0].detail.find("[restore_backup]") != std::string::npos);
    // Format Data only with explicit permission.
    o.only = {"format_data"};
    o.allowFormatData = false;
    if (find(p, "format_data")) CHECK(!runTask("autofix", [&](Task& t) { return autofix::apply(p, o, t); }));
    // Unattended runs: at most 3 a day.
    CHECK(autofix::autoRunAllowed());
    for (int i = 0; i < 3; i++) autofix::noteAutoRun();
    CHECK(!autofix::autoRunAllowed());

    // Verified boot stopped the boot: disable verity on an unlocked bootloader - never on a locked one,
    // which refuses vbmeta with verification off (the phone wouldn't start at all).
    CHECK(device::bootloaderUnlocked() == 1);  // the simulator is unlocked (orange)
    std::string ps = root + "/sys/fs/pstore/dmesg-ramoops-1";
    CHECK(util::writeFile(ps, "[ 5.1] device-mapper: verity: 252:1: data block 12345 is corrupted\n"));
    p = autofix::diagnose();
    CHECK(find(p, "disable_verity") && find(p, "disable_verity")->recommended);
    device::setProp("ro.boot.flash.locked", "1");
    device::setProp("ro.boot.verifiedbootstate", "green");
    device::setProp("ro.boot.vbmeta.device_state", "locked");
    std::string how;
    CHECK(device::bootloaderUnlocked(&how) == 0 && how.find("verifiedbootstate=green") != std::string::npos);
    p = autofix::diagnose();
    CHECK(!find(p, "disable_verity"));
    bool advised = false;
    for (auto& a : p.advice) advised |= a.find("bootloader is locked") != std::string::npos;
    CHECK(advised);
    device::setProp("ro.boot.flash.locked", "0");
    device::setProp("ro.boot.verifiedbootstate", "orange");
    device::setProp("ro.boot.vbmeta.device_state", "unlocked");
    unlink(ps.c_str());
}

static void testI18n() {
    i18n::setLanguage("en");
    CHECK(i18n::tr("Back") == "Back");
    i18n::setLanguage("ro");
    CHECK(i18n::language() == "ro");
    CHECK(i18n::tr("Back") == "\xC3\x8Enapoi");                                                    // Înapoi
    CHECK(i18n::tr("Swipe to install to slot B") == "Gliseaz\xC4\x83 pentru a instala \xC3\xAEn slotul B");
    CHECK(i18n::tr("slot details need bootctl") == "slot details need bootctl");                  // {} is one word only
    CHECK(i18n::tr("Flashing My ROM v2.zip") == "Se instaleaz\xC4\x83 My ROM v2.zip");             // {*} is any text
    CHECK(i18n::tr("3 zip(s) queued") == "3 zip-uri \xC3\xAEn coad\xC4\x83");
    CHECK(i18n::tr("boot_a") == "boot_a" && i18n::tr("") == "");
    {
        i18n::NoTranslate raw;
        CHECK(i18n::tr("Back") == "Back");
    }
    CHECK(i18n::longDate(2, 3, 10) == "Mar\xC8\x9Bi, 03 noiembrie");
    CHECK(i18n::upper("Op\xC8\x9Biuni \xC8\x99i \xC4\x83\xC3\xA2\xC3\xAE abc") == "OP\xC8\x9AIUNI \xC8\x98I \xC4\x82\xC3\x82\xC3\x8E ABC");
    CHECK(i18n::upper("\xE2\x80\xA2 \xC5\x82") == "\xE2\x80\xA2 \xC5\x81");  // bullet kept, l-stroke
    // Every entry: a translation, the same placeholder, no conflicting duplicates.
    std::map<std::string, std::string> seen;
    for (auto& e : i18n::entries("ro")) {
        CHECK(!e.second.empty());
        for (const char* ph : {"{}", "{*}"})
            CHECK((e.first.find(ph) == std::string::npos) == (e.second.find(ph) == std::string::npos));
        auto it = seen.find(e.first);
        CHECK(it == seen.end() || it->second == e.second);
        seen[e.first] = e.second;
    }
    i18n::setLanguage("en");
    CHECK(i18n::longDate(2, 3, 10) == "Tuesday, 03 November");
}

int main() {
    char tmpl[] = "/tmp/fmrre-selftest-XXXXXX";
    std::string dir = mkdtemp(tmpl);
    log::init(dir + "/test.log");
    testSha();
    testInstaller(dir);
    testSparse(dir);
    testSuper(dir);
    testImages(dir);
    testAvb(dir);
    testBackupRestore(dir);
    testLegacyFolders();
    testPathcheck(dir);
    testCrypt();
    testFrozenFormats();
    testDecoders(dir);
    testPayload(dir);
    testRescue();
    testAutofix(dir);
    testI18n();
    util::removeTree(dir);
    if (g_fail) {
        fprintf(stderr, "%d check(s) FAILED\n", g_fail);
        return 1;
    }
    printf("All self-tests passed\n");
    return 0;
}
