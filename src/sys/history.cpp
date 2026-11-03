#include "sys/history.h"

#include <ctime>
#include <mutex>

#include "core/util.h"
#include "platform/device.h"
#include "sys/partitions.h"

namespace tw::history {

namespace {
std::mutex g_mu;

std::string clean(std::string s) {
    for (auto& c : s)
        if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    return s;
}
}  // namespace

std::string path() {
    std::string dir = Partitions::get().storagePath() + "/TWRP";
    if (util::mkdirs(dir)) return dir + "/history.txt";
    return device::path("/tmp/twrpv2-history.txt");
}

void record(const std::string& action, const std::string& detail, bool ok) {
    std::lock_guard<std::mutex> l(g_mu);
    std::string f = path(), old;
    util::readFile(f, old, 1 << 20);
    if (old.size() > (512u << 10)) old = old.substr(old.find('\n', old.size() - (256u << 10)) + 1);  // keep it small
    char line[64];
    time_t now = time(nullptr);
    snprintf(line, sizeof(line), "%lld\t", (long long)now);
    old += std::string(line) + (ok ? "OK" : "FAILED") + "\t" + clean(action) + "\t" + clean(detail) + "\n";
    util::writeFile(f, old);
}

std::vector<Entry> recent(size_t n) {
    std::lock_guard<std::mutex> l(g_mu);
    std::string data;
    std::vector<Entry> out;
    if (!util::readFile(path(), data, 1 << 20)) return out;
    auto lines = util::split(data, '\n');
    for (size_t i = lines.size(); i-- > 0 && out.size() < n;) {
        auto f = util::split(lines[i], '\t', false);
        if (f.size() < 4) continue;
        Entry e;
        e.time = atoll(f[0].c_str());
        e.ok = f[1] == "OK";
        e.action = f[2];
        e.detail = f[3];
        out.push_back(e);
    }
    return out;
}

std::string ago(int64_t t) {
    int64_t d = (int64_t)time(nullptr) - t;
    if (d < 0) return "just now";
    if (d < 90) return "just now";
    if (d < 3600) return std::to_string(d / 60) + " min ago";
    if (d < 86400) return std::to_string(d / 3600) + " h ago";
    if (d < 2 * 86400) return "yesterday";
    return std::to_string(d / 86400) + " days ago";
}

}  // namespace tw::history
