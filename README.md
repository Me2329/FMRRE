# TWRP v2

**A modern, native Android custom recovery, rebuilt from scratch with a fresh interface and many more features.**

TWRP v2 is a real recovery environment: a single static binary that `init` starts from the recovery
ramdisk. It draws directly to the display (DRM/KMS or fbdev), reads the touchscreen and buttons from
`/dev/input`, and does the work itself: it flashes zips and images, makes backups, wipes, mounts and more.
It does not need Android, a framework or a GPU.

![Overview](docs/screenshots/overview.png)
![Tools](docs/screenshots/tools.png)
![Themes](docs/screenshots/themes.png)
![GSI wizard, Find & flash, dd command, Root](docs/screenshots/gsi_root.png)

> **Status: experimental - NOT tested on a real phone yet.** Everything here builds (host, static arm64 and
> armv7), runs in the desktop simulator and passes the host self-tests (real `update-binary` protocol, sparse
> and compressed images, super metadata rewrite, backup → wipe → restore, OpenRecoveryScript). The features that
> touch `super`, `vbmeta`, `misc` and device-mapper follow the AOSP on-disk formats exactly and are covered by
> tests, but they have **never run on hardware**. Always try it with `fastboot boot` first, keep a working
> recovery, and keep the super/vbmeta/boot backups TWRP v2 makes automatically.

---

## What's new compared to classic TWRP

### Interface
- **Material-style UI, rendered in software.** Anti-aliased rounded cards, soft shadows, glows,
  ripples, a floating nav dock and animated page transitions, with no GPU needed. A full 1080×2400 frame takes
  about 8–16 ms on one desktop-class CPU core. The screen is only redrawn while something moves, so the
  recovery uses no CPU when idle.
- **4 background styles and 8 accent colours:** Midnight, AMOLED (pure black), Aurora (animated glow)
  and Light.
- **Crisp at any density:** TrueType fonts (Inter plus DejaVu Sans Mono) and a vector icon set rasterised
  from signed-distance shapes. There are no bitmap themes to port per resolution.
- **Swipe-to-confirm** on every destructive action, plus a **typed "yes"** to format data.
- **Kinetic scrolling** with overscroll bounce, long-press, multi-select, an on-screen keyboard with key
  previews, toasts, bottom sheets and dialogs.
- **Live status bar:** clock, CPU temperature and battery level/charging.
- **Console drawer:** the full colour-coded log is one tap away, with a button to save it to storage.
- **Hardware-key navigation:** Vol+/Vol− move focus, Power selects. It works even with a broken touchscreen.
- **PIN lock** on boot and when the screen turns off, **screen timeout** (dim, then blank), haptics,
  a brightness slider, 180° rotation and a **notch offset**.
- **Screenshots** with Vol− + Power, the camera icon, or Advanced → Take screenshot.

