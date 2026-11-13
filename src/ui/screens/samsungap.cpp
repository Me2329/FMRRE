// Flash stock partition images straight out of a Samsung firmware AP_*.tar / .tar.md5, on the phone - no PC or Odin.
// The checksum is verified, then each image you pick is pulled out and sent through FMRRE's normal, verified flash
// (LZ4 decode + sparse expand + SHA-256 read-back). The usual reason: put the stock boot / recovery / vbmeta / dtbo
// back on a Samsung (an S8) to undo a bad flash or a bootloop, without a computer.
#include "ui/screens/screens.h"

#include <unistd.h>

#include "core/i18n.h"
#include "core/task.h"
#include "core/util.h"
#include "platform/device.h"
#include "sys/imagesrc.h"
#include "sys/partitions.h"
#include "sys/samsung.h"

namespace tw {

namespace {

// Partitions it is risky to overwrite from a firmware image (they hold data or need the matching rest of the ROM).
bool risky(const std::string& part) {
    static const char* k[] = {"system", "super", "userdata", "data", "product", "vendor", "odm", "cache", "omr", "prism", "optics"};
    for (const char* s : k)
        if (part == s) return true;
    return false;
}

class SamsungApScreen : public Screen {
public:
    explicit SamsungApScreen(std::string ap) : ap_(std::move(ap)) {}
    std::string title() const override { return "Samsung firmware"; }
    std::string subtitle() const override { return util::basename(ap_); }
    void onShow(App&) override {
        Partitions::get().refresh();
        err_.clear();
        if (!samsung::read(ap_, pkg_, err_)) pkg_ = samsung::Package{};
    }

    void draw(Ui& ui, App& app, Rect area) override {
        Theme& th = ui.th;
        float pad = ui.dp(16);
        ui.beginScroll(ui.id("ap_scroll"), area);
        float x = area.x + pad, w = area.w - pad * 2, y = area.y;

        if (!err_.empty()) {
            Rect e(x, y, w, ui.dp(80));
            ui.card(e, ui.dp(16));
            icons::drawCentered(*ui.c, Icon::Warning, e.x + ui.dp(30), e.cy(), ui.dp(22), th.danger);
            ui.label(Rect(e.x + ui.dp(56), e.y + ui.dp(28), e.w - ui.dp(72), ui.dp(24)), err_, ui.dp(13), th.text);
            y += e.h + ui.dp(12);
            ui.endScroll(y);
            return;
        }

        // Checksum banner.
        const char* mtext = "No checksum in this package";
        Color mtint = th.textDim;
        Icon micon = Icon::Info;
        if (pkg_.md5 == samsung::Md5State::Ok) { mtext = "MD5 checksum OK"; mtint = th.ok; micon = Icon::Check; }
        else if (pkg_.md5 == samsung::Md5State::Mismatch) { mtext = "MD5 MISMATCH - the file is damaged"; mtint = th.danger; micon = Icon::Warning; }
        else if (pkg_.md5 == samsung::Md5State::Bad) { mtext = "Couldn't read the checksum"; mtint = th.warn; micon = Icon::Warning; }
        Rect mb(x, y, w, ui.dp(56));
        ui.card(mb, ui.dp(16));
        icons::drawCentered(*ui.c, micon, mb.x + ui.dp(28), mb.cy(), ui.dp(20), mtint);
        ui.label(Rect(mb.x + ui.dp(52), mb.y + ui.dp(19), mb.w - ui.dp(68), ui.dp(22)), mtext, ui.dp(13.5f), mtint, Face::SemiBold);
        y += mb.h + ui.dp(14);

        ui.sectionHeader(Rect(x, y, w, ui.dp(22)), "Images - tap one to flash");
        y += ui.dp(28);
        bool damaged = pkg_.md5 == samsung::Md5State::Mismatch;
        for (size_t i = 0; i < pkg_.images.size(); i++) {
            const samsung::Image& im = pkg_.images[i];
            Partition* p = Partitions::get().find(im.partition);
            bool here = p != nullptr;
            bool warn = risky(im.partition);
            Color tint = !here ? th.textFaint : warn ? th.warn : th.accent;
            std::string sub;
            {
                i18n::NoTranslate raw;
                sub = im.name + "  \xE2\x80\xA2  " + util::humanSize(im.size) + (im.lz4 ? "  \xE2\x80\xA2  LZ4" : "");
            }
            if (!here) sub += "  (no such partition here)";
            Rect r(x, y, w, ui.dp(66));
            int f;
            {
                i18n::NoTranslate raw;  // the partition name is a device string
                f = ui.listItem(ui.id("ap", int(i)), r, warn ? Icon::Warning : Icon::Chip, im.partition, sub, "", tint, here);
            }
            if ((f & Clicked) && here) startFlash(app, im, *p, warn, damaged);
            y += r.h + ui.dp(8);
        }
        y += ui.dp(8);
        ui.endScroll(y);
    }

private:
    void startFlash(App& app, const samsung::Image& im, Partition& p, bool warn, bool damaged) {
        std::string ap = ap_, part = im.partition, name = im.name, dev = p.blockDev;
        samsung::Image img = im;
        std::string msg = "Write the stock " + part + " from this firmware onto " + util::basename(dev) + ". It is checked (SHA-256) after writing.";
        if (damaged) msg = "The package's checksum did NOT match - it may be corrupt. " + msg;
        if (warn) msg = "This replaces " + part + ", which holds data or needs the matching ROM - only do this if you know why. " + msg;
        app.confirm("Flash " + part + "?", msg, "Swipe to flash", [this, ap, img, part, dev, &app]() {
            app.runTask("Flash " + part, Icon::Download, [ap, img, part, dev](Task& t) {
                std::string dir = Partitions::get().appDir() + "/ap";
                util::mkdirs(dir);
                std::string tmp = dir + "/" + img.name;
                if (!samsung::extract(ap, img, tmp, t)) return false;
                img::FlashOptions fo;
                fo.verify = true;
                fo.precheck = true;
                fo.limit = Partitions::blockSize(dev);
                bool ok = img::flash(tmp, "", dev, fo, t);
                unlink(tmp.c_str());
                t.print(ok ? "Flashed %s" : "Flash of %s FAILED", part.c_str());
                return ok;
            });
        }, warn || damaged, warn ? Icon::Warning : Icon::Download);
    }

    std::string ap_, err_;
    samsung::Package pkg_;
};

}  // namespace

std::unique_ptr<Screen> makeSamsungApScreen(const std::string& ap) { return std::make_unique<SamsungApScreen>(ap); }

}  // namespace tw
