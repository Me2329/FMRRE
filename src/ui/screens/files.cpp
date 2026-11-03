#include "ui/screens/screens.h"

#include <algorithm>
#include <ctime>
#include <set>
#include <sys/stat.h>

#include "core/log.h"
#include "core/sha256.h"
#include "core/util.h"
#include "platform/device.h"
#include "sys/imagesrc.h"
#include "sys/installer.h"
#include "sys/payload.h"
#include "sys/partitions.h"

namespace tw {

Icon iconForFile(const std::string& name, bool dir) {
    if (dir) return Icon::Folder;
    std::string n = util::toLower(name);
    if (util::endsWith(n, ".img.xz") || util::endsWith(n, ".img.gz") || util::endsWith(n, ".img.zst") || util::endsWith(n, ".img.lz4"))
        return Icon::Chip;
    if (util::endsWith(n, ".zip") || util::endsWith(n, ".apk") || util::endsWith(n, ".gz") || util::endsWith(n, ".tar") ||
        util::endsWith(n, ".7z") || util::endsWith(n, ".win") || util::endsWith(n, ".xz"))
        return Icon::Zip;
    if (util::endsWith(n, ".img") || util::endsWith(n, ".bin") || util::endsWith(n, ".mbn") || util::endsWith(n, ".img.xz") ||
        util::endsWith(n, ".img.gz") || util::endsWith(n, ".img.zst") || util::endsWith(n, ".img.lz4"))
        return Icon::Chip;
    if (util::endsWith(n, ".png") || util::endsWith(n, ".jpg") || util::endsWith(n, ".jpeg") || util::endsWith(n, ".webp"))
        return Icon::Image;
    if (util::endsWith(n, ".sh") || util::endsWith(n, ".rc") || util::endsWith(n, ".prop")) return Icon::Terminal;
    if (util::endsWith(n, ".log") || util::endsWith(n, ".txt")) return Icon::Console;
    return Icon::File;
}

Color colorForFile(const Theme& th, const std::string& name, bool dir) {
    switch (iconForFile(name, dir)) {
        case Icon::Folder: return th.warn;
        case Icon::Zip: return th.accent;
        case Icon::Chip: return th.danger;
        case Icon::Image: return th.ok;
        case Icon::Terminal: return th.info;
        default: return th.textDim;
    }
}

namespace {

struct Clipboard {
    std::vector<std::string> paths;
    bool cut = false;
};
Clipboard g_clip;

std::string displayPath(const std::string& p) {
    if (device::isSim() && util::startsWith(p, device::simRoot())) {
        std::string r = p.substr(device::simRoot().size());
        return r.empty() ? "/" : r;
    }
    return p;
}

std::string fmtDate(int64_t t) {
    char buf[32];
    time_t tt = (time_t)t;
    struct tm tm;
    localtime_r(&tt, &tm);
    strftime(buf, sizeof(buf), "%d %b %Y, %H:%M", &tm);
    return buf;
}

class FilesScreen : public Screen {
public:
    FilesScreen(std::string start, std::vector<std::string> filter, PickFn onPick, std::string title, AppFn onCantFind)
        : filter_(std::move(filter)), onPick_(std::move(onPick)), title_(std::move(title)), onCantFind_(std::move(onCantFind)) {
        cwd_ = start.empty() ? Partitions::get().storagePath() : start;
        if (!util::isDir(cwd_)) cwd_ = device::path("/");
        load();
    }
    std::string title() const override { return title_.empty() ? "Files" : title_; }
    std::string subtitle() const override {
        if (!sel_.empty()) return std::to_string(sel_.size()) + " selected";
        return std::to_string(entries_.size()) + " items";
    }
    void onShow(App&) override { load(); }

    bool onBack(App&) override {
        if (!sel_.empty()) { sel_.clear(); return true; }
        if (canGoUp()) { cd(util::dirname(cwd_)); return true; }
        return false;
    }

