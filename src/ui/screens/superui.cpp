// Super (dynamic partitions) manager and Verified Boot (vbmeta) screens.
#include "ui/screens/screens.h"

#include "core/util.h"
#include "platform/device.h"
#include "sys/avb.h"
#include "sys/imagesrc.h"
#include "sys/partitions.h"
#include "sys/pathcheck.h"
#include "sys/super.h"

namespace tw {

namespace {

uint64_t parseMb(const std::string& s) {
    try {
        double v = std::stod(s);
        return v > 0 ? uint64_t(v * (1 << 20)) : 0;
    } catch (...) {
        return 0;
    }
}

class SuperScreen : public Screen {
public:
    std::string title() const override { return "Super partitions"; }
    std::string subtitle() const override {
        return ok_ ? std::to_string(m_.parts.size()) + " logical partitions  \xE2\x80\xA2  " + util::humanSize(m_.freeBytes()) + " free"
                   : "Dynamic partitions";
    }
    void onShow(App&) override { reload(); }
    void reload() {
        dev_ = lp::superDevice();
        ok_ = lp::read(dev_, lp::currentSlotIndex(), m_, err_);
        busy_ = lp::busy(busyWhy_);
    }

    void partActions(App& app, const lp::Part& p) {
        Dialog d;
        d.title = p.name;
        d.message = util::humanSize(p.bytes()) + "  \xE2\x80\xA2  " + std::to_string(p.extents.size()) + " extent(s)  \xE2\x80\xA2  group " +
                    m_.groups[p.group].name + ((p.attrs & lp::kAttrReadonly) ? "  \xE2\x80\xA2  read-only" : "");
        std::string name = p.name;
        uint64_t cur = p.bytes(), grow = m_.maxGrowBytes(name);
        struct A { Icon i; std::string l; int code; };
        std::vector<A> acts = {
            {Icon::Image, "Resize to fit an image...", 1},
            {Icon::Plus, "+512 MB", 2},
            {Icon::Plus, "+1 GB", 3},
            {Icon::Up, "Grow to maximum (+" + util::humanSize(grow) + ")", 4},
            {Icon::Edit, "Custom size (MB)...", 5},
            {Icon::Bolt, "Flash an image to " + name, 6},
            {Icon::Key, "Map read-write", 7},
            {Icon::Down, "Shrink to 0 (keep entry)", 8},
            {Icon::Trash, "Delete partition", 9},
        };
        for (auto& a : acts) d.actions.push_back({a.i, a.l});
        d.onAction = [this, acts, name, cur, grow, &app](int idx) {
            if (idx < 0) return;
            auto resize = [&app, name](uint64_t bytes) {
                app.runTask("Resizing " + name, Icon::Storage, [name, bytes](Task& t) { return lp::resizePartition(name, bytes, t); });
            };
            switch (acts[idx].code) {
                case 1:
                    app.push(makeFilesScreen("", img::imageExtensions(), [name](App& a, const std::string& p) {
                        img::Info i = img::inspect(p);
                        a.pop();
                        if (!i.ok || !i.imageSize) { a.toast("Can't tell this image's size", Icon::Warning, a.ui.th.danger); return; }
                        a.runTask("Resizing " + name, Icon::Storage, [name, i](Task& t) {
                            t.print("%s expands to %s", util::basename(i.path).c_str(), util::humanSize(i.imageSize).c_str());
                            return lp::resizePartition(name, i.imageSize, t);
                        });
                    }, "Image to fit"));
                    break;
                case 2: resize(cur + (512ull << 20)); break;
                case 3: resize(cur + (1ull << 30)); break;
                case 4: resize(cur + grow); break;
                case 5:
                    app.push(makeTextInputScreen("New size of " + name + " (MB)", "e.g. 3500", std::to_string(cur >> 20),
                                                 [resize](App& a, const std::string& s) {
                                                     uint64_t b = parseMb(s);
                                                     if (!b) { a.toast("Enter a size in MB", Icon::Warning); return; }
                                                     resize(b);
                                                 }));
                    break;
                case 6: {
                    std::string dev = lp::mappedPath(name);
                    if (dev.empty()) { app.toast(name + " is not mapped (size 0?)", Icon::Warning, app.ui.th.warn); break; }
                    app.push(makeFilesScreen("", img::imageExtensions(), [](App& a, const std::string& p) { a.pop(); a.push(makeFlashImageScreen(p)); },
                                             "Image for " + name, [](App& a) { a.pop(); a.push(makeManualFlashScreen()); }));
                    break;
                }
                case 7: {
                    lp::Metadata mm;
                    std::string e;
                    bool ok = lp::read(lp::superDevice(), lp::currentSlotIndex(), mm, e) && lp::map(mm, name, true, e);
                    app.toast(ok ? name + " mapped read-write" : e, ok ? Icon::Check : Icon::Warning, ok ? app.ui.th.ok : app.ui.th.danger);
                    reload();
                    break;
                }
                case 8:
                    app.confirm("Shrink " + name + " to 0?", "Its content is lost. The entry stays, so it can be resized again later.",
                                "Swipe to shrink", [resize]() { resize(0); });
                    break;
                case 9:
                    app.confirm("Delete " + name + "?", "The logical partition and its data are removed from super. "
                                "The metadata is backed up first (FMRRE/super).", "Swipe to delete", [&app, name]() {
                        app.runTask("Deleting " + name, Icon::Trash, [name](Task& t) { return lp::deletePartition(name, t); });
                    });
                    break;
            }
        };
        app.dialog(std::move(d));
    }

