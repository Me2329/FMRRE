// Whose signature is on a zip, in words: the installed system's key, a trusted one, the public test key,
// another ROM's... with "Trust this key".
#include "ui/screens/screens.h"

#include "core/i18n.h"
#include "core/util.h"
#include "sys/installer.h"
#include "sys/zipsig.h"

namespace tw {

Color signatureTint(const Theme& th, const zipsig::Result& r) {
    switch (r.state) {
        case zipsig::State::Bad: return th.danger;
        case zipsig::State::Valid:
            if (r.trusted()) return th.ok;
            return r.testKey || r.otherThanSystem() ? th.warn : th.info;
        default: return th.textDim;
    }
}

Icon signatureIcon(const zipsig::Result& r) {
    if (r.state == zipsig::State::Bad) return Icon::Warning;
    return r.valid() ? (r.trusted() ? Icon::Check : Icon::Key) : Icon::Shield;
}

std::string signatureExplanation(const zipsig::Result& r, bool isRom) {
    using zipsig::State;
    using zipsig::Trust;
    switch (r.state) {
        case State::Unsigned:
            return "Most custom ROMs, GApps and Magisk zips aren't signed this way - that's normal. Official OTAs and many "
                   "ROM releases are.";
        case State::Bad:
            return "The zip was changed after it was signed, or it is damaged. Download it again; a modified ROM has to be "
                   "signed again by whoever changed it.";
        case State::Error: return "The file couldn't be read to the end.";
        case State::Valid: break;
    }
    if (r.trust == Trust::System)
        return "The system installed now takes updates signed with this key: this is an update of your ROM. Your data can stay.";
    if (r.trust == Trust::Recovery) return "This recovery trusts this key - the phone maker's or the ROM's.";
    if (r.trust == Trust::User) return "You added this key to FMRRE/keys yourself.";
    if (r.testKey) return "Anyone can sign with the AOSP test key: it shows an unofficial or test build, not who made it.";
    if (!r.systemKeysKnown)
        return "The installed system's keys couldn't be read, so FMRRE can't tell whether this is an update of your ROM.";
    if (isRom)
        return "The system installed now takes updates signed with another key: this is another ROM, or another build of it "
               "signed by someone else. Switching ROMs? Wipe data after flashing, or Android usually stops at the boot "
               "animation.";
    return "Not the installed system's key - normal for add-ons (GApps, kernels, Magisk), which their makers sign.";
}

namespace {

float textH(const std::string& s, float size, float w, Face f = Face::Regular) {
    return font::wrap(f, size, s, w).size() * font::lineHeight(f, size);
}

class SignatureScreen : public Screen {
public:
    explicit SignatureScreen(std::string zip) : zip_(std::move(zip)) { isRom_ = installer::inspectZip(zip_).isRom; }
    std::string title() const override { return "Signature"; }
    std::string subtitle() const override { return util::basename(zip_); }