    float headerActionsWidth() const override { return 96; }
    void headerActions(Ui& ui, App& app, Rect r) override {
        float s = ui.dp(44);
        Rect b(r.r() - s, r.cy() - s / 2, s, s);
        if (ui.iconButton(ui.id("f_sort"), b, Icon::Sort, ui.th.text, true)) {
            sort_ = (sort_ + 1) % 3;
            load();
            const char* names[] = {"name", "size", "date"};
            app.toast(std::string("Sorted by ") + names[sort_], Icon::Sort);
        }
        b.x -= s + ui.dp(8);
        if (ui.iconButton(ui.id("f_new"), b, Icon::Plus, ui.th.text, true)) {
            std::string dir = cwd_;
            app.push(makeTextInputScreen("New folder", "Folder name", "", [dir](App& a, const std::string& name) {
                if (name.empty()) return;
                if (util::mkdirs(util::joinPath(dir, name))) a.toast("Folder created", Icon::Folder);
                else a.toast("Couldn't create folder", Icon::Warning, a.ui.th.danger);
            }));
        }
    }

    bool canGoUp() const {
        if (device::isSim()) return cwd_.size() > device::simRoot().size() && cwd_ != device::simRoot() + "/";
        return cwd_ != "/";
    }

    void cd(const std::string& p) {
        if (!util::isDir(p)) return;
        cwd_ = p;
        sel_.clear();
        scrollReset_ = true;
        load();
    }

    void load() {
        entries_ = util::listDir(cwd_, showHidden_);
        hiddenByFilter_ = 0;
        if (!filter_.empty() && !showAll_) {
            size_t before = entries_.size();
            entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                          [&](const util::DirEntry& e) {
                                              if (e.dir) return false;
                                              std::string n = util::toLower(e.name);
                                              for (auto& f : filter_)
                                                  if (util::endsWith(n, f)) return false;
                                              return true;
                                          }),
                           entries_.end());
            hiddenByFilter_ = int(before - entries_.size());
        }
        int s = sort_;
        std::sort(entries_.begin(), entries_.end(), [s](const util::DirEntry& a, const util::DirEntry& b) {
            if (a.dir != b.dir) return a.dir;
            if (s == 1 && a.size != b.size) return a.size > b.size;
            if (s == 2 && a.mtime != b.mtime) return a.mtime > b.mtime;
            return util::toLower(a.name) < util::toLower(b.name);
        });
    }

