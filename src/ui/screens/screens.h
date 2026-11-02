#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "ui/app.h"

namespace tw {

std::unique_ptr<Screen> makeHomeScreen();
std::unique_ptr<Screen> makeInstallScreen();
std::unique_ptr<Screen> makeFlashImageScreen(const std::string& image);
std::unique_ptr<Screen> makeWipeScreen();
std::unique_ptr<Screen> makeFormatDataScreen();
std::unique_ptr<Screen> makeBackupScreen();
std::unique_ptr<Screen> makeRestoreScreen();
std::unique_ptr<Screen> makeMountScreen();
std::unique_ptr<Screen> makeTerminalScreen(const std::string& initialCmd = "");
std::unique_ptr<Screen> makeAdvancedScreen();
std::unique_ptr<Screen> makeMonitorScreen();
std::unique_ptr<Screen> makePartInfoScreen();
std::unique_ptr<Screen> makeInputTestScreen();
std::unique_ptr<Screen> makeSettingsScreen();
std::unique_ptr<Screen> makeRebootScreen();
std::unique_ptr<Screen> makeOperationScreen(Icon icon);
std::unique_ptr<Screen> makeAboutScreen();

// File browser. With `filter` (e.g. {".zip"}) and `onPick` it works as a picker.
using PickFn = std::function<void(App&, const std::string&)>;
std::unique_ptr<Screen> makeFilesScreen(const std::string& start = "", std::vector<std::string> filter = {},
                                        PickFn onPick = nullptr, const std::string& title = "");
std::unique_ptr<Screen> makeTextViewerScreen(const std::string& path);

// Generic text entry with the on-screen keyboard.
using TextFn = std::function<void(App&, const std::string&)>;
std::unique_ptr<Screen> makeTextInputScreen(const std::string& title, const std::string& hint,
                                            const std::string& initial, TextFn onDone, bool password = false);

// Shared helpers for screens
Icon iconForFile(const std::string& name, bool dir);
Color colorForFile(const Theme& th, const std::string& name, bool dir);

}  // namespace tw
