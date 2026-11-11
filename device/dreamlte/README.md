# FMRRE on the Samsung Galaxy S8 (SM-G950F / G950FD, "dreamlte")

The international **Exynos 8895** S8: **A-only** (a dedicated `RECOVERY` partition), **no fastboot** (Samsung uses
**Download mode** + Odin), **no vbmeta** (it predates AVB 2.0, so nothing to disable), and the bootloader **can be
unlocked** (`F` / `FD` = the global model).

> Nothing here has been run on a physical S8 yet. Every step is the standard Samsung custom-recovery procedure and
> the tools refuse what they can tell is wrong, but treat it as a first test. Flashing a recovery only writes the
> `RECOVERY` partition: Android, your data and the bootloader stay as they are, and `stock-recovery.tar.md5` puts the
> stock recovery back with Odin in a minute. **Flashing anything unofficial trips Knox for good** (Samsung Pay,
> Secure Folder and Samsung Pass stop working, even after going back to stock).

## What you need (Windows)
- **Odin** 3.13+ and Samsung's **USB driver** ("Android USB Driver for Windows").
- **Python 3** - only to build the image: python.org, or `winget install Python.Python.3.12`. (If `python` opens the
  Microsoft Store, use `py` instead.)
- The **FMRRE installer kit**: GitHub > Actions > latest green **build** run > artifact **fmrre-installer-kit**. Unzip
  it: it holds `out\arm64\recovery` (FMRRE for the S8's 64-bit CPU), `pc\`, `tools\`, `device\`.
- The **stock firmware of your phone**, the same version it runs now (Settings > About phone > Software information >
  Build number), from a firmware site or a downloader (Frija, Bifrost, SamFirm). Unzip it: `BL_`, **`AP_`**, `CP_`,
  `CSC_`, `HOME_CSC_` `.tar.md5` files. Keep them all: they are your way back to stock.
- `lz4` is not needed (the builder has its own LZ4, and uses `lz4` for speed when it is installed). `adb` is handy
  once FMRRE runs: `adb shell fmrre ...`.

## 1. Allow custom binaries (OEM unlocking)
1. Sign into a Google **and** a Samsung account, connect to Wi-Fi, and leave the phone online.
2. Settings > About phone > Software information > tap **Build number** 7 times.
3. Settings > Developer options > turn on **OEM unlocking**. On the S8 that is the unlock - there is no separate
   step in Download mode (that came with the S10).
   Missing toggle? That's Samsung's **7-day lock** (VaultKeeper / RMM): stay signed in and online for 7 days, then look
   again. `python pc\fmrre_rescue.py check-unlock` (phone on USB debugging) tells you which case you are in.
4. In Download mode (below) the top-left text should show **OEM LOCK: OFF**. If it shows **RMM STATE: Prenormal** or
   **KG STATE: Prenormal**, the phone still refuses custom binaries ("Only official released binaries are allowed")
   until the 7 days are over - wait, don't flash.

## 2. Build the FMRRE recovery (PowerShell, in the kit folder)
```powershell
cd C:\path\to\fmrre-installer-kit
python pc\fmrre_mkrecovery.py C:\path\to\AP_G950F....tar.md5
```
It checks the AP's MD5 (a minute for ~4 GB), takes `recovery.img.lz4` out of it, puts FMRRE in the stock ramdisk in
place of `/sbin/recovery` (your kernel, device tree and boot header stay exactly as they were), checks the result by
reading it back, and writes:

| File | Use |
|---|---|
| `fmrre-recovery.tar.md5` | Odin, **AP** slot: installs FMRRE (recovery.img only - the S8 has no vbmeta) |
| `stock-recovery.tar.md5` | Odin, **AP** slot: puts the stock recovery back |
| `fmrre-recovery.img` | the raw image (Heimdall on Linux/macOS, or *Install image* from inside FMRRE later) |

Optional: `--device dreamlte` also puts `device\dreamlte\twrp.fstab` in it (names, EFS / CP-EFS / modem backups,
microSD). Without it FMRRE reads the stock recovery's own fstab, whose device paths are right by definition - the
safer choice for the first boot.

## 3. Flash it (Odin)
1. Power off. Hold **Volume Down + Bixby + Power** until the warning screen, then **Volume Up** = Download mode.
2. Connect the USB cable: Odin shows a blue **COM** box.
3. **AP** = `fmrre-recovery.tar.md5`. **Options**: untick **Auto Reboot**. **Start** > **PASS!**
4. Boot **straight into recovery**: hold **Volume Down + Power** until the screen goes black (about 7 s), then at once
   switch to **Volume Up + Bixby + Power** and hold until FMRRE appears. If Android starts instead, it puts the stock
   recovery back on that boot (harmless: flash again).

## 4. First boot - what to check
- FMRRE first asks **Low detail / High detail**: **Low** is the smooth choice on the 2017 SoC (Settings changes it).
- **Advanced > Device info**: Samsung, dreamlte (SM-G950F), A-only, Exynos 8895, Android 9.
- **Mount**: each partition with its block device under `/dev/block/platform/11120000.ufs/by-name/` (capitals).
- **Advanced > Display test** (1440x2960 panel) and **Input test** - if touches land in the wrong place, use its
  touch calibration chips (Swap X/Y, Flip X, Flip Y).
- On the PC: `adb devices` lists the phone as `recovery`; `adb shell fmrre help`; `adb pull /tmp/recovery.log`.
  Send that log if anything misbehaves - Samsung's kernel keeps SELinux enforcing in recovery, so a "Permission
  denied" there is the first thing to look at for this port.
- Don't wipe or format anything on this first test: the point is to see FMRRE run.

## Good to know
- **/data encryption**: when Android encrypted /data (Secure startup, or a forced encryption), FMRRE cannot read it
  - it detects that and keeps backups and settings on the microSD or in RAM. **Format Data** works and leaves the
  encryption footer at the end of the partition free, so Android can encrypt again on its next boot.
- **Android puts the stock recovery back** on its first boot after a flash (`recovery-from-boot.p`). For a test
  that is fine; keeping FMRRE for good needs that file disabled, which FMRRE doesn't do yet.
- **Back to stock**: recovery only - Odin AP = `stock-recovery.tar.md5`. Everything - Odin with BL + AP + CP +
  `HOME_CSC` (keeps your data) or `CSC` (wipes it).
- **On the phone** FMRRE can open the AP itself: `fmrre ap-extract /sdcard/AP_....tar.md5 boot` takes an image out,
  decompressed; *Install > Samsung firmware (AP .tar)* flashes stock images from it.
- **Linux/macOS**: the same builder (`python3 pc/fmrre_mkrecovery.py AP_....tar.md5`), then Heimdall:
  `heimdall flash --RECOVERY fmrre-recovery.img --no-reboot`.