    void fileActions(App& app, const util::DirEntry& e) {
        std::string path = util::joinPath(cwd_, e.name);
        std::string n = util::toLower(e.name);
        Dialog d;
        d.title = e.name;
        d.message = util::humanSize(e.size) + "  \xE2\x80\xA2  " + fmtDate(e.mtime);
        struct A { Icon i; std::string label; int code; };
        std::vector<A> acts;
        bool ota = util::endsWith(n, ".zip") && payload::isOtaZip(path);
        if (util::endsWith(n, ".zip")) acts.push_back({Icon::Install, ota ? "Install A/B ROM / OTA" : "Install zip", 1});
        if (ota || n == "payload.bin") acts.push_back({Icon::Download, "Extract images from payload", 13});
        if (img::isImageName(n)) acts.push_back({Icon::Bolt, "Flash image", 2});
        if (util::endsWith(n, ".zip") && !img::inspect(path).zipImages.empty()) acts.push_back({Icon::Bolt, "Flash .img inside zip", 2});
        if (img::isImageName(n) || util::endsWith(n, ".zip")) acts.push_back({Icon::Phone, "Flash as GSI (system)", 11});
        acts.push_back({Icon::Terminal, "dd command for this file", 12});
        acts.push_back({Icon::Eye, "View", 3});
        acts.push_back({Icon::Hash, "SHA-256 checksum", 4});
        acts.push_back({Icon::Copy, "Copy", 5});
        acts.push_back({Icon::Move, "Move", 6});
        acts.push_back({Icon::Edit, "Rename", 7});
        acts.push_back({Icon::Key, "chmod 755", 8});
        acts.push_back({Icon::Key, "chmod 644", 9});
        acts.push_back({Icon::Trash, "Delete", 10});
        for (auto& a : acts) d.actions.push_back({a.i, a.label});
        std::string dir = cwd_;
        d.onAction = [acts, path, dir, this](int idx) {
            if (idx < 0) return;
            App& a = *app_;
            switch (acts[idx].code) {
                case 1: {
                    if (payload::isOtaZip(path)) { a.push(makePayloadScreen(path)); break; }
                    installer::InstallOptions o;
                    a.runTask("Flashing " + util::basename(path), Icon::Install,
                              [path, o](Task& t) { return installer::installZip(path, o, t); });
                    break;
                }
                case 2: a.push(makeFlashImageScreen(path)); break;
                case 3: a.push(makeTextViewerScreen(path)); break;
                case 11: a.push(makeGsiScreen(path)); break;
                case 12: a.push(makeManualFlashScreen(path)); break;
                case 13: a.push(makePayloadScreen(path)); break;
                case 4:
                    a.runTask("SHA-256 of " + util::basename(path), Icon::Hash, [path](Task& t) {
                        t.setStatus("Hashing " + util::basename(path));
                        struct C { Task* t; } c{&t};
                        std::string h = Sha256::hexOfFile(
                            path, [](uint64_t d, uint64_t tot, void* cx) { ((C*)cx)->t->setSubProgress(float(d) / tot); }, &c);
                        t.print("SHA-256 (%s):", util::basename(path).c_str());
                        t.print("%s", h.substr(0, 32).c_str());
                        t.print("%s", h.substr(32).c_str());
                        util::writeFile(path + ".sha256", h + "  " + util::basename(path) + "\n");
                        t.print("Saved to %s.sha256", util::basename(path).c_str());
                        return !h.empty();
                    });
                    break;
                case 5: g_clip = {{path}, false}; a.toast("Copied - open a folder and paste", Icon::Copy); break;
                case 6: g_clip = {{path}, true}; a.toast("Cut - open a folder and paste", Icon::Move); break;
                case 7:
                    a.push(makeTextInputScreen("Rename", "New name", util::basename(path), [path, dir](App& ap, const std::string& nn) {
                        if (nn.empty()) return;
                        if (rename(path.c_str(), util::joinPath(dir, nn).c_str()) == 0) ap.toast("Renamed", Icon::Edit);
                        else ap.toast("Rename failed", Icon::Warning, ap.ui.th.danger);
                    }));
                    break;
                case 8: chmod(path.c_str(), 0755); a.toast("Permissions set to 755", Icon::Key); load(); break;
                case 9: chmod(path.c_str(), 0644); a.toast("Permissions set to 644", Icon::Key); load(); break;
                case 10:
                    a.confirm("Delete file?", util::basename(path) + " will be permanently deleted.", "Swipe to delete", [path, this]() {
                        util::removeTree(path);
                        app_->toast("Deleted", Icon::Trash);
                        load();
                    });
                    break;
            }
        };
        app.dialog(std::move(d));
    }

