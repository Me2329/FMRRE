#include "ui/screens/screens.h"

#include <ctime>
#include <map>
#include <set>

#include "core/settings.h"
#include "core/util.h"
#include "platform/device.h"
#include "sys/backup.h"
#include "sys/partitions.h"

namespace tw {

namespace {

class BackupScreen : public Screen {
public:
    BackupScreen() {
        name_ = util::dateStamp() + "_" + device::info().device;
        for (auto& p : Partitions::get().all())
            if (p.backup && p.present && (p.mountPoint == "/data" || p.mountPoint == "/boot" || p.mountPoint == "/init_boot"))
                sel_.insert(p.mountPoint);
    }
    std::string title() const override { return "Backup"; }
    std::string subtitle() const override { return Partitions::get().find(Partitions::get().currentStorage()) ?
        "to " + Partitions::get().find(Partitions::get().currentStorage())->displayName + " storage" : ""; }

    void onShow(App&) override {
        Partitions::get().refresh();
        sizes_.clear();
        for (auto& p : Partitions::get().all())
            if (p.backup && p.present) sizes_[p.mountPoint] = p.isImage() ? p.size : p.used;
    }

    void draw(Ui& ui, App& app, Rect area) override {
        Theme& th = ui.th;
        Settings& s = Settings::get();
        float pad = ui.dp(16);
        Rect bottom = area.cutBottom(ui.dp(88));
        ui.beginScroll(ui.id("bk_scroll"), area);
        float x = area.x + pad, w = area.w - pad * 2, y = area.y;

        // Name
        Rect nm(x, y, w, ui.dp(70));
        ui.card(nm, ui.dp(20));
        if (ui.listItem(ui.id("bk_name"), nm.inset(ui.dp(4)), Icon::Edit, "Backup name", name_, "", th.accent) & Clicked) {
            app.push(makeTextInputScreen("Backup name", "Name", name_, [this](App&, const std::string& v) {
                if (!v.empty()) name_ = v;
            }));
        }
        y += nm.h + ui.dp(18);

        ui.sectionHeader(Rect(x, y, w, ui.dp(24)), "Partitions");
        y += ui.dp(30);
        std::vector<Partition*> list;
        for (auto& p : Partitions::get().all())
            if (p.backup && p.present) list.push_back(&p);
        Rect card(x, y, w, ui.dp(62) * list.size() + ui.dp(8));
        ui.card(card);
        float iy = y + ui.dp(4);
        uint64_t total = 0;
        for (auto* p : list) {
            bool on = sel_.count(p->mountPoint) > 0;
            uint64_t sz = sizes_.count(p->mountPoint) ? sizes_[p->mountPoint] : 0;
            if (on) total += sz;
            std::string sub = p->isImage() ? "image  \xE2\x80\xA2  " + util::basename(p->blockDev) : p->fsType + "  \xE2\x80\xA2  files";
            if (p->mountPoint == "/data") sub += " (excl. internal storage)";
            if (ui.checkRow(ui.id(p->mountPoint), Rect(x + ui.dp(4), iy, w - ui.dp(8), ui.dp(58)), p->displayName, sub, on,
                            sz ? util::humanSize(sz) : "")) {
                if (on) sel_.insert(p->mountPoint); else sel_.erase(p->mountPoint);
            }
            iy += ui.dp(62);
        }
        y += card.h + ui.dp(18);

        ui.sectionHeader(Rect(x, y, w, ui.dp(24)), "Options");
        y += ui.dp(30);
        Rect opt(x, y, w, ui.dp(68) * 3 + ui.dp(8));
        ui.card(opt);
        bool comp = s.flag("backup_compress", true), dig = s.flag("backup_digest", true), enc = s.flag("backup_encrypt", false);
        if (ui.toggleRow(ui.id("comp"), Rect(x + ui.dp(4), y + ui.dp(4), w - ui.dp(8), ui.dp(64)), Icon::Zip, "Compression",
                         "gzip - smaller backups, slightly slower", comp))
            s.setFlag("backup_compress", comp), s.save();
        if (ui.toggleRow(ui.id("dig"), Rect(x + ui.dp(4), y + ui.dp(72), w - ui.dp(8), ui.dp(64)), Icon::Shield, "SHA-256 digests",
                         "Verify integrity before restoring", dig))
            s.setFlag("backup_digest", dig), s.save();
        if (ui.toggleRow(ui.id("enc"), Rect(x + ui.dp(4), y + ui.dp(140), w - ui.dp(8), ui.dp(64)), Icon::Lock, "Encrypt with a password",
                         "AES-256 - the password is needed to restore", enc))
            s.setFlag("backup_encrypt", enc), s.save();
        y += opt.h + ui.dp(18);

        ui.sectionHeader(Rect(x, y, w, ui.dp(24)), "Storage");
        y += ui.dp(30);
        float cx = x;
        auto storages = Partitions::get().storages();
        for (size_t i = 0; i < storages.size(); i++) {
            float cw = ui.chipWidth(storages[i]->displayName, Icon::Storage);
            if (ui.chip(ui.id("st", (int)i), Rect(cx, y, cw, ui.dp(40)), storages[i]->displayName,
                        Partitions::get().currentStorage() == storages[i]->mountPoint, Icon::Storage))
                Partitions::get().setStorage(storages[i]->mountPoint);
            cx += cw + ui.dp(8);
        }
        y += ui.dp(52);
        Partition* st = Partitions::get().find(Partitions::get().currentStorage());
        if (st) {
            ui.label(Rect(x + ui.dp(6), y, w, ui.dp(20)),
                     "Estimated " + util::humanSize(total) + "  \xE2\x80\xA2  " + util::humanSize(st->avail) + " free on " + st->displayName,
                     ui.dp(13), total > st->avail && st->avail ? th.danger : th.textDim);
            y += ui.dp(30);
        }
        ui.endScroll(y);

        Rect sb = bottom.inset(pad, ui.dp(14));
        if (sel_.empty()) {
            ui.c->fillRoundRect(sb, sb.h / 2, th.surface2);
            ui.label(sb, "Select at least one partition", ui.dp(15), th.textFaint, Face::SemiBold, AlignCenter);
        } else if (ui.slideToConfirm(ui.id("bk_go"), sb, "Swipe to back up", th.info)) {
            backup::Options o;
            for (auto& p : Partitions::get().all())
                if (sel_.count(p.mountPoint)) o.parts.push_back(p.mountPoint);
            o.name = name_;
            o.compress = s.flag("backup_compress", true);
            o.digest = s.flag("backup_digest", true);
            std::string title = "Backup " + name_;
            name_ = util::dateStamp() + "_" + device::info().device;
            if (!s.flag("backup_encrypt", false)) {
                app.runTask(title, Icon::Backup, [o](Task& t) { return backup::create(o, t); });
                return;
            }
            // Asked twice: a typo here would make the backup unrecoverable.
            app.push(makeTextInputScreen("Backup password", "At least 4 characters", "", [o, title](App& a, const std::string& p1) {
                if (p1.size() < 4) { a.toast("Use at least 4 characters", Icon::Warning, a.ui.th.warn); return; }
                a.push(makeTextInputScreen("Repeat the password", "", "", [o, title, p1](App& b, const std::string& p2) {
                    if (p1 != p2) { b.toast("The passwords don't match", Icon::Warning, b.ui.th.danger); return; }
                    backup::Options oo = o;
                    oo.password = p1;
                    b.runTask(title, Icon::Backup, [oo](Task& t) { return backup::create(oo, t); });
                }, true));
            }, true));
        }
    }

private:
    std::string name_;
    std::set<std::string> sel_;
    std::map<std::string, uint64_t> sizes_;
};

class RestoreDetailScreen : public Screen {
public:
    explicit RestoreDetailScreen(backup::Set s) : set_(std::move(s)) {
        for (auto& i : set_.items) sel_.insert(i.part);
    }
    std::string title() const override { return "Restore"; }
    std::string subtitle() const override { return set_.name; }

