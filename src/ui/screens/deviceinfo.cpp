// Device info: everything FMRRE detected about this phone, on one read-only screen - the first thing to check when
// bringing it up on a new device (is the model right? A/B or A-only? which slot? is /data encrypted? is this the
// right build for the CPU?). Nothing here changes anything.
#include "ui/screens/screens.h"

#include <algorithm>

#include "core/i18n.h"
#include "core/util.h"
#include "platform/device.h"
#include "sys/partitions.h"

namespace tw {

namespace {

class DeviceInfoScreen : public Screen {
public:
    std::string title() const override { return "Device info"; }
    std::string subtitle() const override { return "What FMRRE detected about this phone"; }
    void onShow(App&) override { Partitions::get().refresh(); }

    void draw(Ui& ui, App& app, Rect area) override {
        Theme& th = ui.th;
        Canvas& c = *ui.c;
        const auto& in = device::info();
        float pad = ui.dp(16);
        ui.beginScroll(ui.id("di_scroll"), area);
        float x = area.x + pad, w = area.w - pad * 2, y = area.y;

        // One section = a card of label/value rows; the value is monospaced for the technical ones.
        auto section = [&](const char* name, std::vector<std::pair<std::string, std::string>> rows, Face valFace = Face::Regular) {
            // drop empty values
            rows.erase(std::remove_if(rows.begin(), rows.end(), [](auto& r) { return r.second.empty(); }), rows.end());
            if (rows.empty()) return;
            ui.sectionHeader(Rect(x, y, w, ui.dp(22)), name);
            y += ui.dp(28);
            float rh = ui.dp(34);
            Rect card(x, y, w, rh * rows.size() + ui.dp(10));
            ui.card(card, ui.dp(16));
            float ry = card.y + ui.dp(6);
            for (auto& r : rows) {
                font::drawIn(c, Face::Regular, ui.dp(12.5f), Rect(card.x + ui.dp(16), ry + ui.dp(8), ui.dp(140), ui.dp(20)),
                             r.first, th.textDim);  // label: translated by the font layer
                {
                    i18n::NoTranslate raw;  // value: a device string, shown as-is
                    font::drawIn(c, valFace, ui.dp(12.5f), Rect(card.x + ui.dp(150), ry + ui.dp(8), card.w - ui.dp(166), ui.dp(20)),
                                 r.second, th.text, AlignRight);
                }
                ry += rh;
            }
            y += card.h + ui.dp(14);
        };

        std::string ab = i18n::tr(in.ab ? "A/B (seamless)" : "A-only (dedicated recovery)");
        if (in.ab && !in.slot.empty()) ab += "  \xE2\x80\xA2  slot " + std::string(1, (char)toupper(in.slot.back()));
        std::string enc = i18n::tr(Partitions::get().dataEncrypted() ? "encrypted" : "not encrypted");

        section("Device", {
            {"Model", in.model},
            {"Manufacturer", in.manufacturer.empty() ? in.brand : in.manufacturer},
            {"Codename", in.device},
            {"Serial", in.serial},
        });
        section("System", {
            {"Android", in.android + (in.sdk.empty() ? "" : "  (API " + in.sdk + ")")},
            {"Security patch", in.patch},
            {"Partitions", ab},
            {"Data", enc},
        });
        // CPU / build.
        std::string build = std::string(device::buildAbi()) + " build";
        std::string run = device::machine();
        std::string advice = device::abiAdvice();
        auto cpu = device::cpu();
        auto mem = device::memory();
        section("Hardware", {
            {"Chipset", in.platform.empty() ? device::prop("ro.hardware") : in.platform},
            {"CPU", (cpu.cores ? std::to_string(cpu.cores) + " cores" : "") + (run.empty() ? "" : "  \xE2\x80\xA2  " + run)},
            {"Memory", mem.total ? util::humanSize(mem.total) : ""},
            {"Screen", std::to_string(c.width()) + " x " + std::to_string(c.height())},
            {"FMRRE build", build + (advice.empty() ? "" : "  \xE2\x80\xA2  " + advice)},
        });
        // Live-ish.
        auto bat = device::battery();
        std::string batLine;
        if (bat.percent >= 0) {
            batLine = std::to_string(bat.percent) + "%";
            if (!bat.status.empty()) batLine += "  \xE2\x80\xA2  " + bat.status;
            if (bat.tempC > -500) { char t[16]; snprintf(t, sizeof t, "  %.0f\xC2\xB0""C", bat.tempC); batLine += t; }
        }
        section("Status", {
            {"Battery", batLine},
            {"Kernel", in.kernel},
            {"Fingerprint", in.fingerprint},
        }, Face::Mono);

        // A shortcut to the full partition table.
        Rect pi(x, y, w, ui.dp(58));
        if (ui.listItem(ui.id("di_parts"), pi, Icon::Storage, "All partitions",
                        std::to_string(Partitions::get().all().size()) + " detected", "", th.accent) & Clicked)
            app.push(makePartInfoScreen());
        y += pi.h + ui.dp(18);
        ui.endScroll(y);
    }
};

}  // namespace

std::unique_ptr<Screen> makeDeviceInfoScreen() { return std::make_unique<DeviceInfoScreen>(); }

}  // namespace tw
