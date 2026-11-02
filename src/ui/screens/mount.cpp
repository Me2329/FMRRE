#include "ui/screens/screens.h"

#include "core/settings.h"
#include "core/util.h"
#include "sys/partitions.h"

namespace tw {

namespace {

class MountScreen : public Screen {
public:
    std::string title() const override { return "Mount"; }
    std::string subtitle() const override { return "Tap a partition to mount / unmount"; }
    void onShow(App&) override { Partitions::get().refresh(); }

    float headerActionsWidth() const override { return 44; }
    void headerActions(Ui& ui, App& app, Rect r) override {
        float s = ui.dp(44);
        if (ui.iconButton(ui.id("m_ref"), Rect(r.r() - s, r.cy() - s / 2, s, s), Icon::Refresh, ui.th.text, true)) {
            Partitions::get().refresh();
            app.toast("Refreshed", Icon::Refresh);
        }
    }

    void draw(Ui& ui, App& app, Rect area) override {
        Theme& th = ui.th;
        Canvas& c = *ui.c;
        float pad = ui.dp(16);
        ui.beginScroll(ui.id("mount_scroll"), area);
        float x = area.x + pad, w = area.w - pad * 2, y = area.y;
        ui.sectionHeader(Rect(x, y, w, ui.dp(24)), "Filesystems");
        y += ui.dp(30);
        auto& P = Partitions::get();
        for (size_t i = 0; i < P.all().size(); i++) {
            Partition& p = P.all()[i];
            if (p.isImage()) continue;
            Rect r(x, y, w, ui.dp(92));
            ui.card(r, ui.dp(20));
            Id id = ui.id("mp", (int)i);
            int res = ui.clickable(id, r);
            ui.drawPressFx(id, r, ui.dp(20), th.text);
            Color tint = p.mounted ? th.ok : (p.present ? th.textDim : th.danger);
            c.fillRoundRect(Rect(r.x + ui.dp(14), r.y + ui.dp(16), ui.dp(40), ui.dp(40)), ui.dp(12), withAlpha(tint, 0.16f));
            icons::drawCentered(c, p.removable ? Icon::Sd : Icon::Mount, r.x + ui.dp(34), r.y + ui.dp(36), ui.dp(22), tint);
            float tx = r.x + ui.dp(66);
            ui.label(Rect(tx, r.y + ui.dp(12), r.w - ui.dp(150), ui.dp(24)), p.displayName, ui.dp(16), th.text, Face::SemiBold);
            ui.label(Rect(tx, r.y + ui.dp(36), r.w - ui.dp(150), ui.dp(18)), p.mountPoint + "  \xE2\x80\xA2  " + p.fsType +
                     (p.present ? "" : "  \xE2\x80\xA2  missing"), ui.dp(12), th.textDim);
            Rect sw(r.r() - ui.dp(66), r.y + ui.dp(22), ui.dp(50), ui.dp(28));
            ui.switchVisual(id, sw, p.mounted);
            if (p.mounted && p.size) {
                float frac = float(p.used) / p.size;
                ui.usageBar(Rect(tx, r.y + ui.dp(64), r.w - ui.dp(66) - ui.dp(100), ui.dp(6)), frac);
                ui.label(Rect(r.r() - ui.dp(106), r.y + ui.dp(56), ui.dp(92), ui.dp(20)),
                         util::humanSize(p.used) + " / " + util::humanSize(p.size), ui.dp(11), th.textDim, Face::Regular, AlignRight);
            } else {
                ui.label(Rect(tx, r.y + ui.dp(58), r.w - ui.dp(80), ui.dp(18)), p.present ? (p.size ? util::humanSize(p.size) : "") : "",
                         ui.dp(12), th.textFaint);
            }
            if (res & Clicked) {
                std::string err;
                bool ok = p.mounted ? P.unmount(p, &err) : P.mount(p, &err);
                if (!ok) app.toast((p.mounted ? "Unmount failed: " : "Mount failed: ") + err, Icon::Warning, th.danger);
                P.refresh();
            }
            y += r.h + ui.dp(10);
        }
        y += ui.dp(8);
        ui.sectionHeader(Rect(x, y, w, ui.dp(24)), "Options");
        y += ui.dp(30);
        Rect opt(x, y, w, ui.dp(68) * 1 + ui.dp(8));
        ui.card(opt);
        Settings& s = Settings::get();
        bool rw = s.flag("mount_system_rw");
        if (ui.toggleRow(ui.id("sysrw"), Rect(x + ui.dp(4), y + ui.dp(4), w - ui.dp(8), ui.dp(64)), Icon::Lock, "Mount system as read/write",
                         "Off keeps dm-verity happy (recommended)", rw))
            s.setFlag("mount_system_rw", rw), s.save();
        y += opt.h + ui.dp(12);
        float bw = (w - ui.dp(10)) / 2;
        if (ui.button(ui.id("mall"), Rect(x, y, bw, ui.dp(50)), "Mount data", Icon::Mount, BtnStyle::Tonal)) {
            P.mountAllStorage();
            P.refresh();
        }
        if (ui.button(ui.id("uall"), Rect(x + bw + ui.dp(10), y, bw, ui.dp(50)), "Unmount all", Icon::Close, BtnStyle::Outline)) {
            P.unmountAll();
            P.refresh();
            app.toast("All partitions unmounted", Icon::Mount);
        }
        y += ui.dp(64);
        ui.label(Rect(x + ui.dp(6), y, w, ui.dp(18)), "fstab: " + P.fstabPath(), ui.dp(11.5f), th.textFaint, Face::Mono);
        y += ui.dp(30);
        ui.endScroll(y);
    }
};

}  // namespace

std::unique_ptr<Screen> makeMountScreen() { return std::make_unique<MountScreen>(); }

}  // namespace tw