    void draw(Ui& ui, App& app, Rect area) override {
        app_ = &app;
        Theme& th = ui.th;
        Canvas& c = *ui.c;
        float pad = ui.dp(16);
        // Path bar
        Rect pb = area.cutTop(ui.dp(56)).inset(pad, ui.dp(6));
        c.fillRoundRect(pb, pb.h / 2, th.surface);
        Rect upB(pb.x + ui.dp(4), pb.y + ui.dp(4), pb.h - ui.dp(8), pb.h - ui.dp(8));
        if (canGoUp()) {
            if (ui.iconButton(ui.id("f_up"), upB, Icon::Up, th.accent)) cd(util::dirname(cwd_));
        } else {
            icons::drawCentered(c, Icon::Storage, upB.cx(), upB.cy(), ui.dp(18), th.textFaint);
        }
        font::drawIn(c, Face::Mono, ui.dp(13), Rect(upB.r() + ui.dp(8), pb.y, pb.w - upB.w - ui.dp(24), pb.h), displayPath(cwd_),
                     th.text);
        // Quick locations
        Rect qr = area.cutTop(ui.dp(48));
        struct Q { std::string label; std::string path; Icon icon; };
        std::vector<Q> qs = {{"Internal", Partitions::get().storagePath(), Icon::Phone},
                             {"Root", device::path("/"), Icon::Hash},
                             {"tmp", device::path("/tmp"), Icon::Folder}};
        if (auto* sd = Partitions::get().find("/external_sd"))
            if (sd->present) qs.push_back({"MicroSD", sd->contentPath(), Icon::Sd});
        qs.push_back({"Backups", Partitions::get().storagePath() + "/TWRP", Icon::Backup});
        float qx = pad;
        if (!filter_.empty()) {
            float w = ui.chipWidth("All files", Icon::Eye);
            if (ui.chip(ui.id("q_all"), Rect(qx, qr.y + ui.dp(4), w, ui.dp(36)), "All files", showAll_, Icon::Eye)) {
                showAll_ = !showAll_;
                load();
            }
            qx += w + ui.dp(8);
        }
        for (size_t i = 0; i < qs.size(); i++) {
            float w = ui.chipWidth(qs[i].label, qs[i].icon);
            if (ui.chip(ui.id("q", (int)i), Rect(qx, qr.y + ui.dp(4), w, ui.dp(36)), qs[i].label, util::startsWith(cwd_, qs[i].path) && i != 1, qs[i].icon)) {
                util::mkdirs(qs[i].path);
                cd(qs[i].path);
            }
            qx += w + ui.dp(8);
        }
        // Bottom bars
        bool selMode = !sel_.empty();
        bool clipMode = !g_clip.paths.empty() && !selMode && !onPick_;
        Rect bar;
        if (selMode || clipMode) bar = area.cutBottom(ui.dp(76));

        Id sid = ui.id("files_scroll");
        if (scrollReset_) { ui.resetScroll(sid); scrollReset_ = false; }
        ui.beginScroll(sid, area);
        float y = area.y + ui.dp(4);
        float rowH = ui.dp(64);
        float x = area.x + ui.dp(8), w = area.w - ui.dp(16);
        if (hiddenByFilter_ > 0) {
            Rect hb(x + ui.dp(4), y, w - ui.dp(8), ui.dp(40));
            c.fillRoundRect(hb, ui.dp(12), th.accentSoft);
            int hr = ui.clickable(ui.id("f_hidden"), hb);
            font::drawIn(c, Face::SemiBold, ui.dp(12.5f), hb.inset(ui.dp(14), 0),
                         std::to_string(hiddenByFilter_) + " other file(s) hidden by the type filter - tap to show all", th.accent);
            if (hr & Clicked) { showAll_ = true; load(); }
            y += ui.dp(48);
        }
        if (entries_.empty()) {
            icons::drawCentered(c, Icon::Folder, area.cx(), y + ui.dp(70), ui.dp(48), th.textFaint);
            ui.label(Rect(area.x, y + ui.dp(108), area.w, ui.dp(24)), filter_.empty() ? "This folder is empty" : "No matching files here",
                     ui.dp(15), th.textDim, Face::SemiBold, AlignCenter);
            y += ui.dp(160);
        }
        Rect clip = c.clipLocal();
        for (size_t i = 0; i < entries_.size(); i++) {
            auto& e = entries_[i];
            Rect row(x, y, w, rowH - ui.dp(2));
            y += rowH;
            if (row.b() < clip.y || row.y > clip.b()) continue;  // cull offscreen rows
            std::string full = util::joinPath(cwd_, e.name);
            bool selected = sel_.count(full) > 0;
            if (selected) c.fillRoundRect(row, ui.dp(16), th.accentSoft);
            std::string sub = e.dir ? fmtDate(e.mtime) : util::humanSize(e.size) + "  \xE2\x80\xA2  " + fmtDate(e.mtime);
            if (e.link) sub = "link  \xE2\x80\xA2  " + sub;
            int res = ui.listItem(ui.id("e", (int64_t)std::hash<std::string>()(full)), row, selected ? Icon::Check : iconForFile(e.name, e.dir),
                                  e.name, sub, "", selected ? th.accent : colorForFile(th, e.name, e.dir), e.dir);
            if (res & LongPressed) {
                if (selected) sel_.erase(full); else sel_.insert(full);
                app.vibrate(30);
            } else if (res & Clicked) {
                if (selMode) {
                    if (selected) sel_.erase(full); else sel_.insert(full);
                } else if (e.dir) {
                    cd(full);
                    break;
                } else if (onPick_) {
                    onPick_(app, full);
                } else {
                    fileActions(app, e);
                }
            }
        }
        if (onCantFind_) {
            Rect cf(x + ui.dp(4), y + ui.dp(8), w - ui.dp(8), ui.dp(66));
            ui.card(cf, ui.dp(18), th.surface2, false);
            if (ui.listItem(ui.id("f_cantfind"), cf.inset(ui.dp(2)), Icon::Search, "Can't find your file?",
                            "Type its path - get a checked dd command", "", th.warn) & Clicked)
                onCantFind_(app);
            y += ui.dp(82);
        }
        ui.endScroll(y + ui.dp(8));

        if (selMode) {
            Rect b = bar.inset(pad, ui.dp(10));
            ui.card(b, b.h / 2, th.surface2);
            float bw = b.w / 4;
            struct BA { Icon i; const char* l; };
            BA acts[4] = {{Icon::Copy, "Copy"}, {Icon::Move, "Move"}, {Icon::Trash, "Delete"}, {Icon::Close, "Clear"}};
            for (int k = 0; k < 4; k++) {
                Rect r(b.x + k * bw, b.y, bw, b.h);
                Id bid = ui.id("selact", k);
                int res = ui.clickable(bid, r.inset(ui.dp(4)));
                ui.drawPressFx(bid, r.inset(ui.dp(4)), ui.dp(20), th.text);
                Color col = k == 2 ? th.danger : th.text;
                icons::drawCentered(c, acts[k].i, r.cx(), r.y + ui.dp(20), ui.dp(20), col);
                font::drawIn(c, Face::SemiBold, ui.dp(11), Rect(r.x, r.y + ui.dp(32), r.w, ui.dp(16)), acts[k].l, col, AlignCenter);
                if (res & Clicked) {
                    std::vector<std::string> items(sel_.begin(), sel_.end());
                    if (k == 0 || k == 1) {
                        g_clip = {items, k == 1};
                        sel_.clear();
                        app.toast(std::to_string(items.size()) + (k == 1 ? " item(s) cut" : " item(s) copied"), acts[k].i);
                    } else if (k == 2) {
                        app.confirm("Delete " + std::to_string(items.size()) + " item(s)?", "This cannot be undone.", "Swipe to delete",
                                    [items, this]() {
                                        sel_.clear();
                                        app_->runTask("Deleting files", Icon::Trash, [items](Task& t) {
                                            for (size_t n = 0; n < items.size(); n++) {
                                                t.print("rm %s", util::basename(items[n]).c_str());
                                                util::removeTree(items[n]);
                                                t.setProgress(float(n + 1) / items.size());
                                            }
                                            return true;
                                        });
                                    });
                    } else {
                        sel_.clear();
                    }
                }
            }
        } else if (clipMode) {
            Rect b = bar.inset(pad, ui.dp(12));
            float cw = ui.dp(52);
            std::string lab = "Paste " + std::to_string(g_clip.paths.size()) + " item(s) here";
            if (ui.button(ui.id("paste"), Rect(b.x, b.y, b.w - cw - ui.dp(8), b.h), lab, g_clip.cut ? Icon::Move : Icon::Copy)) {
                auto clip2 = g_clip;
                g_clip = {};
                std::string dst = cwd_;
                app.runTask(clip2.cut ? "Moving files" : "Copying files", clip2.cut ? Icon::Move : Icon::Copy, [clip2, dst](Task& t) {
                    bool ok = true;
                    for (size_t n = 0; n < clip2.paths.size(); n++) {
                        const std::string& src = clip2.paths[n];
                        std::string to = util::joinPath(dst, util::basename(src));
                        t.setStatus(util::basename(src));
                        t.print("%s %s", clip2.cut ? "mv" : "cp", util::basename(src).c_str());
                        t.setRange(float(n) / clip2.paths.size(), 1.0f / clip2.paths.size());
                        if (clip2.cut && rename(src.c_str(), to.c_str()) == 0) continue;
                        if (util::isDir(src)) {
                            std::string out;
                            int rc = util::runCapture("cp -a " + util::shellQuote(src) + " " + util::shellQuote(dst) + "/", out);
                            if (rc != 0) { t.print("  failed: %s", out.c_str()); ok = false; continue; }
                        } else if (!util::copyFile(src, to, [&](uint64_t d, uint64_t tot) { if (tot) t.setSubProgress(float(d) / tot); })) {
                            t.print("  failed");
                            ok = false;
                            continue;
                        }
                        if (clip2.cut) util::removeTree(src);
                    }
                    return ok;
                });
            }
            if (ui.iconButton(ui.id("paste_x"), Rect(b.r() - cw, b.y, cw, b.h), Icon::Close, th.text, true)) g_clip = {};
        }
    }

private:
    std::string cwd_;
    std::vector<util::DirEntry> entries_;
    std::vector<std::string> filter_;
    PickFn onPick_;
    std::string title_;
    AppFn onCantFind_;
    std::set<std::string> sel_;
    int sort_ = 0;
    bool showAll_ = false;
    int hiddenByFilter_ = 0;
    bool showHidden_ = true;
    bool scrollReset_ = false;
    App* app_ = nullptr;
};