### Recovery features
| Area | Features |
|---|---|
| **Install** | Zip **queue**, CRC **integrity check** before flashing, real `update-binary` protocol (`ui_print`, `progress`, `set_progress`, `wipe_cache`), zip type detection (Edify, script, Magisk/KSU module, A/B OTA), optional cache/ART wipe and auto-reboot afterwards |
| **ADB sideload** | Receive a package over `adb sideload` without copying it to storage first. Spawns `minadbd` when the ramdisk has it, or falls back to switching the USB function to `sideload` and watching the drop paths; then flashes with the same verified installer pipeline |
| **Images** | Flash **raw and Android sparse** images that are also **`.xz`, `.gz`, `.zst`, `.lz4` or inside a `.zip`** - streamed, no temp files (xz decoder built in). Target picked from the image *content* (boot / init_boot / vendor_boot / vbmeta / dtbo / super / system), fit check, optional archive pre-check, **SHA-256 read-back verify**, logical partitions remapped read-write (the reason plain `dd` fails on `system_a`) |
| **GSI wizard** | Pick a GSI in any format → checks it fits → **grows `system_a` inside super** (optionally frees the other slot or deletes `product_a`) → streams it → **disables AVB in vbmeta** → optional factory reset. Refuses while a Virtual A/B update is merging |
| **Super partitions** | Reads and rewrites the LP metadata like fastbootd: usage map, groups, **resize / delete / create** logical partitions, free the inactive slot, map read-write, **automatic metadata backup + restore** before every change (written to all slots, backup copy first, verified by read-back) |
| **Find & flash a file** | "Can't find your file?": type a path (`/sdcard/...` is mapped to recovery's real path), get a **diagnosis** (wrong folder, typo with fuzzy suggestions, unfinished download, encrypted or unmounted storage, missing extension - files are recognised by their content) and a **ready-to-run command** (`dd`, `xz -dc \| dd`, `simg2img` for sparse, `blockdev --setrw` for logical partitions) to open in the terminal, save as a script, or flash with the built-in streamer |
| **Verified boot** | Show and change vbmeta flags for both slots (`--disable-verity --disable-verification` equivalent), original header saved first; flashing a `vbmeta.img` can patch it on the fly |
| **Root** | Detects Magisk / KernelSU / APatch and whether the ramdisk is in `boot` or `init_boot`; installs from zip/APK or flashes a `magisk_patched` image to the right partition **after an automatic stock backup**; one-tap unroot; **module manager** (enable, disable, remove, "bootloop? disable all") |
| **Automation** | **OpenRecoveryScript** (`/cache/recovery/openrecoveryscript`) with a cancellable countdown, AOSP `/cache/recovery/command` and the **bootloader message in `misc`** (read and cleared, no recovery boot loops), and a **`twrp` command line tool** for `adb shell` (see below) |
| **Wipe** | Factory reset (keeps internal storage), Advanced wipe checklist, **Format Data** (mkfs + metadata), ART/Dalvik wipe |
| **Backup** | Streaming tar/raw backups with **gzip compression**, **SHA-256 digests** (written TWRP-style as `.sha2` too), size estimate, free-space warning, storage picker (internal / MicroSD / USB OTG), throughput and ETA |
| **Restore** | Reads TWRP v2 and **classic TWRP** backups, per-partition selection, **verify before restore** (refuses corrupt backups), verify-only and delete |
| **Mount** | Toggle each partition with live usage bars, read-only system by default (dm-verity safe), unmount all, **USB OTG auto-detect** and rescan, **exFAT / NTFS** via kernel drivers (exfat, sdfat, ntfs3) or FUSE helpers |
| **File systems** | Check & repair (e2fsck, fsck.f2fs, fsck.fat, fsck.exfat, ntfsfix), grow to fill the partition (resize2fs, resize.f2fs), change file system, info - long-press a partition in Mount |
| **Files** | Full file manager: sort, hidden files, quick locations, **multi-select**, copy/move/paste, rename, chmod, delete, SHA-256, text and **hex viewer**, install/flash straight from a file, "flash as GSI", "dd command for this file". Pickers show **how many files the type filter hides** and have an **All files** switch |
| **Terminal** | Root shell with on-screen keyboard, history, quick-command chips, Ctrl-C |
| **Advanced** | **System monitor** (CPU graph, per-core frequency, RAM, battery details, every thermal zone), partition info, **input and touch tester**, copy log, fix contexts, restart ADB, A/B **slot switching** |
| **Reboot** | System, Recovery, Bootloader, **Fastbootd**, **Download** (Samsung), **EDL** (Qualcomm), Power off |
| **Devices** | DRM/KMS and fbdev (32 bpp in any channel order, RGB565), multitouch protocol A/B and single touch, touch calibration (swap/flip), A/B slots, **dynamic/logical partitions** (`/dev/block/mapper`), reads **TWRP or AOSP fstabs** |

---

## Project layout

```
src/
  core/       logging, utilities, SHA-256, settings, background task runner
  gfx/        software canvas (SDF anti-aliasing), fonts (stb_truetype), vector icons, PNG
  platform/   DRM/KMS + fbdev + simulator displays, evdev input, device info/power/battery
  sys/        partitions & fstab, installer (zip/update-binary), image streaming (sparse/xz/gz/zip),
              super (LP metadata + device-mapper), vbmeta, boot images & root, path diagnostics,
              OpenRecoveryScript + twrp CLI, sideload, backup engine
  ui/         immediate-mode widget toolkit, app shell (navigation, dialogs, console, lock)
  ui/screens/ home, install, GSI wizard, super, vbmeta, find & flash, root, fs tools, wipe,
              backup/restore, mount, files, terminal, advanced, settings, reboot, ORS
  sim.cpp     fake device sandbox for desktop development
tests/        host self-tests
device/       example twrp.fstab and device-tree snippet
scripts/      build, make a recovery image, try on a device
```

---

## Building

### Android binary (no NDK needed)
TWRP v2 cross-compiles to a **fully static** binary with [Zig](https://ziglang.org/), so it runs from any
recovery ramdisk regardless of the bionic version.

```bash
pip install ziglang
```

```bash
scripts/build.sh arm64
```

This produces `out/arm64/recovery` (about 1.3 MB). Use `scripts/build.sh arm` for 32-bit armv7 devices.

### With the Android NDK
```bash
ANDROID_NDK=/path/to/ndk scripts/build.sh ndk
```

### Inside an AOSP / LineageOS / TWRP tree
Put this repo anywhere in the tree (for example `bootable/twrpv2`) and add the module from `Android.bp`:
```make
PRODUCT_PACKAGES += twrpv2_recovery
```
It installs as `/system/bin/recovery` in the recovery ramdisk and overrides the stock recovery.
See `device/example/` for an fstab and a device-tree snippet.

---

## Putting it on a phone

**1. Try it without flashing anything.** This works if you're already in a recovery with root adb (TWRP,
OrangeFox, LineageOS recovery):
```bash
scripts/try_on_device.sh out/arm64/recovery
```

**2. Build a bootable image from your current recovery** by swapping the binary in its ramdisk
(needs `magiskboot`, which you can take from the Magisk APK):
```bash
scripts/make_recovery_img.sh recovery.img out/arm64/recovery device/example/twrp.fstab
```
```bash
fastboot boot twrpv2-recovery.img
```
Only flash it once `fastboot boot` works.

**3. Build it into a device tree** as described above.

### fstab
TWRP v2 looks for `/etc/twrp.fstab`, `/etc/recovery.fstab`, `/system/etc/recovery.fstab` and finally
`/vendor/etc/fstab.<hardware>`. Both the TWRP format and the AOSP format work, and partitions such as boot,
init_boot, vendor_boot and dtbo are added automatically from `/dev/block/by-name`. See
`device/example/twrp.fstab` for every supported flag.

### Runtime requirements in the ramdisk
`sh`, `tar`, `getprop`/`setprop` (toybox) and, for formatting, `mke2fs` and/or `make_f2fs`. These are
present in practically every recovery ramdisk.

---

## Desktop simulator

Develop and test the whole UI on Linux, against a sandboxed fake device (Pixel 8 Pro, A/B, dynamic
partitions, demo zips and images). Nothing outside the sandbox folder is touched, and zips are installed
as a dry run.

```bash
scripts/build.sh sim
```
```bash
out/sim/twrpv2 --sim
```

In the window, the mouse acts as touch, ↑/↓ are Vol+/Vol−, Enter is Power, Esc is Back and F1 is Home.

**Headless automation** (used for the screenshots and in CI):
```bash
out/sim/twrpv2 --sim --headless --script tools/scripts/tour.txt --out shots/
```
Script commands: `tap X Y`, `longpress X Y`, `swipe X1 Y1 X2 Y2 MS`, `wait MS`, `waittask`,
`key back|home|volup|voldown|power`, `text ...`, `shot file.png`.

**Self-tests:**
```bash
build-host/twrpv2_selftest
```

---

## Command line (`twrp`)

From a computer, over `adb shell` while TWRP v2 runs (the UI shows progress), or from the recovery terminal:
```bash
adb shell /system/bin/recovery twrp gsi /sdcard/Download/system-td-arm64-ab-vanilla.img.xz
```
```bash
adb shell /system/bin/recovery twrp check /sdcard/Download/system.img
```
`ln -s /system/bin/recovery /sbin/twrp` lets you just type `twrp ...`. Commands (the same ones work in
OpenRecoveryScript files): `install`, `flash <image> <partition>`, `gsi`, `wipe`, `format data`, `backup`,
`restore`, `mount`, `unmount`, `mkdir`, `cmd`, `sideload`, `lp list|resize|delete|free-other-slot`,
`vbmeta disable|enable|status`, `root status|backup|modules-off`, `check <path>`, `dd <image> <partition>`,
`print`, `sleep`, `reboot [target]`. `twrp help` lists them.

In the simulator: `out/sim/twrpv2 --sim twrp lp list`.

---

## Known limitations (honest list)
- **No decryption** of FBE/FDE `/data`. When data is encrypted, TWRP v2 detects it and keeps backups and
  settings off `/data`; **Format Data** still works.
- **No MTP or `payload.bin` OTA flashing** yet. Use **ADB sideload** or flash zips from storage.
- **Super metadata editing has only been tested on images, not on a phone.** Retrofit dynamic partitions (super
  split across `system_a`/`system_b`) are read-only on purpose; use fastbootd there.
- `.zst` and `.lz4` images need the `zstd` / `lz4` tools in the ramdisk (`.xz`, `.gz` and `.zip` are built in).
- Root: TWRP v2 installs and manages Magisk / KernelSU / APatch; it has no `su` of its own.
- ADB sideload needs `minadbd` in the ramdisk for the real sideload protocol; without it TWRP v2 flips USB to the `sideload` function and watches the standard drop paths instead.
- Zip flashing checks integrity (CRC) but not OTA **signatures**.
- SELinux labels are preserved only if the ramdisk's `tar` supports it (`--selinux` or `--xattrs`).
  Otherwise use *Advanced → Fix storage contexts*.
- Download and EDL reboot targets only work where the bootloader supports them.

---

## Credits & licences
TWRP v2 is an independent project inspired by the original TWRP (Team Win Recovery Project); its code is
written from scratch. Licensed under the **Apache License 2.0** (see `LICENSE`).
Third-party components: [Inter](https://rsms.me/inter/) (SIL OFL 1.1), DejaVu Sans Mono (Bitstream Vera
licence), [stb_truetype](https://github.com/nothings/stb) (public domain), [miniz](https://github.com/richgel999/miniz) (MIT),
[XZ Embedded](https://github.com/tukaani-project/xz-embedded) (0BSD).
Font licences are in `res/fonts/`.
