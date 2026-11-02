// GSI wizard: everything `fastboot flash system gsi.img` + fastbootd + vbmeta does, in one screen.
#include "ui/screens/screens.h"

#include <unistd.h>

#include "core/settings.h"
#include "core/util.h"
#include "platform/device.h"
#include "sys/avb.h"
#include "sys/imagesrc.h"
#include "sys/partitions.h"
#include "sys/pathcheck.h"
#include "sys/super.h"

namespace tw {

namespace {

struct Plan {
    bool dynamic = false, feasible = false;
    std::string sysName, dev;  // system_a, its block device
    uint64_t cur = 0, need = 0, grow = 0, room = 0;
    uint64_t otherSlotBytes = 0, productBytes = 0;
    std::string why;
};

class GsiScreen : public Screen {
public:
    explicit GsiScreen(const std::string& img) : img_(img) {
        Settings& s = Settings::get();
        wipeData_ = s.flag("gsi_wipe", true);
        disableAvb_ = s.flag("gsi_avb", true);
        verify_ = s.flag("img_verify", true);
        reload();
    }
    std::string title() const override { return "GSI wizard"; }
    std::string subtitle() const override { return img_.empty() ? "Choose a system image" : util::basename(img_); }
    void onShow(App&) override { reload(); }

    void reload() {
        info_ = img_.empty() ? img::Info() : img::inspect(img_, entry_);
        entry_ = info_.zipEntry;
        haveMeta_ = lp::read(lp::superDevice(), lp::currentSlotIndex(), meta_, metaErr_);
        busy_ = lp::busy(busyWhy_);
        avb_ = avb::read(avb::vbmetaDevice(device::info().slot));
        plan();
    }

    void plan() {
        Plan p;
        const std::string slot = device::info().slot;
        p.sysName = "system" + slot;
        p.need = info_.imageSize;
        if (haveMeta_ && meta_.find(p.sysName)) {
            p.dynamic = true;
            p.cur = meta_.find(p.sysName)->bytes();
            std::string other = slot == "_b" ? "_a" : "_b";
            for (auto& part : meta_.parts)
                if (!slot.empty() && util::endsWith(part.name, other)) p.otherSlotBytes += part.bytes();
            if (auto* pr = meta_.find("product" + slot)) p.productBytes = pr->bytes();
            lp::Metadata sim = meta_;
            std::string e;
            if (freeOther_)
                for (auto& part : sim.parts)
                    if (util::endsWith(part.name, other)) part.extents.clear();
            if (deleteProduct_) sim.remove("product" + slot, e);
            p.grow = p.need > p.cur ? p.need - p.cur : 0;
            p.room = sim.maxGrowBytes(p.sysName);
            p.feasible = p.need && p.grow <= p.room;
            p.dev = lp::mappedPath(p.sysName);
            if (p.dev.empty()) p.dev = device::path("/dev/block/mapper/" + p.sysName);
            if (!p.feasible && p.need) p.why = "Needs " + util::humanSize(p.grow - std::min(p.grow, p.room)) + " more space in super";
        } else {
            Partition* sys = Partitions::get().find("/system_root");
            if (!sys) sys = Partitions::get().find("/system");
            if (sys && sys->present) {
                p.dev = sys->blockDev;
                p.cur = Partitions::blockSize(sys->blockDev);
                p.feasible = p.need && (p.cur == 0 || p.need <= p.cur);
                if (!p.feasible && p.need) p.why = "The GSI is bigger than the system partition";
            } else {
                p.why = "No system partition found";
            }
        }
        if (!info_.ok) p.feasible = false;
        plan_ = p;
    }