    void draw(Ui& ui, App& app, Rect area) override {
        Theme& th = ui.th;
        Canvas& c = *ui.c;
        float pad = ui.dp(16);
        ui.beginScroll(ui.id("sup_scroll"), area);
        float x = area.x + pad, w = area.w - pad * 2, y = area.y;
        if (!ok_) {
            Rect e(x, y, w, ui.dp(150));
            ui.card(e);
            icons::drawCentered(c, Icon::Storage, e.cx(), e.y + ui.dp(44), ui.dp(32), th.textFaint);
            ui.label(Rect(e.x, e.y + ui.dp(76), e.w, ui.dp(24)), "No dynamic partitions", ui.dp(16), th.text, Face::SemiBold, AlignCenter);
            ui.textBlock(e.x + ui.dp(20), e.y + ui.dp(104), e.w - ui.dp(40), pathcheck::pretty(dev_) + ": " + err_, ui.dp(12.5f), th.textDim);
            ui.endScroll(y + e.h);
            return;
        }
        if (busy_) {
            Rect b(x, y, w, ui.dp(80));
            c.fillRoundRect(b, ui.dp(18), withAlpha(th.danger, 0.14f));
            ui.textBlock(x + ui.dp(16), y + ui.dp(14), w - ui.dp(32), busyWhy_, ui.dp(13), th.danger, Face::SemiBold);
            y += b.h + ui.dp(12);
        }
        // overview
        Rect ov(x, y, w, ui.dp(150));
        ui.card(ov);
        uint64_t total = m_.totalBytes(), used = m_.usedBytes();
        font::drawIn(c, Face::Bold, ui.dp(22), Rect(x + ui.dp(16), y + ui.dp(12), w, ui.dp(30)), util::humanSize(m_.freeBytes()) + " free", th.text);
        font::drawIn(c, Face::Regular, ui.dp(12.5f), Rect(x + ui.dp(16), y + ui.dp(44), w - ui.dp(32), ui.dp(20)),
                     util::humanSize(used) + " used of " + util::humanSize(total) + "  \xE2\x80\xA2  " + std::to_string(m_.slotCount) +
                         " metadata slots" + ((m_.headerFlags & lp::kHeaderFlagVirtualAB) ? "  \xE2\x80\xA2  virtual A/B" : ""),
                     th.textDim);
        Rect bar(x + ui.dp(16), y + ui.dp(74), w - ui.dp(32), ui.dp(14));
        c.fillRoundRect(bar, ui.dp(7), th.surface2);
        Color cols[] = {th.accent, th.info, th.warn, th.accent2, th.ok, rgb(0xC084FC), th.danger};
        float bx = bar.x;
        int k = 0;
        std::vector<std::pair<std::string, Color>> legend;
        for (auto& p : m_.parts) {
            if (!p.bytes()) continue;
            Color col = cols[k++ % 7];
            float pw = bar.w * float(p.bytes()) / std::max<uint64_t>(1, total);
            c.fillRect(Rect(bx, bar.y, std::max(1.f, pw - 1), bar.h), col);
            bx += pw;
            legend.push_back({p.name, col});
        }
        float lx = x + ui.dp(16), ly = y + ui.dp(100);
        for (auto& [n, col] : legend) {
            float lw = font::measure(Face::Regular, ui.dp(11.5f), n) + ui.dp(26);
            if (lx + lw > x + w - ui.dp(8)) { lx = x + ui.dp(16); ly += ui.dp(20); }
            if (ly > ov.b() - ui.dp(14)) break;
            c.fillCircle(lx + ui.dp(5), ly + ui.dp(8), ui.dp(4.5f), col);
            font::drawIn(c, Face::Regular, ui.dp(11.5f), Rect(lx + ui.dp(14), ly, lw, ui.dp(16)), n, th.textDim);
            lx += lw;
        }
        y += ov.h + ui.dp(16);

        // groups
        ui.sectionHeader(Rect(x, y, w, ui.dp(24)), "Groups");
        y += ui.dp(30);
        Rect gc(x, y, w, ui.dp(52) * m_.groups.size() + ui.dp(8));
        ui.card(gc);
        float gy = y + ui.dp(8);
        for (uint32_t g = 0; g < m_.groups.size(); g++) {
            auto& gr = m_.groups[g];
            font::drawIn(c, Face::SemiBold, ui.dp(13.5f), Rect(x + ui.dp(16), gy, w * 0.6f, ui.dp(20)), gr.name, th.text);
            std::string v = util::humanSize(m_.groupUsed(g)) + (gr.maxSize ? " / " + util::humanSize(gr.maxSize) : " (no limit)");
            font::drawIn(c, Face::Regular, ui.dp(12), Rect(x + w * 0.5f, gy, w * 0.5f - ui.dp(16), ui.dp(20)), v, th.textDim, AlignRight);
            if (gr.maxSize) ui.usageBar(Rect(x + ui.dp(16), gy + ui.dp(26), w - ui.dp(32), ui.dp(6)), float(m_.groupUsed(g)) / gr.maxSize, th.info);
            gy += ui.dp(52);
        }
        y += gc.h + ui.dp(16);

        // partitions
        ui.sectionHeader(Rect(x, y, w, ui.dp(24)), "Logical partitions  (tap for actions)");
        y += ui.dp(30);
        Rect pc(x, y, w, ui.dp(66) * m_.parts.size() + ui.dp(8));
        ui.card(pc);
        float py = y + ui.dp(4);
        for (size_t i = 0; i < m_.parts.size(); i++) {
            auto& p = m_.parts[i];
            bool mapped = !lp::mappedPath(p.name).empty();
            std::string sub = std::to_string(p.extents.size()) + " extent(s)" + ((p.attrs & lp::kAttrReadonly) ? "  \xE2\x80\xA2  ro" : "") +
                              (mapped ? "  \xE2\x80\xA2  mapped" : p.bytes() ? "  \xE2\x80\xA2  not mapped" : "");
            if (ui.listItem(ui.id("lp", (int)i), Rect(x + ui.dp(4), py, w - ui.dp(8), ui.dp(62)), p.bytes() ? Icon::Chip : Icon::File, p.name, sub,
                            p.bytes() ? util::humanSize(p.bytes()) : "empty", p.bytes() ? th.accent : th.textFaint) & Clicked) {
                if (busy_) app.toast("Super is busy: " + busyWhy_, Icon::Warning, th.danger);
                else partActions(app, p);
            }
            py += ui.dp(66);
        }
        y += pc.h + ui.dp(16);

        // tools
        ui.sectionHeader(Rect(x, y, w, ui.dp(24)), "Tools");
        y += ui.dp(30);
        struct T { Icon icon; const char* title; const char* sub; int code; };
        T tools[] = {{Icon::Swap, "Free the other slot", "Shrink every _a/_b partition of the inactive slot to 0", 1},
                     {Icon::Plus, "Create logical partition", "New partition in a group", 2},
                     {Icon::Backup, "Back up metadata", "Save geometry + all metadata slots", 3},
                     {Icon::Restore, "Restore metadata backup", "Undo resizes/deletes from a saved copy", 4}};
        Rect tc(x, y, w, ui.dp(70) * 4 + ui.dp(8));
        ui.card(tc);
        for (int i = 0; i < 4; i++) {
            if (!(ui.listItem(ui.id("st", i), Rect(x + ui.dp(4), y + ui.dp(4) + i * ui.dp(70), w - ui.dp(8), ui.dp(66)), tools[i].icon, tools[i].title,
                              tools[i].sub, "", i == 3 ? th.warn : th.accent) & Clicked))
                continue;
            switch (tools[i].code) {
                case 1:
                    app.confirm("Free the other slot?", "All partitions of the inactive slot are shrunk to 0. That slot can't boot until it is "
                                "flashed again (an OTA does this).", "Swipe to free", [&app]() {
                        app.runTask("Freeing the other slot", Icon::Swap, [](Task& t) { return lp::freeOtherSlot(t); });
                    });
                    break;
                case 2: {
                    std::string grp = m_.groups.size() > 1 ? m_.groups[1 + lp::currentSlotIndex() % (m_.groups.size() - 1)].name : m_.groups[0].name;
                    app.push(makeTextInputScreen("Name and size in MB", "e.g. product_a 1024", "", [grp](App& a, const std::string& s) {
                        auto f = util::splitWs(s);
                        if (f.size() != 2 || !parseMb(f[1])) { a.toast("Type: <name> <size MB>", Icon::Warning); return; }
                        std::string n = f[0];
                        uint64_t b = parseMb(f[1]);
                        a.runTask("Creating " + n, Icon::Plus, [n, grp, b](Task& t) { return lp::createPartition(n, grp, b, t); });
                    }));
                    break;
                }
                case 3: {
                    std::string f = lp::backupDir() + "/super_metadata_" + util::dateStamp() + ".bin", e;
                    bool okb = lp::saveMetadataRegion(dev_, f, e);
                    app.toast(okb ? "Saved " + util::basename(f) : e, okb ? Icon::Check : Icon::Warning, okb ? th.ok : th.danger);
                    break;
                }
                case 4:
                    util::mkdirs(lp::backupDir());
                    app.push(makeFilesScreen(lp::backupDir(), {".bin"}, [this](App& a, const std::string& f) {
                        a.pop();
                        a.confirm("Restore " + util::basename(f) + "?", "Replaces the current super metadata (partition layout) with this copy. "
                                  "Reboot to recovery afterwards.", "Swipe to restore", [&a, f, this]() {
                            std::string e;
                            bool okr = lp::restoreMetadataRegion(dev_, f, e);
                            a.toast(okr ? "Metadata restored - reboot to recovery" : e, okr ? Icon::Check : Icon::Warning, okr ? a.ui.th.ok : a.ui.th.danger);
                            reload();
                        });
                    }, "Metadata backup"));
                    break;
            }
        }
        y += tc.h + ui.dp(16);
        ui.endScroll(y);
    }

private:
    std::string dev_, err_, busyWhy_;
    lp::Metadata m_;
    bool ok_ = false, busy_ = false;
};

class VbmetaScreen : public Screen {
public:
    std::string title() const override { return "Verified boot"; }
    std::string subtitle() const override { return "vbmeta: dm-verity & AVB verification"; }