    void draw(Ui& ui, App& app, Rect area) override {
        Theme& th = ui.th;
        float pad = ui.dp(16);
        Rect bottom = area.cutBottom(ui.dp(88));
        ui.beginScroll(ui.id("rd_scroll"), area);
        float x = area.x + pad, w = area.w - pad * 2, y = area.y;
        Rect info(x, y, w, ui.dp(92));
        ui.card(info, ui.dp(22));
        char date[64];
        time_t tt = (time_t)set_.mtime;
        struct tm tm;
        localtime_r(&tt, &tm);
        strftime(date, sizeof(date), "%d %B %Y, %H:%M", &tm);
        icons::drawCentered(*ui.c, Icon::Restore, info.x + ui.dp(36), info.cy(), ui.dp(28), th.ok);
        ui.label(Rect(info.x + ui.dp(66), info.y + ui.dp(18), info.w - ui.dp(80), ui.dp(24)), date, ui.dp(16), th.text, Face::SemiBold);
        ui.label(Rect(info.x + ui.dp(66), info.y + ui.dp(46), info.w - ui.dp(80), ui.dp(20)),
                 util::humanSize(set_.size) + "  \xE2\x80\xA2  " + std::to_string(set_.items.size()) + " partition(s)" +
                     (set_.hasManifest ? "  \xE2\x80\xA2  TWRP v2" : "  \xE2\x80\xA2  classic TWRP"),
                 ui.dp(13), th.textDim);
        y += info.h + ui.dp(18);
        ui.sectionHeader(Rect(x, y, w, ui.dp(24)), "Restore these");
        y += ui.dp(30);
        Rect card(x, y, w, ui.dp(62) * set_.items.size() + ui.dp(8));
        ui.card(card);
        float iy = y + ui.dp(4);
        for (size_t k = 0; k < set_.items.size(); k++) {
            auto& it = set_.items[k];
            bool on = sel_.count(it.part) > 0;
            Partition* p = Partitions::get().find(it.part);
            if (!p) p = Partitions::get().find("/" + it.part);
            std::string sub = it.file + (it.sha256.empty() ? "" : "  \xE2\x80\xA2  sha256");
            if (ui.checkRow(ui.id("ri", (int)k), Rect(x + ui.dp(4), iy, w - ui.dp(8), ui.dp(58)), p ? p->displayName : it.part, sub, on,
                            util::humanSize(it.size))) {
                if (on) sel_.insert(it.part); else sel_.erase(it.part);
            }
            iy += ui.dp(62);
        }
        y += card.h + ui.dp(14);
        Settings& s = Settings::get();
        if (backup::encrypted(set_)) {
            Rect er(x, y, w, ui.dp(44));
            ui.c->fillRoundRect(er, ui.dp(14), withAlpha(th.info, 0.12f));
            icons::drawCentered(*ui.c, Icon::Lock, x + ui.dp(22), er.cy(), ui.dp(16), th.info);
            ui.label(Rect(x + ui.dp(40), y, w - ui.dp(48), er.h), "Encrypted - you'll be asked for the password", ui.dp(13), th.text);
            y += er.h + ui.dp(10);
        }
        Rect opt(x, y, w, ui.dp(72));
        ui.card(opt);
        bool ver = s.flag("restore_verify", true);
        if (ui.toggleRow(ui.id("rv"), opt.inset(ui.dp(4)), Icon::Shield, "Verify digests first", "Abort if the backup is corrupt", ver))
            s.setFlag("restore_verify", ver), s.save();
        y += opt.h + ui.dp(14);
        float bw = (w - ui.dp(10)) / 2;
        backup::Set copy = set_;
        if (ui.button(ui.id("rverify"), Rect(x, y, bw, ui.dp(50)), "Verify only", Icon::Shield, BtnStyle::Tonal))
            app.runTask("Verify " + set_.name, Icon::Shield, [copy](Task& t) { return backup::verify(copy, t); });
        if (ui.button(ui.id("rdel"), Rect(x + bw + ui.dp(10), y, bw, ui.dp(50)), "Delete", Icon::Trash, BtnStyle::Danger)) {
            std::string path = set_.path;
            app.confirm("Delete backup?", set_.name, "Swipe to delete", [path, &app]() {
                util::removeTree(path);
                app.toast("Backup deleted", Icon::Trash);
                app.pop();
            });
        }
        y += ui.dp(64);
        ui.endScroll(y);
        Rect sb = bottom.inset(pad, ui.dp(14));
        if (sel_.empty()) {
            ui.c->fillRoundRect(sb, sb.h / 2, th.surface2);
            ui.label(sb, "Nothing selected", ui.dp(15), th.textFaint, Face::SemiBold, AlignCenter);
        } else if (ui.slideToConfirm(ui.id("r_go"), sb, "Swipe to restore", th.ok)) {
            std::vector<std::string> parts(sel_.begin(), sel_.end());
            bool v = s.flag("restore_verify", true);
            std::string title = "Restore " + set_.name;
            if (!backup::encrypted(set_)) {
                app.runTask(title, Icon::Restore, [copy, parts, v](Task& t) { return backup::restore(copy, parts, v, t); });
                return;
            }
            app.push(makeTextInputScreen("Backup password", "", "", [copy, parts, v, title](App& a, const std::string& pw) {
                std::string err;
                if (!backup::checkPassword(copy, pw, err)) { a.toast(err.find("wrong password") != std::string::npos ? "Wrong password" : err, Icon::Lock, a.ui.th.danger); return; }
                a.runTask(title, Icon::Restore, [copy, parts, v, pw](Task& t) { return backup::restore(copy, parts, v, t, pw); });
            }, true));
        }
    }

private:
    backup::Set set_;
    std::set<std::string> sel_;
};

class RestoreScreen : public Screen {
public:
    std::string title() const override { return "Restore"; }
    std::string subtitle() const override { return std::to_string(sets_.size()) + " backup(s) found"; }
    void onShow(App&) override { sets_ = backup::list(); }