    void draw(Ui& ui, App& app, Rect area) override {
        Theme& th = ui.th;
        Canvas& c = *ui.c;
        float pad = ui.dp(16);
        ui.beginScroll(ui.id("sig_scroll"), area);
        float x = area.x + pad, w = area.w - pad * 2, y = area.y;
        zipsig::Check ck = zipsig::check(zip_);
        const zipsig::Result& r = ck.result;
        Color tint = ck.done ? signatureTint(th, r) : th.accent;
        std::string head = ck.done ? r.headline() : "Checking the signature...";

        // ---- verdict
        float tw = w - ui.dp(88);
        float hh = textH(head, ui.dp(16), tw, Face::SemiBold);
        Rect hc(x, y, w, std::max(ui.dp(92), hh + ui.dp(64)));
        ui.card(hc);
        c.fillCircle(x + ui.dp(40), hc.y + ui.dp(46), ui.dp(26), withAlpha(tint, 0.15f));
        icons::drawCentered(c, ck.done ? signatureIcon(r) : Icon::Shield, x + ui.dp(40), hc.y + ui.dp(46), ui.dp(26), tint);
        float ty = hc.y + ui.dp(18);
        ty += ui.textBlock(x + ui.dp(78), ty, tw, head, ui.dp(16), th.text, Face::SemiBold);
        ui.label(Rect(x + ui.dp(78), ty + ui.dp(4), tw, ui.dp(20)), util::basename(zip_), ui.dp(12.5f), th.textDim);
        if (!ck.done) {
            ui.progressBar(Rect(x + ui.dp(78), hc.b() - ui.dp(18), tw, ui.dp(6)), ck.progress, th.accent);
            ui.keepAnimating();
        }
        y += hc.h + ui.dp(12);

        if (ck.done) {
            // ---- details
            std::vector<std::pair<std::string, std::string>> rows;
            if (r.valid()) {
                rows.push_back({"Signed by", r.signer()});
                if (!r.cert.o.empty() && r.cert.o != r.signer()) rows.push_back({"Organisation", r.cert.o});
                rows.push_back({"Key", r.cert.keyDesc()});
                rows.push_back({"Hash", r.hash});
                rows.push_back({"Key ID", r.cert.keyId().substr(0, 16)});
                if (r.trusted()) rows.push_back({"Trusted by", r.trustedBy});
            } else if (!r.reason.empty()) {
                rows.push_back({"Found", r.reason});
            }
            if (!rows.empty()) {
                float rh = ui.dp(28);
                Rect dc(x, y, w, rh * rows.size() + ui.dp(20));
                ui.card(dc);
                float ry = y + ui.dp(10);
                for (auto& row : rows) {
                    font::drawIn(c, Face::Regular, ui.dp(13), Rect(x + ui.dp(16), ry, w * 0.34f, rh), row.first, th.textDim);
                    i18n::NoTranslate raw;  // names and numbers
                    font::drawIn(c, row.first == "Key ID" ? Face::Mono : Face::SemiBold, ui.dp(13),
                                 Rect(x + w * 0.34f, ry, w * 0.66f - ui.dp(16), rh), row.second, th.text, AlignRight);
                    ry += rh;
                }
                y += dc.h + ui.dp(12);
            }
            // ---- what it means
            std::string ex = signatureExplanation(r, isRom_);
            bool warn = r.state == zipsig::State::Bad || (r.otherThanSystem() && isRom_);
            Color nc = warn ? (r.state == zipsig::State::Bad ? th.danger : th.warn) : th.info;
            float eh = textH(ex, ui.dp(13), w - ui.dp(56));
            Rect nr(x, y, w, eh + ui.dp(24));
            c.fillRoundRect(nr, ui.dp(14), withAlpha(nc, 0.10f));
            icons::drawCentered(c, warn ? Icon::Warning : Icon::Info, x + ui.dp(22), y + ui.dp(22), ui.dp(16), nc);
            ui.textBlock(x + ui.dp(40), y + ui.dp(12), w - ui.dp(56), ex, ui.dp(13), warn ? nc : th.text);
            y += nr.h + ui.dp(12);
            if (!r.systemKeysKnown && !r.systemNote.empty()) {
                ui.label(Rect(x + ui.dp(4), y, w - ui.dp(8), ui.dp(20)), r.systemNote, ui.dp(12), th.textFaint);
                y += ui.dp(28);
            }
            if (r.valid() && !r.trusted() && !r.testKey) {
                if (ui.button(ui.id("sig_trust"), Rect(x, y, w, ui.dp(50)), "Trust this key", Icon::Key, BtnStyle::Tonal)) {
                    std::string err, saved;
                    if (zipsig::trust(r, err, &saved)) app.toast("Key saved to FMRRE/keys", Icon::Key);
                    else app.toast(err, Icon::Warning, th.danger);
                }
                y += ui.dp(58);
                y += ui.textBlock(x + ui.dp(4), y, w - ui.dp(8), "Its zips then count as trusted, also for \"only trusted zips\"",
                                  ui.dp(12), th.textFaint) + ui.dp(12);
            }
        }
        ui.endScroll(y);
    }

private:
    std::string zip_;
    bool isRom_ = false;
};

}  // namespace

std::unique_ptr<Screen> makeSignatureScreen(const std::string& zip) { return std::make_unique<SignatureScreen>(zip); }

}  // namespace tw