    void draw(Ui& ui, App& app, Rect area) override {
        Theme& th = ui.th;
        Canvas& c = *ui.c;
        float pad = ui.dp(16);
        Rect bottom = area.cutBottom(ui.dp(88));
        ui.beginScroll(ui.id("gsi_scroll"), area);
        float x = area.x + pad, w = area.w - pad * 2, y = area.y;

        // ---- image
        Rect ic(x, y, w, ui.dp(96));
        ui.card(ic);
        Color tint = info_.ok ? (info_.looksLikeGsi ? th.ok : th.warn) : th.danger;
        c.fillCircle(x + ui.dp(36), ic.cy(), ui.dp(22), withAlpha(tint, 0.15f));
        icons::drawCentered(c, info_.ok ? Icon::Phone : Icon::Warning, x + ui.dp(36), ic.cy(), ui.dp(22), tint);
        ui.label(Rect(x + ui.dp(70), ic.y + ui.dp(14), w - ui.dp(84), ui.dp(22)), img_.empty() ? "No image selected" : util::basename(img_),
                 ui.dp(15), th.text, Face::SemiBold);
        std::string d = img_.empty() ? "Tap to choose a GSI (.img / .img.xz / .img.gz / .zip)"
                        : !info_.ok ? info_.error
                        : info_.describe() + (info_.looksLikeGsi ? "" : "  \xE2\x80\xA2  doesn't look like a system image");
        ui.textBlock(x + ui.dp(70), ic.y + ui.dp(40), w - ui.dp(84), d, ui.dp(12.5f), info_.ok ? th.textDim : th.danger);
        if (ui.clickable(ui.id("gsi_pick"), ic) & Clicked)
            app.push(makeFilesScreen("", {".img", ".img.xz", ".img.gz", ".img.zst", ".img.lz4", ".xz", ".gz", ".zip"},
                                     [this](App& a, const std::string& p) { img_ = p; entry_.clear(); a.pop(); reload(); }, "Select GSI",
                                     [](App& a) { a.pop(); a.push(makeManualFlashScreen()); }));
        y += ic.h + ui.dp(14);

        if (busy_) {
            Rect b(x, y, w, ui.dp(84));
            c.fillRoundRect(b, ui.dp(18), withAlpha(th.danger, 0.14f));
            ui.textBlock(x + ui.dp(16), y + ui.dp(14), w - ui.dp(32), busyWhy_, ui.dp(13), th.danger, Face::SemiBold);
            y += b.h + ui.dp(14);
        }

        // ---- space in super
        ui.sectionHeader(Rect(x, y, w, ui.dp(24)), plan_.dynamic ? "Dynamic partitions" : "System partition");
        y += ui.dp(30);
        Rect sc(x, y, w, plan_.dynamic ? ui.dp(170) : ui.dp(96));
        ui.card(sc);
        float sy = y + ui.dp(14);
        auto row = [&](const std::string& k, const std::string& v, Color col) {
            font::drawIn(c, Face::Regular, ui.dp(13), Rect(x + ui.dp(16), sy, w / 2, ui.dp(20)), k, th.textDim);
            font::drawIn(c, Face::SemiBold, ui.dp(13), Rect(x + w / 2, sy, w / 2 - ui.dp(16), ui.dp(20)), v, col, AlignRight);
            sy += ui.dp(26);
        };
        if (plan_.dynamic) {
            uint64_t total = meta_.totalBytes();
            Rect bar(x + ui.dp(16), sy, w - ui.dp(32), ui.dp(12));
            c.fillRoundRect(bar, ui.dp(6), th.surface2);
            float bx = bar.x;
            Color cols[] = {th.accent, th.info, th.warn, th.accent2, th.ok, th.danger};
            int k = 0;
            for (auto& part : meta_.parts) {
                if (!part.bytes()) continue;
                float pw = bar.w * float(part.bytes()) / std::max<uint64_t>(1, total);
                Color col = part.name == plan_.sysName ? th.accent : withAlpha(cols[1 + k++ % 5], 0.7f);
                c.fillRect(Rect(bx, bar.y, std::max(1.f, pw - 1), bar.h), col);
                bx += pw;
            }
            sy += ui.dp(24);
            row(plan_.sysName + " now", util::humanSize(plan_.cur), th.text);
            row("GSI needs", plan_.need ? util::humanSize(plan_.need) : "-", plan_.need > plan_.cur ? th.warn : th.ok);
            row("Free in super", util::humanSize(meta_.freeBytes()), th.text);
            row("Result", !plan_.need ? "-" : plan_.feasible ? (plan_.grow ? "grow by " + util::humanSize(plan_.grow) : "fits as is") : "doesn't fit",
                plan_.feasible ? th.ok : th.danger);
        } else {
            row("Device", plan_.dev.empty() ? "not found" : pathcheck::pretty(plan_.dev), th.text);
            row("Size", plan_.cur ? util::humanSize(plan_.cur) : "-", th.text);
            row("Result", plan_.feasible ? "fits" : plan_.why, plan_.feasible ? th.ok : th.danger);
        }
        y += sc.h + ui.dp(16);

        // ---- options
        ui.sectionHeader(Rect(x, y, w, ui.dp(24)), "Steps");
        y += ui.dp(30);
        struct Opt { const char* id; Icon icon; std::string title, sub; bool* v; bool show; const char* key; };
        std::string other = device::info().slot == "_b" ? "_a" : "_b";
        bool tight = plan_.dynamic && plan_.need > plan_.cur && !plan_.feasible;
        std::vector<Opt> opts = {
            {"o_other", Icon::Swap, "Free the other slot (" + other + ")", "Frees " + util::humanSize(plan_.otherSlotBytes) +
             ". That slot won't boot until reflashed.", &freeOther_, plan_.dynamic && plan_.otherSlotBytes > 0, nullptr},
            {"o_prod", Icon::Trash, "Delete product" + device::info().slot, "Frees " + util::humanSize(plan_.productBytes) +
             ". Most GSIs don't need the vendor product partition.", &deleteProduct_, plan_.dynamic && plan_.productBytes > 0, nullptr},
            {"o_avb", Icon::Bolt, "Disable AVB verification", avb_.ok ? "vbmeta flags now: " + std::to_string(avb_.flags) + " (GSIs need 3)"
             : "No vbmeta header found - skipped", &disableAvb_, true, "gsi_avb"},
            {"o_wipe", Icon::Wipe, "Factory reset (keeps internal storage)", "Needed when coming from another ROM", &wipeData_, true, "gsi_wipe"},
            {"o_verify", Icon::Shield, "Verify after flashing", "Read back and compare SHA-256", &verify_, true, "img_verify"},
            {"o_pre", Icon::Search, "Check archive first", "Decompress once before writing anything", &precheck_,
             info_.comp != img::Compression::None, nullptr},
            {"o_reboot", Icon::Reboot, "Reboot to system when done", "", &reboot_, true, nullptr},
        };
        int n = 0;
        for (auto& o : opts) n += o.show;
        Rect oc(x, y, w, ui.dp(72) * n + ui.dp(8));
        ui.card(oc);
        float oy = y + ui.dp(4);
        for (auto& o : opts) {
            if (!o.show) continue;
            if (tight && (o.v == &freeOther_ || o.v == &deleteProduct_) && !*o.v)
                c.fillRoundRect(Rect(x + ui.dp(4), oy, w - ui.dp(8), ui.dp(68)), ui.dp(14), withAlpha(th.warn, 0.12f));
            if (ui.toggleRow(ui.id(o.id), Rect(x + ui.dp(4), oy, w - ui.dp(8), ui.dp(68)), o.icon, o.title, o.sub, *o.v)) {
                if (o.key) Settings::get().setFlag(o.key, *o.v), Settings::get().save();
                plan();
            }
            oy += ui.dp(72);
        }
        y += oc.h + ui.dp(12);
        ui.textBlock(x + ui.dp(4), y, w - ui.dp(8),
                     "Bootloader must be unlocked. If the GSI bootloops on an encrypted phone, use Wipe > Format Data. "
                     "Super metadata is backed up to TWRP/super before any change.",
                     ui.dp(12), th.textFaint);
        y += ui.dp(56);
        ui.endScroll(y);

        Rect sb = bottom.inset(pad, ui.dp(14));
        bool ok = plan_.feasible && !busy_ && !img_.empty();
        if (!ok) {
            c.fillRoundRect(sb, sb.h / 2, th.surface2);
            std::string why = img_.empty() ? "Choose a GSI first" : busy_ ? "Super is busy (OTA pending)" : !info_.ok ? "Can't read the image" : plan_.why;
            ui.label(sb, why.empty() ? "Not ready" : why, ui.dp(14), th.textFaint, Face::SemiBold, AlignCenter);
        } else if (ui.slideToConfirm(ui.id("gsi_go"), sb, "Swipe to flash GSI", th.accent)) {
            start(app);
        }
    }