class TextViewerScreen : public Screen {
public:
    explicit TextViewerScreen(std::string path) : path_(std::move(path)) {
        std::string data;
        util::readFile(path_, data, 256 * 1024);
        binary_ = data.find('\0') != std::string::npos;
        if (binary_) {
            // hexdump of the first 4 KiB
            static const char* hx = "0123456789abcdef";
            for (size_t off = 0; off < std::min<size_t>(data.size(), 4096); off += 8) {
                char addr[16];
                snprintf(addr, sizeof(addr), "%06zx  ", off);
                std::string line = addr, asc;
                for (size_t k = 0; k < 8; k++) {
                    if (off + k < data.size()) {
                        unsigned char ch = data[off + k];
                        line += hx[ch >> 4];
                        line += hx[ch & 15];
                        line += ' ';
                        asc += (ch >= 32 && ch < 127) ? (char)ch : '.';
                    } else {
                        line += "   ";
                    }
                }
                lines_.push_back(line + " " + asc);
            }
        } else {
            lines_ = util::split(data, '\n', false);
        }
    }
    std::string title() const override { return util::basename(path_); }
    std::string subtitle() const override {
        return util::humanSize(util::fileSize(path_)) + (binary_ ? "  \xE2\x80\xA2  hex view" : "  \xE2\x80\xA2  " +
                                                         std::to_string(lines_.size()) + " lines");
    }

