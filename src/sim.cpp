// Simulator sandbox: a fake device tree on the host so the full recovery can be
// developed and screenshotted without a phone. Never touches real devices.
#include "sim.h"

#include <cstring>
#include <random>
#include <sys/stat.h>

#include "core/log.h"
#include "core/util.h"
#include "sys/super.h"
#include "miniz.h"

#include <fcntl.h>
#include <unistd.h>

namespace tw::sim {

static void randomFile(const std::string& p, size_t size) {
    std::mt19937 rng(1234);
    std::string data(size, '\0');
    for (size_t i = 0; i < size; i += 4) {
        uint32_t v = rng();
        for (int k = 0; k < 4 && i + k < size; k++) data[i + k] = char(v >> (8 * k));
    }
    // Recognisable header for boot images
    if (util::basename(p).find("boot") != std::string::npos) memcpy(&data[0], "ANDROID!", 8);
    util::writeFile(p, data);
}

static void makeZip(const std::string& path, const std::string& script, bool magisk) {
    mz_zip_archive z{};
    if (!mz_zip_writer_init_file(&z, path.c_str(), 0)) return;
    std::string bin = "#!/sbin/sh\n# simulated update-binary\nOUTFD=$2\nui_print() { echo \"ui_print $1\" >&$OUTFD; }\nui_print \"Hello\"\n";
    mz_zip_writer_add_mem(&z, "META-INF/com/google/android/update-binary", bin.data(), bin.size(), MZ_BEST_COMPRESSION);
    mz_zip_writer_add_mem(&z, "META-INF/com/google/android/updater-script", script.data(), script.size(), MZ_BEST_COMPRESSION);
    if (magisk) {
        std::string prop = "id=demo\nname=Demo module\nversion=1.0\n";
        mz_zip_writer_add_mem(&z, "module.prop", prop.data(), prop.size(), MZ_BEST_COMPRESSION);
    }
    std::string filler(300000, 'x');
    for (size_t i = 0; i < filler.size(); i++) filler[i] = char('a' + (i * 7919) % 26);
    mz_zip_writer_add_mem(&z, "system/payload.dat", filler.data(), filler.size(), MZ_BEST_COMPRESSION);
    mz_zip_writer_finalize_archive(&z);
    mz_zip_writer_end(&z);
}

// Android sparse image whose expanded content starts with an ext4 superblock.
static std::string sparseExt4(uint64_t expanded, const std::string& label) {
    const uint32_t blk = 4096;
    uint32_t total = uint32_t(expanded / blk);
    std::string out;
    auto u16 = [&](uint16_t v) { out.append((const char*)&v, 2); };
    auto u32 = [&](uint32_t v) { out.append((const char*)&v, 4); };
    const uint32_t dataBlocks = 64;  // 256 KiB of content after the superblock
    u32(0xED26FF3A); u16(1); u16(0); u16(28); u16(12);
    u32(blk); u32(total); u32(3); u32(0);
    // chunk 1: raw, superblock + content
    u16(0xCAC1); u16(0); u32(1 + dataBlocks); u32(12 + (1 + dataBlocks) * blk);
    std::string sb(blk, '\0');
    sb[1080] = 0x53; sb[1081] = (char)0xEF;
    memcpy(&sb[1144], label.data(), std::min<size_t>(label.size(), 16));
    out += sb;
    std::mt19937 rng(42);
    for (uint32_t i = 0; i < dataBlocks * blk; i++) out.push_back(char('A' + rng() % 8));
    // chunk 2: don't care up to the last block
    u16(0xCAC3); u16(0); u32(total - 2 - dataBlocks); u32(12);
    // chunk 3: fill the last block with a pattern
    u16(0xCAC2); u16(0); u32(1); u32(16); u32(0x54575632);  // "2VWT"
    return out;
}

static void writeGzip(const std::string& path, const std::string& data) {
    size_t clen = 0;
    void* c = tdefl_compress_mem_to_heap(data.data(), data.size(), &clen, TDEFL_DEFAULT_MAX_PROBES);
    if (!c) return;
    std::string out("\x1f\x8b\x08\0\0\0\0\0\0\x03", 10);
    out.append((const char*)c, clen);
    mz_free(c);
    uint32_t crc = (uint32_t)mz_crc32(MZ_CRC32_INIT, (const unsigned char*)data.data(), data.size()), sz = (uint32_t)data.size();
    out.append((const char*)&crc, 4);
    out.append((const char*)&sz, 4);
    util::writeFile(path, out);
}

// A Pixel-like virtual A/B super: 8 GiB, _b partitions empty, ~2.5 GiB free.
static void ensureSuper(const std::string& root) {
    std::string superDev = root + "/dev/block/by-name/super";
    lp::Metadata probe;
    std::string err;
    if (lp::read(superDev, 0, probe, err)) return;
    LOGI("sim: creating dynamic partitions (super)");
    const uint64_t superSize = 8ull << 30;
    int fd = open(superDev.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    if (ftruncate(fd, (off_t)superSize) != 0) { close(fd); return; }
    close(fd);
    lp::Metadata m;
    m.slotCount = 3;
    m.maxSize = 65536;
    m.minor = 2;
    m.headerSize = 256;
    m.headerFlags = lp::kHeaderFlagVirtualAB;
    m.devs.push_back({2048, 1 << 20, 0, superSize, "super", 0});
    m.groups.push_back({"default", 0, 0});
    m.groups.push_back({"google_dynamic_partitions_a", 0, superSize - (16ull << 20)});
    m.groups.push_back({"google_dynamic_partitions_b", 0, superSize - (16ull << 20)});
    struct P { const char* n; uint64_t mb; };
    P layout[] = {{"system", 1840}, {"system_ext", 420}, {"product", 2650}, {"vendor", 610}, {"vendor_dlkm", 42}, {"odm", 8}};
    for (const char* s : {"_a", "_b"})
        for (auto& p : layout) {
            std::string e;
            m.add(std::string(p.n) + s, std::string("google_dynamic_partitions") + s, lp::kAttrReadonly, e);
            if (std::string(s) == "_a") m.resize(std::string(p.n) + s, p.mb << 20, e);
        }
    if (!lp::format(superDev, m, err)) { LOGE("sim: super: %s", err.c_str()); return; }
    for (auto& p : m.parts) {
        std::string f = root + "/dev/block/mapper/" + p.name;
        if (!p.bytes()) { unlink(f.c_str()); continue; }
        int pf = open(f.c_str(), O_RDWR | O_CREAT, 0644);
        if (pf >= 0) { if (ftruncate(pf, (off_t)p.bytes()) != 0) {} close(pf); }
    }
    // Real AVB headers in vbmeta so the verified-boot tools have something to patch.
    for (auto v : {"vbmeta_a", "vbmeta_b"}) {
        std::string vb(4096, '\0');
        memcpy(&vb[0], "AVB0", 4);
        vb[7] = 1;
        memcpy(&vb[128], "avbtool 1.3.0", 13);
        util::writeFile(root + "/dev/block/by-name/" + v, vb);
    }
    std::string dl = root + "/data/media/0/Download/";
    // Boot images with real v4 headers (Pixel 8: ramdisk in init_boot, GKI kernel in boot).
    auto bootHdr = [&](const std::string& f, uint32_t kernel, uint32_t ramdisk) {
        std::string b(4 << 20, '\0');
        memcpy(&b[0], "ANDROID!", 8);
        auto put = [&](size_t o, uint32_t v) { memcpy(&b[o], &v, 4); };
        put(8, kernel); put(12, ramdisk);
        put(16, ((15u << 14) << 11) | ((26u << 4) | 10u));  // Android 15, 2026-10
        put(20, 4096); put(40, 4);
        util::writeFile(f, b);
    };
    for (auto s2 : {"_a", "_b"}) {
        bootHdr(root + "/dev/block/by-name/boot" + std::string(s2), 3 << 20, 0);
        bootHdr(root + "/dev/block/by-name/init_boot" + std::string(s2), 0, 900 << 10);
    }
    util::mkdirs(root + "/data/adb/magisk");
    util::writeFile(root + "/data/adb/magisk/util_functions.sh", "MAGISK_VER='29.0'\nMAGISK_VER_CODE=29000\n");
    struct M { const char* id; const char* prop; };
    M mods[] = {{"playintegrityfix", "id=playintegrityfix\nname=Play Integrity Fix\nversion=v18.7\nauthor=chiteroman\n"},
                {"zygisk_lsposed", "id=zygisk_lsposed\nname=LSPosed\nversion=v1.10.1\nauthor=LSPosed\n"},
                {"bootloop_demo", "id=bootloop_demo\nname=Font changer (demo)\nversion=2.1\nauthor=someone\n"}};
    for (auto& m : mods) {
        util::mkdirs(root + "/data/adb/modules/" + m.id);
        util::writeFile(root + "/data/adb/modules/" + m.id + "/module.prop", m.prop);
    }
    bootHdr(dl + "twrpv2-recovery.img", 3 << 20, 1 << 20);  // as built by scripts/make_recovery_img.sh from boot.img
    util::writeFile(dl + "magisk_patched-29000_Xy12z.img", "");
    bootHdr(dl + "magisk_patched-29000_Xy12z.img", 0, 950 << 10);
    // Demo GSIs as they are actually downloaded.
    std::string gsi = sparseExt4(3300ull << 20, "/");
    writeGzip(dl + "lineage-22.1-arm64_bvN-vndklite.img.gz", gsi);
    std::string raw = dl + "system-td-arm64-ab-vanilla.img";
    util::writeFile(raw, gsi);
    // Desktop only: use the host's xz to produce the .img.xz most GSIs ship as (keeps the raw .img otherwise).
    if (util::haveBinary("xz") && util::run("xz -T0 -f " + util::shellQuote(raw)) == 0) LOGI("sim: created %s.xz", raw.c_str());
    // A GSI saved with a name the image picker can't recognise - the "can't find my file" case.
    util::writeFile(dl + "GSI_A15_download(1)", gsi);
    util::writeFile(dl + "pixel-gsi.img.crdownload", std::string(4096, '\0'));
}

// A LineageOS-style A/B ROM zip (payload.bin) built for this fake device.
static void ensureOta(const std::string& root) {
    std::string zip = root + "/data/media/0/Download/lineage-22.2-20261101-nightly-husky-signed.zip";
    if (util::exists(zip)) return;
    LOGI("sim: creating a demo A/B OTA (payload.bin)");
    std::mt19937 rng(2026);
    auto image = [&](size_t size, const char* magic, size_t magicAt, size_t randomBytes) {
        std::string d(size, '\0');
        for (size_t i = 4096; i < 4096 + randomBytes && i < size; i++) d[i] = char('A' + rng() % 23);
        if (magic) memcpy(&d[magicAt], magic, strlen(magic));
        return d;
    };
    struct Spec { const char* name; size_t size; const char* magic; size_t at; size_t rnd; bool verity; };
    const Spec specs[] = {
        {"boot", 1 << 20, "ANDROID!", 0, 256 << 10, false},
        {"init_boot", 1 << 20, "ANDROID!", 0, 200 << 10, false},
        {"vendor_boot", 2 << 20, "VNDRBOOT", 0, 384 << 10, false},
        {"dtbo", 512 << 10, "\xd7\xb7\xab\x1e", 0, 64 << 10, false},
        {"vbmeta", 4096, "AVB0", 0, 0, false},
        {"system", 24 << 20, "\x53\xef", 1080, 1 << 20, true},
        {"system_ext", 4 << 20, "\x53\xef", 1080, 128 << 10, false},
        {"product", 8 << 20, "\x53\xef", 1080, 256 << 10, false},
        {"vendor", 8 << 20, "\x53\xef", 1080, 512 << 10, true},
        {"vendor_dlkm", 1 << 20, "\x53\xef", 1080, 64 << 10, false},
        {"odm", 512 << 10, "\x53\xef", 1080, 16 << 10, false},
    };
    std::vector<OtaPart> parts;
    std::vector<std::string> dynamic;
    for (auto& sp : specs) {
        OtaPart p;
        std::string err;
        if (!otaPartFromImage(sp.name, image(sp.size, sp.magic, sp.at, sp.rnd), sp.verity, root + "/tmp/ota_part.tmp", p, err)) {
            LOGE("sim: OTA %s: %s", sp.name, err.c_str());
            return;
        }
        parts.push_back(std::move(p));
        if (sp.magic && sp.at == 1080) dynamic.push_back(sp.name);
    }
    std::string pl = encodePayload(parts, "google_dynamic_partitions", (8ull << 30) - (16ull << 20), dynamic, "2026-11-05");
    std::string meta = "ota-type=AB\npost-build=google/lineage_husky/husky:15/BP1A.250505.005/eng.20261101:userdebug/release-keys\n"
                       "post-sdk-level=35\npost-security-patch-level=2026-11-05\npost-timestamp=1793500000\npre-device=husky\n";
    if (!writeOtaZip(zip, pl, meta)) LOGE("sim: cannot write %s", zip.c_str());
}

void ensureRoot(const std::string& root) {
    if (util::exists(root + "/sim.prop")) { ensureSuper(root); ensureOta(root); return; }
    LOGI("sim: creating sandbox in %s", root.c_str());
    for (auto d : {"/data/media/0/Download", "/data/media/0/TWRP", "/data/media/0/Pictures", "/data/app", "/data/data/com.android.settings",
                   "/data/dalvik-cache/arm64", "/data/system", "/cache/recovery", "/system_root/system/app", "/vendor/etc",
                   "/product/app", "/metadata", "/persist", "/dev/block/by-name", "/dev/block/mapper", "/etc", "/tmp",
                   "/external_sd/Music"})
        util::mkdirs(root + d);
    util::writeFile(root + "/sim.prop",
                    "ro.product.model=Pixel 8 Pro\nro.product.device=husky\nro.product.brand=google\n"
                    "ro.product.manufacturer=Google\nro.build.version.release=15\nro.build.version.sdk=35\n"
                    "ro.boot.slot_suffix=_a\nro.serialno=1A2B3C4D5E\nro.build.version.security_patch=2026-10-05\n"
                    "ro.board.platform=zuma\nro.build.fingerprint=google/husky/husky:15/AP4A/12345:user/release-keys\n");
    util::writeFile(root + "/etc/recovery.fstab",
                    "# TWRP v2 fstab (simulator)\n"
                    "/system_root  ext4   system       flags=display=\"System\";logical;slotselect;backup=1\n"
                    "/vendor       ext4   vendor       flags=display=\"Vendor\";logical;slotselect;backup=1\n"
                    "/product      ext4   product      flags=display=\"Product\";logical;slotselect\n"
                    "/system_ext   ext4   system_ext   flags=display=\"System Ext\";logical;slotselect\n"
                    "/metadata     ext4   /dev/block/by-name/metadata  flags=display=\"Metadata\"\n"
                    "/data         f2fs   /dev/block/by-name/userdata  flags=display=\"Data\";backup=1;wipeingui;storage\n"
                    "/cache        ext4   /dev/block/by-name/cache     flags=display=\"Cache\";wipeingui\n"
                    "/persist      ext4   /dev/block/by-name/persist   flags=display=\"Persist\";backup=1\n"
                    "/boot         emmc   /dev/block/by-name/boot      flags=display=\"Boot\";slotselect;backup=1;flashimg=1\n"
                    "/init_boot    emmc   /dev/block/by-name/init_boot flags=display=\"Init Boot\";slotselect;backup=1;flashimg=1\n"
                    "/vendor_boot  emmc   /dev/block/by-name/vendor_boot flags=display=\"Vendor Boot\";slotselect;backup=1;flashimg=1\n"
                    "/dtbo         emmc   /dev/block/by-name/dtbo      flags=display=\"DTBO\";slotselect;backup=1;flashimg=1\n"
                    "/vbmeta       emmc   /dev/block/by-name/vbmeta    flags=display=\"VBMeta\";slotselect;flashimg=1\n"
                    "/external_sd  vfat   /dev/block/mmcblk1p1         flags=display=\"MicroSD\";storage;wipeingui;removable\n");
    std::string bn = root + "/dev/block/by-name/";
    for (auto p : {"boot_a", "boot_b", "init_boot_a", "init_boot_b", "dtbo_a", "dtbo_b", "vbmeta_a", "vbmeta_b"})
        randomFile(bn + p, 1 << 20);
    randomFile(bn + "vendor_boot_a", 2 << 20);
    randomFile(bn + "vendor_boot_b", 2 << 20);
    for (auto p : {"userdata", "cache", "metadata", "persist", "super"}) util::writeFile(bn + p, "");
    for (auto p : {"system_a", "vendor_a", "product_a"}) util::writeFile(root + "/dev/block/mapper/" + p, "");
    util::mkdirs(root + "/dev/block");
    util::writeFile(root + "/dev/block/mmcblk1p1", "");
    // Some content to back up / browse
    util::writeFile(root + "/system_root/system/build.prop", "ro.build.display.id=AP4A.261005\n");
    util::writeFile(root + "/vendor/etc/fstab.zuma", "# vendor fstab\n");
    for (int i = 0; i < 12; i++) {
        std::string d = root + "/data/data/com.example.app" + std::to_string(i);
        util::mkdirs(d + "/databases");
        util::writeFile(d + "/databases/main.db", std::string(20000 + i * 3000, char('a' + i)));
    }
    util::writeFile(root + "/data/system/packages.xml", "<packages/>\n");
    randomFile(root + "/data/dalvik-cache/arm64/system@framework@boot.art", 400000);
    makeZip(root + "/data/media/0/Download/LineageOS-22.1-20261028-nightly-husky.zip",
            "ui_print(\"Installing LineageOS 22.1\");\nui_print(\"Target: google/husky\");\n"
            "ui_print(\"Patching system image unconditionally...\");\nui_print(\"Patching vendor image...\");\n"
            "ui_print(\"Flashing boot, dtbo, vendor_boot...\");\nui_print(\"Script succeeded\");\n",
            false);
    makeZip(root + "/data/media/0/Download/Magisk-v29.0.zip",
            "ui_print(\"- Magisk 29.0 installer\");\nui_print(\"- Target image: /dev/block/by-name/init_boot_a\");\n"
            "ui_print(\"- Patching ramdisk\");\nui_print(\"- Flashing new boot image\");\nui_print(\"- Done\");\n",
            false);
    makeZip(root + "/data/media/0/Download/NikGapps-core-15.zip",
            "ui_print(\"NikGapps core for Android 15\");\nui_print(\"Installing 12 packages\");\nui_print(\"Done\");\n", false);
    randomFile(root + "/data/media/0/Download/twrpv2-husky.img", 3 << 20);
    randomFile(root + "/data/media/0/Download/kernel-boot.img", 2 << 20);
    util::writeFile(root + "/data/media/0/Download/notes.txt",
                    "TWRP v2 simulator\n\nThis sandbox mimics an Android device.\nEverything you do here stays inside this folder.\n");
    util::writeFile(root + "/external_sd/Music/song.mp3", std::string(500000, '\x11'));
    ensureSuper(root);
    ensureOta(root);
}

}  // namespace tw::sim
