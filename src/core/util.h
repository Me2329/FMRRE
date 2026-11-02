#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace tw::util {

int64_t nowMs();
std::string timeString(bool h24, bool seconds = false);
std::string dateStamp();  // 2026-11-02--14-03-11

std::string trim(const std::string& s);
std::vector<std::string> split(const std::string& s, char sep, bool skipEmpty = true);
std::vector<std::string> splitWs(const std::string& s);
bool startsWith(const std::string& s, const std::string& p);
bool endsWith(const std::string& s, const std::string& p);
std::string toLower(std::string s);
std::string humanSize(uint64_t bytes);
std::string humanDuration(int64_t ms);
std::string basename(const std::string& p);
std::string dirname(const std::string& p);
std::string joinPath(const std::string& a, const std::string& b);
std::string shellQuote(const std::string& s);

bool exists(const std::string& p);
bool isDir(const std::string& p);
bool isFile(const std::string& p);
uint64_t fileSize(const std::string& p);
bool readFile(const std::string& p, std::string& out, size_t maxBytes = 16 << 20);
std::string readLine(const std::string& p);  // first line, trimmed; "" on error
bool writeFile(const std::string& p, const std::string& data);
bool mkdirs(const std::string& p, int mode = 0755);
bool removeTree(const std::string& p, const std::vector<std::string>& excludeTopLevel = {});
bool copyFile(const std::string& from, const std::string& to,
              const std::function<void(uint64_t, uint64_t)>& progress = nullptr);
uint64_t dirSize(const std::string& p);

struct DirEntry {
    std::string name;
    bool dir = false;
    bool link = false;
    uint64_t size = 0;
    int64_t mtime = 0;
    unsigned mode = 0;
};
std::vector<DirEntry> listDir(const std::string& p, bool showHidden = true);

// Run a shell command. Output (stdout+stderr) is delivered line by line.
// Returns the exit status (or -1 if the command could not be started).
int run(const std::string& cmd, const std::function<void(const std::string&)>& onLine = nullptr);
// Run and capture all output.
int runCapture(const std::string& cmd, std::string& out);
std::string shellPath();
bool haveBinary(const std::string& name);

}  // namespace tw::util
