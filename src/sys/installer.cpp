#include "sys/installer.h"
#include "sys/history.h"
#include "core/log.h"
#include "core/sha256.h"
#include "core/task.h"
#include "core/util.h"
#include "platform/device.h"
#include "sys/imagesrc.h"
#include "sys/partitions.h"
#include "sys/payload.h"
#include "miniz.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace tw::installer {

static const char* kUpdateBinary = "META-INF/com/google/android/update-binary";
static const char* kUpdaterScript = "META-INF/com/google/android/updater-script";

ZipInfo inspectZip(const std::string& path) {
    ZipInfo zi;
    mz_zip_archive z{};
    if (!mz_zip_reader_init_file(&z, path.c_str(), 0)) {
        zi.error = mz_zip_get_error_string(mz_zip_get_last_error(&z));
        return zi;
    }
    zi.ok = true;
    zi.entries = (int)mz_zip_reader_get_num_files(&z);
    for (int i = 0; i < zi.entries; i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&z, i, &st)) continue;
        std::string n = st.m_filename;
        zi.uncompressed += st.m_uncomp_size;
        if (n == kUpdateBinary) zi.hasUpdateBinary = true;
        if (n == kUpdaterScript) zi.hasUpdaterScript = true;
        if (n == "module.prop") zi.isMagiskModule = true;
        if (n == "payload.bin") zi.hasPayload = true;
        if (zi.names.size() < 200) zi.names.push_back(n);
    }
    mz_zip_reader_end(&z);
    if (zi.isMagiskModule) zi.kind = "Magisk / KernelSU module";
    else if (zi.hasPayload) zi.kind = "A/B OTA package (payload.bin)";
    else if (zi.hasUpdateBinary && zi.hasUpdaterScript) zi.kind = "Edify flashable zip";
    else if (zi.hasUpdateBinary) zi.kind = "Script flashable zip";
    else zi.kind = "Not a flashable zip";
    return zi;
}

bool testZip(const std::string& path, Task& t) {
    t.setStatus("Verifying zip integrity");
    mz_zip_archive z{};
    if (!mz_zip_reader_init_file(&z, path.c_str(), 0)) {
        t.print("Zip open failed: %s", mz_zip_get_error_string(mz_zip_get_last_error(&z)));
        return false;
    }
    int n = (int)mz_zip_reader_get_num_files(&z);
    bool ok = true;
    for (int i = 0; i < n && ok; i++) {
        if (!mz_zip_validate_file(&z, i, 0)) {
            mz_zip_archive_file_stat st;
            mz_zip_reader_file_stat(&z, i, &st);
            t.print("  CRC/structure error in %s", st.m_filename);
            ok = false;
        }
        t.setSubProgress(float(i + 1) / n);
    }
    mz_zip_reader_end(&z);
    t.print(ok ? "Zip verified OK (%d entries)" : "Zip is CORRUPT", n);
    return ok;
}

static bool extractEntry(const std::string& zip, const char* entry, const std::string& out) {
    mz_zip_archive z{};
    if (!mz_zip_reader_init_file(&z, zip.c_str(), 0)) return false;
    bool ok = mz_zip_reader_extract_file_to_file(&z, entry, out.c_str(), 0);
    mz_zip_reader_end(&z);
    return ok;
}

// Parse one line of the recovery <-> updater protocol. Progress values from the
// updater are fractions of the whole install; they are mapped into the slice of
// the task's progress bar reserved for this zip.
struct ProgressMap {
    float rbase = 0, rlen = 1;   // our slice of the global bar
    float base = 0, seg = 0;     // updater-relative
    int64_t segStart = 0;
    float segSecs = 0;
};