    float headerActionsWidth() const override { return 44; }
    void headerActions(Ui& ui, App&, Rect r) override {
        float s = ui.dp(44);
        if (ui.iconButton(ui.id("tv_wrap"), Rect(r.r() - s, r.cy() - s / 2, s, s), Icon::Sort, wrap_ ? ui.th.accent : ui.th.text, true))
            wrap_ = !wrap_;
    }

    void draw(Ui& ui, App&, Rect area) override {
        Theme& th = ui.th;
        Rect box = area.inset(ui.dp(12), ui.dp(4));
        ui.card(box, ui.dp(18), th.light ? rgb(0xFFFFFF) : mix(th.bgBottom, rgb(0x000000), 0.3f), false);
        Rect body = box.inset(ui.dp(10));
        float fs = ui.dp(12.5f);
        float lh = font::lineHeight(Face::Mono, fs);
        float gutter = binary_ ? 0 : ui.dp(40);
        ui.beginScroll(ui.id("tv_scroll"), body);
        float y = body.y;
        Rect clip = ui.c->clipLocal();
        for (size_t i = 0; i < lines_.size(); i++) {
            std::vector<std::string> wl = wrap_ ? font::wrap(Face::Mono, fs, lines_[i], body.w - gutter) : std::vector<std::string>{lines_[i]};
            if (y + lh * wl.size() >= clip.y && y <= clip.b()) {
                if (!binary_) font::drawIn(*ui.c, Face::Mono, ui.dp(11), Rect(body.x, y, gutter - ui.dp(8), lh), std::to_string(i + 1),
                                           th.textFaint, AlignRight);
                float yy = y;
                for (auto& l : wl) {
                    font::draw(*ui.c, Face::Mono, fs, body.x + gutter, yy, l, th.text);
                    yy += lh;
                }
            }
            y += lh * wl.size();
        }
        ui.endScroll(y + ui.dp(8));
    }

private:
    std::string path_;
    std::vector<std::string> lines_;
    bool binary_ = false, wrap_ = true;
};