    void draw(Ui& ui, App& app, Rect area) override {
        Theme& th = ui.th;
        float pad = ui.dp(16);
        ui.beginScroll(ui.id("rs_scroll"), area);
        float x = area.x + pad, w = area.w - pad * 2, y = area.y;
        if (sets_.empty()) {
            Rect e(x, y + ui.dp(20), w, ui.dp(200));
            ui.card(e);
            ui.c->fillCircle(e.cx(), e.y + ui.dp(60), ui.dp(32), withAlpha(th.ok, 0.15f));
            icons::drawCentered(*ui.c, Icon::Restore, e.cx(), e.y + ui.dp(60), ui.dp(32), th.ok);
            ui.label(Rect(e.x, e.y + ui.dp(108), e.w, ui.dp(26)), "No backups yet", ui.dp(17), th.text, Face::SemiBold, AlignCenter);
            ui.label(Rect(e.x + ui.dp(20), e.y + ui.dp(136), e.w - ui.dp(40), ui.dp(20)), "Looking in " + util::basename(backup::backupRoot()),
                     ui.dp(13), th.textDim, Face::Regular, AlignCenter);
            y += ui.dp(240);
        }
        for (size_t i = 0; i < sets_.size(); i++) {
            auto& s = sets_[i];
            Rect r(x, y, w, ui.dp(96));
            ui.card(r, ui.dp(22));
            std::string parts;
            for (auto& it : s.items) parts += (parts.empty() ? "" : ", ") + it.part;
            if (backup::encrypted(s)) parts += "  \xE2\x80\xA2  encrypted";
            if (ui.listItem(ui.id("set", (int)i), r.inset(ui.dp(4)), Icon::Restore, s.name, parts.empty() ? "empty" : parts,
                            util::humanSize(s.size), th.ok) & Clicked)
                app.push(std::make_unique<RestoreDetailScreen>(s));
            y += r.h + ui.dp(10);
        }
        ui.endScroll(y + ui.dp(8));
    }

private:
    std::vector<backup::Set> sets_;
};

}  // namespace

std::unique_ptr<Screen> makeBackupScreen() { return std::make_unique<BackupScreen>(); }
std::unique_ptr<Screen> makeRestoreScreen() { return std::make_unique<RestoreScreen>(); }

}  // namespace tw
