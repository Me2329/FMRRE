// A/B ROMs and OTAs (payload.bin): what will be written where, then install to the inactive slot.
// Also extracts partition images (boot / init_boot for Magisk, vendor_boot, system...).
#include "ui/screens/screens.h"

#include <unistd.h>

#include "core/settings.h"
#include "core/util.h"
#include "platform/device.h"
#include "sys/partitions.h"
#include "sys/pathcheck.h"
#include "sys/payload.h"
#include "sys/super.h"

namespace tw {

namespace {

std::string slotName(const std::string& s) { return s == "_b" ? "B" : "A"; }

float textHeight(Ui& ui, const std::string& s, float size, float w, Face f = Face::Regular) {
    return font::wrap(f, size, s, w).size() * font::lineHeight(f, size);
}

std::string outDirFor(const std::string& zip) {
    std::string b = util::basename(zip);
    for (auto ext : {".zip", ".bin"})
        if (util::endsWith(util::toLower(b), ext)) b = b.substr(0, b.size() - 4);
    return Partitions::get().appDir() + "/payload/" + b;
}

class PayloadExtractScreen : public Screen {
public:
    explicit PayloadExtractScreen(const payload::Payload& p) : p_(p) {
        for (auto& pu : p_.parts) {
            bool def = pu.name == "boot" || pu.name == "init_boot" || pu.name == "vendor_boot";
            sel_.push_back(def);
        }
    }
    std::string title() const override { return "Extract images"; }
    std::string subtitle() const override { return util::basename(p_.path); }

    void draw(Ui& ui, App& app, Rect area) override {
        Theme& th = ui.th;
        float pad = ui.dp(16);
        Rect bottom = area.cutBottom(ui.dp(88));
        ui.beginScroll(ui.id("px_scroll"), area);
        float x = area.x + pad, w = area.w - pad * 2, y = area.y;
        float th1 = textHeight(ui, hint(), ui.dp(12.5f), w - ui.dp(8));
        ui.textBlock(x + ui.dp(4), y, w - ui.dp(8), hint(), ui.dp(12.5f), th.textDim);
        y += th1 + ui.dp(12);
        float cw = (w - ui.dp(8)) / 3;
        if (ui.chip(ui.id("px_root"), Rect(x, y, cw, ui.dp(36)), "For root", false, Icon::Key)) select({"boot", "init_boot"});
        if (ui.chip(ui.id("px_all"), Rect(x + cw + ui.dp(4), y, cw, ui.dp(36)), "All", false)) std::fill(sel_.begin(), sel_.end(), true);
        if (ui.chip(ui.id("px_none"), Rect(x + 2 * (cw + ui.dp(4)), y, cw, ui.dp(36)), "None", false)) std::fill(sel_.begin(), sel_.end(), false);
        y += ui.dp(48);
        Rect card(x, y, w, ui.dp(60) * p_.parts.size() + ui.dp(8));
        ui.card(card);
        float iy = y + ui.dp(4);
        for (size_t i = 0; i < p_.parts.size(); i++) {
            auto& pu = p_.parts[i];
            bool v = sel_[i];
            std::string sub = std::string(p_.isDynamic(pu.name) ? "dynamic" : "partition") + (pu.hashTree.count ? " \xE2\x80\xA2 verity computed" : "");
            if (ui.checkRow(ui.id("px", (int)i), Rect(x + ui.dp(4), iy, w - ui.dp(8), ui.dp(56)), pu.name + ".img", sub, v,
                            util::humanSize(pu.newSize)))
                sel_[i] = v;
            iy += ui.dp(60);
        }
        y += card.h + ui.dp(12);
        ui.label(Rect(x, y, w, ui.dp(20)), "To " + pathcheck::pretty(outDirFor(p_.path)), ui.dp(12.5f), th.textFaint);
        y += ui.dp(36);
        ui.endScroll(y);

        uint64_t total = 0;
        std::vector<std::string> names;
        for (size_t i = 0; i < p_.parts.size(); i++)
            if (sel_[i]) { names.push_back(p_.parts[i].name); total += p_.parts[i].newSize; }
        Rect b = bottom.inset(pad, ui.dp(16));
        if (names.empty()) {
            ui.c->fillRoundRect(b, b.h / 2, th.surface2);
            ui.label(b, "Select partitions", ui.dp(14), th.textFaint, Face::SemiBold, AlignCenter);
        } else if (ui.button(ui.id("px_go"), b, "Extract " + std::to_string(names.size()) + " (" + util::humanSize(total) + ")", Icon::Download)) {
            payload::Payload p = p_;
            std::string out = outDirFor(p_.path);
            app.runTask("Extracting images", Icon::Download, [p, names, out](Task& t) { return payload::extract(p, names, out, true, t); });
        }
    }

private:
    std::string hint() const {
        return "Writes <name>.img files, with the dm-verity hash tree and error correction filled in like the phone does, and checks "
               "each one against the SHA-256 in the payload. Patch boot.img / init_boot.img with Magisk or KernelSU, or flash them with fastboot.";
    }
    void select(std::initializer_list<const char*> names) {
        for (size_t i = 0; i < p_.parts.size(); i++) {
            sel_[i] = false;
            for (auto n : names) sel_[i] = sel_[i] || p_.parts[i].name == n;
        }
    }
    payload::Payload p_;
    std::vector<bool> sel_;
};

class PayloadScreen : public Screen {
public:
    explicit PayloadScreen(const std::string& path) : path_(path) {
        ok_ = payload::open(path_, p_, err_);
        Settings& s = Settings::get();
        opts_.verify = s.flag("ota_verify", true);
        opts_.switchSlot = s.flag("ota_switch", true);
        replan();
    }
    std::string title() const override { return "A/B ROM / OTA"; }
    std::string subtitle() const override { return util::basename(path_); }
    void onShow(App&) override { replan(); }

