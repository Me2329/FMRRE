// Simulator sandbox: a fake device tree on the host so the full recovery can be
// developed and screenshotted without a phone. Never touches real devices.
#include "sim.h"

#include <cstring>
#include <random>
#include <sys/stat.h>

#include "core/log.h"
#include "core/util.h"
#include "miniz.h"

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

void ensureRoot(const std::string& root) {
    if (util::exists(root + "/sim.prop")) return;
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
}

}  // namespace tw::sim