static void handleCommand(const std::string& line, Task& t, ProgressMap& pm) {
    auto sp = line.find(' ');
    std::string cmd = line.substr(0, sp);
    std::string arg = sp == std::string::npos ? "" : line.substr(sp + 1);
    if (cmd == "ui_print") {
        t.print("%s", arg.c_str());
    } else if (cmd == "progress") {
        auto f = util::splitWs(arg);
        pm.base += pm.seg;
        try {
            pm.seg = f.size() > 0 ? std::stof(f[0]) : 0;
            pm.segSecs = f.size() > 1 ? std::stof(f[1]) : 0;
        } catch (...) { pm.seg = 0; pm.segSecs = 0; }
        pm.segStart = util::nowMs();
        t.setRange(pm.rbase + pm.rlen * pm.base, pm.rlen * pm.seg);
        t.setSubProgress(0);
    } else if (cmd == "set_progress") {
        pm.segSecs = 0;
        try { t.setSubProgress(std::stof(arg)); } catch (...) {}
    } else if (cmd == "wipe_cache") {
        t.print("Updater requested cache wipe");
        if (auto* c = Partitions::get().find("/cache")) Partitions::get().wipe(*c, t);
    } else if (cmd == "log") {
        LOGI("updater: %s", arg.c_str());
    } else if (cmd == "clear_display" || cmd == "enable_reboot") {
        // nothing to do
    } else if (!cmd.empty()) {
        LOGD("updater: unknown command '%s'", line.c_str());
    }
}

static bool simulateInstall(const std::string& path, const ZipInfo& zi, Task& t) {
    t.print("[simulator] dry-run: update-binary is NOT executed on the host");
    std::string script;
    mz_zip_archive z{};
    if (zi.hasUpdaterScript && mz_zip_reader_init_file(&z, path.c_str(), 0)) {
        size_t len = 0;
        void* p = mz_zip_reader_extract_file_to_heap(&z, kUpdaterScript, &len, 0);
        if (p) { script.assign((char*)p, len); mz_free(p); }
        mz_zip_reader_end(&z);
    }
    auto lines = util::split(script, '\n');
    int steps = std::max<int>(12, (int)lines.size());
    for (int i = 0; i < steps && !t.cancelled(); i++) {
        if (i < (int)lines.size()) {
            std::string l = util::trim(lines[i]);
            if (util::startsWith(l, "ui_print(")) t.print("%s", l.substr(10, l.size() > 13 ? l.size() - 13 : 0).c_str());
        } else if (i % 3 == 0) {
            t.print("Installing step %d...", i);
        }
        t.setSubProgress(float(i + 1) / steps);
        usleep(120 * 1000);
    }
    return true;
}