    void draw(Ui& ui, App& app, Rect area) override {
        Theme& th = ui.th;
        Canvas& c = *ui.c;
        float pad = ui.dp(16);
        ui.beginScroll(ui.id("vb_scroll"), area);
        float x = area.x + pad, w = area.w - pad * 2, y = area.y;
        std::vector<std::string> slots = {device::info().slot};
        if (device::info().ab) slots.push_back(device::info().slot == "_b" ? "_a" : "_b");
        bool any = false;
        for (size_t i = 0; i < slots.size(); i++) {
            std::string dev = avb::vbmetaDevice(slots[i]);
            if (dev.empty()) continue;
            any = true;
            avb::Info inf = avb::read(dev);
            Rect r(x, y, w, ui.dp(112));
            ui.card(r);
            bool off = inf.ok && (inf.flags & 3) == 3;
            Color col = !inf.ok ? th.danger : off ? th.warn : th.ok;
            c.fillCircle(x + ui.dp(34), y + ui.dp(36), ui.dp(20), withAlpha(col, 0.15f));
            icons::drawCentered(c, off ? Icon::Bolt : Icon::Shield, x + ui.dp(34), y + ui.dp(36), ui.dp(20), col);
            font::drawIn(c, Face::SemiBold, ui.dp(15), Rect(x + ui.dp(66), y + ui.dp(16), w, ui.dp(22)),
                         util::basename(dev) + (i == 0 ? "  (current slot)" : ""), th.text);
            std::string st = !inf.ok ? inf.error
                             : off ? "Verification DISABLED (flags " + std::to_string(inf.flags) + ")"
                             : (inf.flags & 1) ? "verity off, verification on (flags 1)"
                             : (inf.flags & 2) ? "verification off (flags 2)" : "Verification enabled (stock)";
            font::drawIn(c, Face::Regular, ui.dp(12.5f), Rect(x + ui.dp(66), y + ui.dp(42), w - ui.dp(80), ui.dp(20)), st, col);
            if (inf.ok)
                font::drawIn(c, Face::Mono, ui.dp(11), Rect(x + ui.dp(66), y + ui.dp(66), w - ui.dp(80), ui.dp(18)),
                             "AVB " + std::to_string(inf.versionMajor) + "." + std::to_string(inf.versionMinor) + "  rollback " +
                                 std::to_string(inf.rollbackIndex) + "  " + inf.release,
                             th.textFaint);
            y += r.h + ui.dp(12);
        }
        if (!any) {
            ui.textBlock(x, y, w, "This device has no vbmeta partition (verification data may live in a boot footer).", ui.dp(14), th.textDim);
            y += ui.dp(60);
        }
        y += ui.dp(6);
        float bw = (w - ui.dp(10)) / 2;
        if (ui.button(ui.id("vb_off"), Rect(x, y, bw, ui.dp(52)), "Disable", Icon::Bolt, BtnStyle::Danger))
            app.confirm("Disable verified boot?", "Sets vbmeta flags to 3 (like fastboot --disable-verity --disable-verification). Needed for "
                        "most GSIs and modified system partitions. Unlocked bootloader required.", "Swipe to disable", [&app]() {
                app.runTask("Disabling verification", Icon::Bolt, [](Task& t) { return avb::disableVerification(true, t); });
            });
        if (ui.button(ui.id("vb_on"), Rect(x + bw + ui.dp(10), y, bw, ui.dp(52)), "Re-enable", Icon::Shield, BtnStyle::Tonal))
            app.runTask("Enabling verification", Icon::Shield, [](Task& t) { return avb::enableVerification(true, t); });
        y += ui.dp(68);
        if (ui.button(ui.id("vb_flash"), Rect(x, y, w, ui.dp(50)), "Flash a vbmeta.img", Icon::Image, BtnStyle::Outline))
            app.push(makeFilesScreen("", img::imageExtensions(), [](App& a, const std::string& p) { a.pop(); a.push(makeFlashImageScreen(p)); },
                                     "Select vbmeta image"));
        y += ui.dp(66);
        ui.textBlock(x, y, w, "The original header is saved to FMRRE/vbmeta before every change. Re-enabling restores flags 0 "
                              "(the bootloader may then refuse a modified system).", ui.dp(12), th.textFaint);
        ui.endScroll(y + ui.dp(60));
    }
};

}  // namespace

std::unique_ptr<Screen> makeSuperScreen() { return std::make_unique<SuperScreen>(); }
std::unique_ptr<Screen> makeVbmetaScreen() { return std::make_unique<VbmetaScreen>(); }

}  // namespace tw
