// Repair / resize / change file system - TWRP's "Advanced Wipe > Repair or Change File System", plus info.
#include "ui/screens/screens.h"

#include "core/util.h"
#include "platform/device.h"
#include "sys/partitions.h"
#include "sys/pathcheck.h"

namespace tw {

namespace {

class FsToolsScreen : public Screen {
public:
    explicit FsToolsScreen(std::string mp) : mp_(std::move(mp)) {}
    std::string title() const override { return mp_.empty() ? "File system tools" : "File system: " + mp_; }
    std::string subtitle() const override { return "Check, repair, resize or change the file system"; }

    void draw(Ui& ui, App& app, Rect area) override {
        Theme& th = ui.th;
        float pad = ui.dp(16);
        ui.beginScroll(ui.id("fs_scroll"), area);
        float x = area.x + pad, w = area.w - pad * 2, y = area.y;
        Partition* p = mp_.empty() ? nullptr : Partitions::get().find(mp_);
        if (!p) {
            ui.sectionHeader(Rect(x, y, w, ui.dp(24)), "Choose a partition");
            y += ui.dp(30);
            std::vector<Partition*> list;
            for (auto& q : Partitions::get().all())
                if (!q.isImage() && q.present) list.push_back(&q);
            Rect c(x, y, w, ui.dp(66) * list.size() + ui.dp(8));
            ui.card(c);
            for (size_t i = 0; i < list.size(); i++)
                if (ui.listItem(ui.id("fsp", (int)i), Rect(x + ui.dp(4), y + ui.dp(4) + i * ui.dp(66), w - ui.dp(8), ui.dp(62)),
                                list[i]->removable ? Icon::Sd : Icon::Mount, list[i]->displayName,
                                list[i]->mountPoint + "  \xE2\x80\xA2  " + list[i]->fsType, "", th.accent) & Clicked)
                    app.push(makeFsToolsScreen(list[i]->mountPoint));
            ui.endScroll(y + c.h + ui.dp(16));
            return;
        }
        Rect info(x, y, w, ui.dp(86));
        ui.card(info);
        icons::drawCentered(*ui.c, p->removable ? Icon::Sd : Icon::Mount, x + ui.dp(34), info.cy(), ui.dp(24), th.accent);
        ui.label(Rect(x + ui.dp(64), y + ui.dp(14), w - ui.dp(80), ui.dp(24)), p->displayName + "  (" + p->fsType + ")", ui.dp(16), th.text,
                 Face::SemiBold);
        ui.label(Rect(x + ui.dp(64), y + ui.dp(42), w - ui.dp(80), ui.dp(20)),
                 pathcheck::pretty(p->blockDev) + "  \xE2\x80\xA2  " + util::humanSize(Partitions::blockSize(p->blockDev)), ui.dp(12.5f), th.textDim);
        y += info.h + ui.dp(16);
        struct A { Icon i; const char* t; const char* s; int code; Color c; };
        std::vector<A> acts = {
            {Icon::Shield, "Check & repair", "e2fsck / fsck.f2fs / fsck.fat / fsck.exfat", 1, th.ok},
            {Icon::Up, "Grow file system to fill partition", "resize2fs / resize.f2fs - e.g. after resizing in super", 2, th.info},
            {Icon::Info, "File system info", "blkid, dumpe2fs header", 3, th.accent},
            {Icon::Refresh, "Change file system...", "Reformat as ext4, f2fs, exFAT or FAT32 (erases it)", 4, th.danger},
        };
        Rect c(x, y, w, ui.dp(70) * acts.size() + ui.dp(8));
        ui.card(c);
        std::string mp = p->mountPoint;
        for (size_t i = 0; i < acts.size(); i++) {
            if (!(ui.listItem(ui.id("fsa", (int)i), Rect(x + ui.dp(4), y + ui.dp(4) + i * ui.dp(70), w - ui.dp(8), ui.dp(66)), acts[i].i, acts[i].t,
                              acts[i].s, "", acts[i].c) & Clicked))
                continue;
            switch (acts[i].code) {
                case 1:
                    app.runTask("Repairing " + p->displayName, Icon::Shield, [mp](Task& t) {
                        Partition* q = Partitions::get().find(mp);
                        return q && Partitions::get().repair(*q, t);
                    });
                    break;
                case 2:
                    app.runTask("Resizing " + p->displayName, Icon::Up, [mp](Task& t) {
                        Partition* q = Partitions::get().find(mp);
                        return q && Partitions::get().resizeFs(*q, t);
                    });
                    break;
                case 3: {
                    std::string f = "/tmp/fsinfo_" + p->name + ".txt";
                    util::writeFile(device::path(f), Partitions::get().fsInfo(*p));
                    app.push(makeTextViewerScreen(device::path(f)));
                    break;
                }
                case 4: {
                    Dialog d;
                    d.title = "Format " + p->displayName + " as...";
                    d.message = "Everything on " + p->displayName + " is erased.";
                    std::vector<std::string> fss = {"ext4", "f2fs", "exfat", "vfat"};
                    for (auto& f : fss) d.actions.push_back({Icon::Storage, f == "vfat" ? std::string("FAT32 (vfat)") : f});
                    d.onAction = [&app, mp, fss](int idx) {
                        if (idx < 0) return;
                        std::string fs = fss[idx];
                        app.confirm("Format " + mp + " as " + fs + "?", "All data on it will be lost.", "Swipe to format", [&app, mp, fs]() {
                            app.runTask("Formatting " + mp, Icon::Refresh, [mp, fs](Task& t) {
                                Partition* q = Partitions::get().find(mp);
                                return q && Partitions::get().format(*q, t, fs);
                            });
                        });
                    };
                    app.dialog(std::move(d));
                    break;
                }
            }
        }
        y += c.h + ui.dp(16);
        ui.endScroll(y);
    }

private:
    std::string mp_;
};

}  // namespace

std::unique_ptr<Screen> makeFsToolsScreen(const std::string& mp) { return std::make_unique<FsToolsScreen>(mp); }

}  // namespace tw
