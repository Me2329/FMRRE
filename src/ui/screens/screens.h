#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "sys/imagesrc.h"
#include "sys/ors.h"
#include "ui/app.h"

namespace tw {

std::unique_ptr<Screen> makeHomeScreen();
std::unique_ptr<Screen> makeInstallScreen();
// Adds a zip to the Install screen's queue (scripts and screenshots).
void queueZip(const std::string& path);
std::unique_ptr<Screen> makeFlashImageScreen(const std::string& image, const std::string& zipEntry = "");
// "Can't find your file?": type a path, see why it may be hidden, get a dd command.
std::unique_ptr<Screen> makeManualFlashScreen(const std::string& path = "");
std::unique_ptr<Screen> makeGsiScreen(const std::string& image = "");
// A/B ROM / OTA zip (payload.bin): install to the inactive slot, or extract images.
std::unique_ptr<Screen> makePayloadScreen(const std::string& path);
// Whose signature is on a zip - the installed system's key, a trusted one, the public test key, another ROM's -
// with "Trust this key". The helpers give the Install and OTA screens the same colours and words.
namespace zipsig { struct Result; }
std::unique_ptr<Screen> makeSignatureScreen(const std::string& zip);
Color signatureTint(const Theme& th, const zipsig::Result& r);
Icon signatureIcon(const zipsig::Result& r);
std::string signatureExplanation(const zipsig::Result& r, bool isRom);
std::unique_ptr<Screen> makeSuperScreen();
// "Why did my phone bootloop?": previous boot's kernel log explained, with fixes.
std::unique_ptr<Screen> makeRescueScreen();
// The flash history (FMRRE/history.txt), newest first.
std::unique_ptr<Screen> makeHistoryScreen();
// Auto repair: health checks + logs -> a plan of fixes, safest first. autostart: count down and run the
// recommended fixes unattended (after a failed boot). prompt: Android asked for a factory reset.
std::unique_ptr<Screen> makeAutoRepairScreen(bool autostart = false, bool prompt = false);
// fastbootd built into FMRRE (USB FunctionFS; TCP in the simulator).
std::unique_ptr<Screen> makeFastbootScreen(bool autostart = true);
std::unique_ptr<Screen> makeVbmetaScreen();
std::unique_ptr<Screen> makeFsToolsScreen(const std::string& mountPoint);
std::unique_ptr<Screen> makeRootScreen();
std::unique_ptr<Screen> makeInstallRecoveryScreen();
std::unique_ptr<Screen> makeWipeScreen();
std::unique_ptr<Screen> makeFormatDataScreen();
std::unique_ptr<Screen> makeBackupScreen();
std::unique_ptr<Screen> makeRestoreScreen();
std::unique_ptr<Screen> makeMountScreen();
// initialCmd runs immediately; prefill is put in the input line for the user to review and run.
std::unique_ptr<Screen> makeTerminalScreen(const std::string& initialCmd = "", const std::string& prefill = "");
std::unique_ptr<Screen> makeAdvancedScreen();
std::unique_ptr<Screen> makeMonitorScreen();
std::unique_ptr<Screen> makePartInfoScreen();
std::unique_ptr<Screen> makeInputTestScreen();
std::unique_ptr<Screen> makeSettingsScreen();
std::unique_ptr<Screen> makeDetailScreen(bool firstRun = false);
std::unique_ptr<Screen> makeRebootScreen();
std::unique_ptr<Screen> makeOperationScreen(Icon icon);
std::unique_ptr<Screen> makeAboutScreen();
std::unique_ptr<Screen> makeOrsScreen(const ors::Script& s);
// Picks up `fmrre` CLI requests queued in /tmp/fmrre/cli (install as App::idleHook).
void orsIdle(App& app);

// File browser. With `filter` (e.g. {".zip"}) and `onPick` it works as a picker.
using PickFn = std::function<void(App&, const std::string&)>;
// onCantFind adds a "Can't find your file?" entry (picker mode) that hands over to a manual path screen.
using AppFn = std::function<void(App&)>;
std::unique_ptr<Screen> makeFilesScreen(const std::string& start = "", std::vector<std::string> filter = {},
                                        PickFn onPick = nullptr, const std::string& title = "", AppFn onCantFind = nullptr);
std::unique_ptr<Screen> makeTextViewerScreen(const std::string& path);

// Generic text entry with the on-screen keyboard.
using TextFn = std::function<void(App&, const std::string&)>;
std::unique_ptr<Screen> makeTextInputScreen(const std::string& title, const std::string& hint,
                                            const std::string& initial, TextFn onDone, bool password = false);

// Partitions an image can be written to: fstab entries plus every mapped logical partition in super.
struct FlashTarget {
    std::string label, dev, lpName;  // lpName set for dynamic partitions
    uint64_t size = 0;
    bool logical = false;
};
std::vector<FlashTarget> flashTargets();
int guessTarget(const std::vector<FlashTarget>& targets, const img::Info& info);  // -1 if unsure

// Shared helpers for screens
Icon iconForFile(const std::string& name, bool dir);
Color colorForFile(const Theme& th, const std::string& name, bool dir);

}  // namespace tw