    void start(App& app) {
        Plan p = plan_;
        std::string img = img_, entry = entry_;
        bool freeOther = freeOther_, delProduct = deleteProduct_, avbOff = disableAvb_ && avb_.ok, wipe = wipeData_, reboot = reboot_;
        img::FlashOptions fo;
        fo.verify = verify_;
        fo.precheck = precheck_;
        std::string slot = device::info().slot;
        app.runTask("Flashing GSI", Icon::Phone, [=](Task& t) {
            t.print("GSI: %s", util::basename(img).c_str());
            int steps = 1 + freeOther + delProduct + (p.grow > 0) + avbOff + wipe;
            int k = 0;
            auto next = [&](float share) {
                float start = float(k) / steps;
                t.setRange(start, share / steps);
                k++;
            };
            std::string dev = p.dev;
            img::FlashOptions o = fo;
            if (p.dynamic) {
                for (auto& part : Partitions::get().all())
                    if (part.mounted && (part.mountPoint == "/system_root" || part.mountPoint == "/system")) Partitions::get().unmount(part);
                if (freeOther) { next(1); t.setStatus("Freeing the other slot"); if (!lp::freeOtherSlot(t)) return false; }
                if (delProduct) { next(1); t.setStatus("Deleting product" + slot); if (!lp::deletePartition("product" + slot, t)) return false; }
                if (p.grow) { next(1); t.setStatus("Resizing " + p.sysName); if (!lp::resizePartition(p.sysName, p.need, t)) return false; }
                lp::Metadata m;
                std::string e;
                if (!lp::read(lp::superDevice(), lp::currentSlotIndex(), m, e) || !m.find(p.sysName)) { t.print("Re-reading super failed: %s", e.c_str()); return false; }
                o.limit = m.find(p.sysName)->bytes();
                if (lp::mappedPath(p.sysName).empty() && !lp::map(m, p.sysName, true, e)) { t.print("Cannot map %s: %s", p.sysName.c_str(), e.c_str()); return false; }
                dev = lp::mappedPath(p.sysName);
            }
            next(1);
            t.setStatus("Writing the system image");
            if (!img::flash(img, entry, dev, o, t)) return false;
            if (avbOff) { next(1); t.setStatus("Patching vbmeta"); if (!avb::disableVerification(false, t)) t.print("(continuing without the vbmeta patch)"); }
            if (wipe) {
                next(1);
                t.setStatus("Factory reset");
                for (auto mp : {"/data", "/cache"})
                    if (auto* part = Partitions::get().find(mp)) Partitions::get().wipe(*part, t);
                Partitions::get().wipeDalvik(t);
            }
            t.setRange(0, 1);
            t.setProgress(1);
            t.print("GSI flashed to %s.", p.sysName.c_str());
            t.print("First boot of a GSI can take several minutes.");
            if (reboot) {
                t.print("Rebooting in 3 seconds...");
                sleep(3);
                device::reboot("");
            }
            return true;
        });
    }

private:
    std::string img_, entry_;
    img::Info info_;
    lp::Metadata meta_;
    bool haveMeta_ = false, busy_ = false;
    std::string metaErr_, busyWhy_;
    avb::Info avb_;
    Plan plan_;
    bool freeOther_ = false, deleteProduct_ = false, disableAvb_ = true, wipeData_ = true, verify_ = true, precheck_ = false, reboot_ = false;
};

}  // namespace

std::unique_ptr<Screen> makeGsiScreen(const std::string& image) { return std::make_unique<GsiScreen>(image); }

}  // namespace tw