static bool installZipImpl(const std::string& path, const InstallOptions& o, Task& t) {
    t.setStatus("Inspecting " + util::basename(path));
    t.print("Installing zip: %s", path.c_str());
    ZipInfo zi = inspectZip(path);
    if (!zi.ok) {
        t.print("Invalid zip file: %s", zi.error.c_str());
        return false;
    }
    t.print("Type: %s, %d entries, %s", zi.kind.c_str(), zi.entries, util::humanSize(zi.uncompressed).c_str());
    if (zi.hasPayload && !zi.hasUpdateBinary) {
        // A/B ROM or OTA: write payload.bin to the inactive slot (each block is checked against its SHA-256).
        payload::Payload p;
        std::string err;
        if (!payload::open(path, p, err)) { t.print("payload.bin: %s", err.c_str()); return false; }
        return payload::install(p, payload::InstallOptions(), t);
    }
    const float rbase = t.rangeStart(), rlen = t.rangeLen();
    float myBase = rbase;
    if (o.verifyZip) {
        t.setRange(rbase, rlen * 0.1f);
        if (!testZip(path, t)) return false;
        myBase += rlen * 0.1f;
    }
    if (!zi.hasUpdateBinary) {
        t.print("No update-binary found - this zip can't be flashed in recovery.");
        if (zi.isMagiskModule) t.print("Hint: install Magisk modules from the Magisk app.");
        return false;
    }
    t.setStatus("Flashing " + util::basename(path));
    float remaining = rbase + rlen - myBase;
    if (device::isSim()) {
        t.setRange(myBase, remaining);
        return simulateInstall(path, zi, t);
    }

    util::mkdirs("/tmp");
    std::string bin = "/tmp/update-binary";
    unlink(bin.c_str());
    if (!extractEntry(path, kUpdateBinary, bin)) {
        t.print("Failed to extract update-binary");
        return false;
    }
    chmod(bin.c_str(), 0755);
    // Many installer scripts still expect /sbin/sh and a writable /tmp.
    if (!util::exists("/sbin/sh") && util::exists("/system/bin/sh")) {
        util::mkdirs("/sbin");
        if (symlink("/system/bin/sh", "/sbin/sh") != 0) LOGW("could not create /sbin/sh");
    }

    int cmdPipe[2], outPipe[2];
    if (pipe(cmdPipe) != 0 || pipe2(outPipe, O_CLOEXEC) != 0) return false;
    fcntl(cmdPipe[0], F_SETFD, FD_CLOEXEC);
    pid_t pid = fork();
    if (pid == 0) {
        close(cmdPipe[0]);
        dup2(outPipe[1], 1);
        dup2(outPipe[1], 2);
        std::string fd = std::to_string(cmdPipe[1]);
        setenv("PATH", "/system/bin:/sbin:/bin:/system/xbin:/vendor/bin", 1);
        setenv("RECOVERY_NAME", "TWRP v2", 1);
        // Shell-script update-binaries need an interpreter when the shebang path is missing.
        std::string head;
        util::readFile(bin, head, 64);
        if (util::startsWith(head, "#!")) {
            std::string sh = util::shellPath();
            execl(sh.c_str(), "sh", bin.c_str(), "3", fd.c_str(), path.c_str(), (char*)nullptr);
        }
        execl(bin.c_str(), bin.c_str(), "3", fd.c_str(), path.c_str(), (char*)nullptr);
        _exit(127);
    }
    close(cmdPipe[1]);
    close(outPipe[1]);
    if (pid < 0) return false;

    ProgressMap pm;
    pm.rbase = myBase;
    pm.rlen = remaining;
    t.setRange(myBase, remaining);
    std::string cmdBuf, outBuf;
    bool cmdOpen = true, outOpen = true;
    while (cmdOpen || outOpen) {
        pollfd pf[2] = {{cmdOpen ? cmdPipe[0] : -1, POLLIN, 0}, {outOpen ? outPipe[0] : -1, POLLIN, 0}};
        int r = poll(pf, 2, 100);
        if (pm.segSecs > 0) t.setSubProgress(std::min(1.0f, (util::nowMs() - pm.segStart) / (pm.segSecs * 1000)));
        if (r <= 0) continue;
        char buf[4096];
        for (int i = 0; i < 2; i++) {
            if (!(pf[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            ssize_t n = read(pf[i].fd, buf, sizeof(buf));
            if (n <= 0) {
                (i == 0 ? cmdOpen : outOpen) = false;
                continue;
            }
            std::string& acc = i == 0 ? cmdBuf : outBuf;
            acc.append(buf, n);
            size_t nl;
            while ((nl = acc.find('\n')) != std::string::npos) {
                std::string line = acc.substr(0, nl);
                acc.erase(0, nl + 1);
                if (i == 0) handleCommand(line, t, pm);
                else LOGI("updater: %s", line.c_str());
            }
        }
    }
    close(cmdPipe[0]);
    close(outPipe[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (!ok) t.print("Updater process ended with ERROR: %d", WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    if (ok && o.wipeCacheAfter) {
        if (auto* c = Partitions::get().find("/cache")) Partitions::get().wipe(*c, t);
        Partitions::get().wipeDalvik(t);
    }
    unlink(bin.c_str());
    return ok;
}

bool installZip(const std::string& path, const InstallOptions& o, Task& t) {
    bool ota = payload::isOtaZip(path);  // A/B OTAs are recorded by payload::install
    bool ok = installZipImpl(path, o, t);
    if (!ota && !device::isSim()) history::record("Install zip", util::basename(path), ok);
    else if (!ota) history::record("Install zip (simulated)", util::basename(path), ok);
    return ok;
}

// ---- Android sparse image support -------------------------------------------------
static const uint32_t kSparseMagic = 0xED26FF3A;

bool isSparseImage(const std::string& path) {
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    uint32_t m = 0;
    bool r = read(fd, &m, 4) == 4 && m == kSparseMagic;
    close(fd);
    return r;
}

bool flashImage(const std::string& image, const std::string& blockDev, Task& t) {
    return img::flash(image, "", blockDev, {}, t);
}

}  // namespace tw::installer