    void replan() {
        if (ok_) plan_ = payload::plan(p_, opts_);
    }

    void draw(Ui& ui, App& app, Rect area) override {
        Theme& th = ui.th;
        Canvas& c = *ui.c;
        float pad = ui.dp(16);
        Rect bottom = area.cutBottom(ui.dp(88));
        ui.beginScroll(ui.id("pl_scroll"), area);
        float x = area.x + pad, w = area.w - pad * 2, y = area.y;

        // ---- package
        auto md = [&](const char* k) {
            auto it = p_.metadata.find(k);
            return it == p_.metadata.end() ? std::string() : it->second;
        };
        std::string build = md("post-build");
        std::string desc = ok_ ? p_.describe() : err_;
        float dh = textHeight(ui, desc, ui.dp(12.5f), w - ui.dp(84));
        float bh = build.empty() ? 0 : textHeight(ui, build, ui.dp(11.5f), w - ui.dp(84), Face::Mono);
        Rect ic(x, y, w, ui.dp(50) + dh + (bh ? bh + ui.dp(8) : 0) + ui.dp(14));
        ui.card(ic);
        Color tint = !ok_ ? th.danger : plan_.ok ? th.accent : th.warn;
        c.fillCircle(x + ui.dp(36), ic.y + ui.dp(38), ui.dp(22), withAlpha(tint, 0.15f));
        icons::drawCentered(c, ok_ ? Icon::Zip : Icon::Warning, x + ui.dp(36), ic.y + ui.dp(38), ui.dp(22), tint);
        ui.label(Rect(x + ui.dp(70), ic.y + ui.dp(14), w - ui.dp(84), ui.dp(22)), util::basename(path_), ui.dp(15), th.text, Face::SemiBold);
        float ty = ic.y + ui.dp(40);
        ty += ui.textBlock(x + ui.dp(70), ty, w - ui.dp(84), desc, ui.dp(12.5f), ok_ ? th.textDim : th.danger);
        if (bh) ui.textBlock(x + ui.dp(70), ty + ui.dp(6), w - ui.dp(84), build, ui.dp(11.5f), th.textFaint, Face::Mono);
        y += ic.h + ui.dp(14);
        if (!ok_) {
            ui.endScroll(y);
            ui.label(bottom.inset(pad, ui.dp(20)), "Can't read this package", ui.dp(13), th.textFaint, Face::Regular, AlignCenter);
            return;
        }

        // ---- plan
        ui.sectionHeader(Rect(x, y, w, ui.dp(24)), "What happens");
        y += ui.dp(30);
        std::vector<std::pair<std::string, std::string>> rows;
        const std::string cur = device::info().slot;
        if (plan_.ok || !plan_.targetSlot.empty()) {
            rows.push_back({"Current slot", slotName(cur)});
            rows.push_back({"Written to", "slot " + slotName(plan_.targetSlot)});
        }
        size_t dyn = 0;
        for (auto& pu : p_.parts) dyn += p_.isDynamic(pu.name);
        rows.push_back({"Partitions", std::to_string(p_.parts.size() - dyn) + " + " + std::to_string(dyn) + " in super"});
        if (plan_.superNeeded)
            rows.push_back({"super", util::humanSize(plan_.superNeeded) + " of " + util::humanSize(plan_.superFree) + " usable"});
        Rect pc(x, y, w, ui.dp(26) * rows.size() + ui.dp(24));
        ui.card(pc);
        float sy = y + ui.dp(12);
        for (auto& r : rows) {
            font::drawIn(c, Face::Regular, ui.dp(13), Rect(x + ui.dp(16), sy, w / 2, ui.dp(20)), r.first, th.textDim);
            font::drawIn(c, Face::SemiBold, ui.dp(13), Rect(x + w / 2, sy, w / 2 - ui.dp(16), ui.dp(20)), r.second, th.text, AlignRight);
            sy += ui.dp(26);
        }
        y += pc.h + ui.dp(12);

        std::vector<std::pair<std::string, Color>> notes;
        if (!plan_.ok) notes.push_back({plan_.error, th.danger});
        for (auto& n : plan_.notes) notes.push_back({n, plan_.needsFreeSource && n.find("one slot") != std::string::npos ? th.warn : th.textDim});
        for (auto& n : notes) {
            float h = textHeight(ui, n.first, ui.dp(12.5f), w - ui.dp(56));
            Rect nr(x, y, w, h + ui.dp(20));
            c.fillRoundRect(nr, ui.dp(14), withAlpha(n.second == th.textDim ? th.info : n.second, 0.10f));
            icons::drawCentered(c, n.second == th.textDim ? Icon::Info : Icon::Warning, x + ui.dp(22), y + ui.dp(20), ui.dp(16),
                                n.second == th.textDim ? th.info : n.second);
            ui.textBlock(x + ui.dp(40), y + ui.dp(10), w - ui.dp(56), n.first, ui.dp(12.5f), n.second == th.textDim ? th.text : n.second);
            y += nr.h + ui.dp(8);
        }
        y += ui.dp(6);

        // ---- partitions
        ui.sectionHeader(Rect(x, y, w, ui.dp(24)), "Partitions");
        y += ui.dp(30);
        Rect lc(x, y, w, ui.dp(30) * p_.parts.size() + ui.dp(16));
        ui.card(lc);
        float ly = y + ui.dp(8);
        for (auto& pu : p_.parts) {
            std::string tag = p_.isDynamic(pu.name) ? "super" : "";
            if (pu.hashTree.count) tag += tag.empty() ? "verity" : " \xE2\x80\xA2 verity";
            font::drawIn(c, Face::Mono, ui.dp(12.5f), Rect(x + ui.dp(16), ly, w * 0.45f, ui.dp(22)), pu.name + plan_.targetSlot, th.text);
            font::drawIn(c, Face::Regular, ui.dp(11.5f), Rect(x + w * 0.45f, ly, w * 0.27f, ui.dp(22)), tag, th.textFaint);
            font::drawIn(c, Face::SemiBold, ui.dp(12.5f), Rect(x + w * 0.70f, ly, w * 0.30f - ui.dp(16), ui.dp(22)), util::humanSize(pu.newSize),
                         th.textDim, AlignRight);
            ly += ui.dp(30);
        }
        y += lc.h + ui.dp(16);

        // ---- options
        ui.sectionHeader(Rect(x, y, w, ui.dp(24)), "Options");
        y += ui.dp(30);
        bool forceRow = plan_.deviceMismatch || opts_.force;
        Rect oc(x, y, w, ui.dp(72) * (3 + forceRow) + ui.dp(8));
        ui.card(oc);
        float oy = y + ui.dp(4);
        auto row = [&](const char* id, Icon icon, const std::string& t, const std::string& s, bool& v, const char* key) {
            if (ui.toggleRow(ui.id(id), Rect(x + ui.dp(4), oy, w - ui.dp(8), ui.dp(68)), icon, t, s, v)) {
                if (key) Settings::get().setFlag(key, v), Settings::get().save();
                replan();
            }
            oy += ui.dp(72);
        };
        row("pl_sw", Icon::Swap, "Switch to slot " + slotName(plan_.targetSlot.empty() ? "_b" : plan_.targetSlot) + " afterwards",
            "Like a normal OTA. Off: install only, stay on " + slotName(cur), opts_.switchSlot, "ota_switch");
        row("pl_ver", Icon::Shield, "Verify every partition", "Check each partition's SHA-256 after writing", opts_.verify, "ota_verify");
        row("pl_rb", Icon::Reboot, "Reboot when done", "Boot the new system right away", reboot_, nullptr);
        if (forceRow)
            row("pl_force", Icon::Warning, "Ignore the device check", "Only if you're sure this build is for your phone "
                "(e.g. a renamed variant)", opts_.force, nullptr);
        y += oc.h + ui.dp(12);
        if (ui.button(ui.id("pl_extract"), Rect(x, y, w, ui.dp(50)), "Extract images (boot.img for Magisk...)", Icon::Download, BtnStyle::Tonal))
            app.push(std::make_unique<PayloadExtractScreen>(p_));
        y += ui.dp(66);
        ui.endScroll(y);

        Rect sb = bottom.inset(pad, ui.dp(14));
        if (!plan_.ok) {
            c.fillRoundRect(sb, sb.h / 2, th.surface2);
            ui.label(sb, p_.full() ? "Can't install on this phone" : "Incremental OTA - extract or use the full one", ui.dp(14), th.textFaint,
                     Face::SemiBold, AlignCenter);
        } else if (ui.slideToConfirm(ui.id("pl_go"), sb, "Swipe to install to slot " + slotName(plan_.targetSlot), th.accent)) {
            payload::Payload p = p_;
            payload::InstallOptions o = opts_;
            bool rb = reboot_;
            app.runTask("Installing " + util::basename(path_), Icon::Install, [p, o, rb](Task& t) {
                bool ok = payload::install(p, o, t);
                if (ok) {
                    t.print("Flashing GApps / Magisk too? Reboot to recovery first, so they go to the new slot.");
                    if (rb) {
                        t.print("Rebooting in 3 seconds...");
                        sleep(3);
                        device::reboot("");
                    }
                }
                return ok;
            });
        }
    }

private:
    std::string path_, err_;
    bool ok_ = false, reboot_ = false;
    payload::Payload p_;
    payload::InstallOptions opts_;
    payload::Plan plan_;
};

}  // namespace

std::unique_ptr<Screen> makePayloadScreen(const std::string& path) { return std::make_unique<PayloadScreen>(path); }

}  // namespace tw