class TextInputScreen : public Screen {
public:
    TextInputScreen(std::string title, std::string hint, std::string initial, TextFn fn, bool password)
        : title_(std::move(title)), hint_(std::move(hint)), text_(std::move(initial)), fn_(std::move(fn)), password_(password) {}
    std::string title() const override { return title_; }

    void draw(Ui& ui, App& app, Rect area) override {
        float pad = ui.dp(16);
        Rect kb = area.cutBottom(ui.keyboardHeight());
        Rect f(area.x + pad, area.y + ui.dp(8), area.w - pad * 2, ui.dp(58));
        std::string shown = password_ ? std::string(text_.size(), '*') : text_;
        ui.textField(ui.id("ti_field"), f, shown, hint_, true);
        float bw = (area.w - pad * 2 - ui.dp(10)) / 2;
        bool done = false;
        if (ui.button(ui.id("ti_cancel"), Rect(area.x + pad, f.b() + ui.dp(16), bw, ui.dp(50)), "Cancel", Icon::Close, BtnStyle::Tonal))
            app.pop();
        if (ui.button(ui.id("ti_ok"), Rect(area.x + pad + bw + ui.dp(10), f.b() + ui.dp(16), bw, ui.dp(50)), "Done", Icon::Check))
            done = true;
        if (ui.keyboard(ui.id("ti_kb"), kb, text_)) done = true;
        if (done && !finished_) {
            finished_ = true;
            app.pop();
            if (fn_) fn_(app, text_);
        }
    }

private:
    std::string title_, hint_, text_;
    TextFn fn_;
    bool password_ = false, finished_ = false;
};

}  // namespace

std::unique_ptr<Screen> makeFilesScreen(const std::string& start, std::vector<std::string> filter, PickFn onPick,
                                        const std::string& title, AppFn onCantFind) {
    return std::make_unique<FilesScreen>(start, std::move(filter), std::move(onPick), title, std::move(onCantFind));
}
std::unique_ptr<Screen> makeTextViewerScreen(const std::string& path) { return std::make_unique<TextViewerScreen>(path); }
std::unique_ptr<Screen> makeTextInputScreen(const std::string& title, const std::string& hint, const std::string& initial,
                                            TextFn onDone, bool password) {
    return std::make_unique<TextInputScreen>(title, hint, initial, std::move(onDone), password);
}

}  // namespace tw
