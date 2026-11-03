#include "sys/fastboot.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cinttypes>
#include <cstring>
#include <endian.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <linux/usb/functionfs.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

#include "core/log.h"
#include "core/task.h"
#include "core/util.h"
#include "platform/device.h"
#include "sys/ors.h"
#include "sys/partitions.h"
#include "sys/super.h"

namespace tw::fastboot {

State& state() {
    static State s;
    return s;
}

namespace {

constexpr size_t kMaxCommand = 4096, kMaxResponse = 256;

bool preadAll(int fd, void* buf, size_t n, uint64_t off) {
    char* p = (char*)buf;
    while (n) {
        ssize_t r = pread(fd, p, n, (off_t)off);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return false;
        p += r; n -= (size_t)r; off += (uint64_t)r;
    }
    return true;
}

bool pwriteAll(int fd, const void* buf, size_t n, uint64_t off) {
    const char* p = (const char*)buf;
    while (n) {
        ssize_t r = pwrite(fd, p, n, (off_t)off);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return false;
        p += r; n -= (size_t)r; off += (uint64_t)r;
    }
    return true;
}

void setState(const std::function<void(State&)>& f) {
    State& s = state();
    std::lock_guard<std::mutex> l(s.mu);
    f(s);
}

// ---- transports -------------------------------------------------------------------

class Transport {
public:
    virtual ~Transport() = default;
    // Next piece of what the host sent: a whole command, or part of download data. <= 0: gone.
    virtual ssize_t read(void* buf, size_t n) = 0;
    virtual bool write(const void* buf, size_t n) = 0;
    virtual std::string peer() const = 0;
    bool readFull(void* buf, size_t n) {
        char* p = (char*)buf;
        while (n) {
            ssize_t r = read(p, n);
            if (r <= 0) return false;
            p += r;
            n -= (size_t)r;
        }
        return true;
    }
};

// fastboot over TCP (fastboot -s tcp:HOST[:PORT]): "FB01" handshake both ways, then every
// message is an 8-byte big-endian length followed by the data.
class TcpTransport : public Transport {
public:
    TcpTransport(int fd, std::string peer, std::function<bool()> cancelled) : fd_(fd), peer_(std::move(peer)), cancelled_(std::move(cancelled)) {}
    ~TcpTransport() override { close(fd_); }
    std::string peer() const override { return peer_; }

    ssize_t read(void* buf, size_t n) override {
        while (left_ == 0) {
            uint8_t h[8];
            if (!recvAll(h, 8)) return -1;
            for (int i = 0; i < 8; i++) left_ = (left_ << 8) | h[i];
        }
        ssize_t r = recvSome(buf, (size_t)std::min<uint64_t>(n, left_));
        if (r <= 0) return -1;
        left_ -= (uint64_t)r;
        return r;
    }
    bool write(const void* buf, size_t n) override {
        uint8_t h[8];
        for (int i = 0; i < 8; i++) h[i] = uint8_t(uint64_t(n) >> (56 - 8 * i));
        return sendAll(h, 8) && sendAll(buf, n);
    }
    bool handshake() {
        char h[4];
        if (!recvAll(h, 4) || h[0] != 'F' || h[1] != 'B') return false;
        return sendAll("FB01", 4);
    }

private:
    ssize_t recvSome(void* buf, size_t n) {
        for (;;) {
            pollfd p{fd_, POLLIN, 0};
            int pr = poll(&p, 1, 250);
            if (cancelled_ && cancelled_()) return -1;
            if (pr < 0 && errno == EINTR) continue;
            if (pr <= 0) continue;
            ssize_t r = recv(fd_, buf, n, 0);
            if (r < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            return r;
        }
    }
    bool recvAll(void* buf, size_t n) {
        char* p = (char*)buf;
        while (n) {
            ssize_t r = recvSome(p, n);
            if (r <= 0) return false;
            p += r;
            n -= (size_t)r;
        }
        return true;
    }
    bool sendAll(const void* buf, size_t n) {
        const char* p = (const char*)buf;
        while (n) {
            ssize_t r = send(fd_, p, n, MSG_NOSIGNAL);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) return false;
            p += r;
            n -= (size_t)r;
        }
        return true;
    }
    int fd_;
    uint64_t left_ = 0;
    std::string peer_;
    std::function<bool()> cancelled_;
};

int tcpListen(const std::string& bindAddr, int port, std::string& err) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) { err = strerror(errno); return -1; }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, bindAddr.c_str(), &a.sin_addr) != 1) a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (sockaddr*)&a, sizeof(a)) != 0 || listen(fd, 1) != 0) {
        err = std::string("port ") + std::to_string(port) + ": " + strerror(errno);
        close(fd);
        return -1;
    }
    return fd;
}

std::unique_ptr<Transport> tcpAccept(int lfd, const std::function<bool()>& cancelled) {
    while (!cancelled()) {
        pollfd p{lfd, POLLIN, 0};
        if (poll(&p, 1, 250) <= 0) continue;
        sockaddr_in a{};
        socklen_t al = sizeof(a);
        int fd = accept(lfd, (sockaddr*)&a, &al);
        if (fd < 0) continue;
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        char ip[INET_ADDRSTRLEN] = "?";
        inet_ntop(AF_INET, &a.sin_addr, ip, sizeof(ip));
        auto t = std::make_unique<TcpTransport>(fd, std::string(ip) + ":" + std::to_string(ntohs(a.sin_port)), cancelled);
        if (t->handshake()) return t;
        LOGW("fastboot: %s sent no FB01 handshake", ip);
    }
    return nullptr;
}

// ---- USB (FunctionFS, like AOSP's fastbootd) -------------------------------------------

constexpr const char* kFfs = "/dev/usb-ffs/fastboot";
constexpr const char* kGadget = "/config/usb_gadget/g1";

struct FuncDesc {
    usb_interface_descriptor intf;
    usb_endpoint_descriptor_no_audio source, sink;
} __attribute__((packed));
struct SsFuncDesc {
    usb_interface_descriptor intf;
    usb_endpoint_descriptor_no_audio source;
    usb_ss_ep_comp_descriptor sourceComp;
    usb_endpoint_descriptor_no_audio sink;
    usb_ss_ep_comp_descriptor sinkComp;
} __attribute__((packed));
struct OsDesc {  // Microsoft OS descriptors: Windows loads WinUSB without a driver
    usb_os_desc_header header;
    usb_ext_compat_desc compat;
} __attribute__((packed));
struct DescV2 {
    usb_functionfs_descs_head_v2 head;
    uint32_t fsCount, hsCount, ssCount;
    FuncDesc fs, hs;
    SsFuncDesc ss;
} __attribute__((packed));
struct DescV2Os {
    usb_functionfs_descs_head_v2 head;
    uint32_t fsCount, hsCount, ssCount, osCount;
    FuncDesc fs, hs;
    SsFuncDesc ss;
    OsDesc os;
} __attribute__((packed));
struct Strings {
    usb_functionfs_strings_head head;
    uint16_t lang;
    char str[sizeof("fastboot")];
} __attribute__((packed));

usb_interface_descriptor intfDesc() {
    usb_interface_descriptor d{};
    d.bLength = USB_DT_INTERFACE_SIZE;
    d.bDescriptorType = USB_DT_INTERFACE;
    d.bNumEndpoints = 2;
    d.bInterfaceClass = 0xff;     // vendor specific
    d.bInterfaceSubClass = 0x42;  // fastboot
    d.bInterfaceProtocol = 0x03;
    d.iInterface = 1;
    return d;
}
usb_endpoint_descriptor_no_audio epDesc(uint8_t addr, uint16_t maxPacket) {
    usb_endpoint_descriptor_no_audio e{};
    e.bLength = USB_DT_ENDPOINT_SIZE;
    e.bDescriptorType = USB_DT_ENDPOINT;
    e.bEndpointAddress = addr;
    e.bmAttributes = USB_ENDPOINT_XFER_BULK;
    e.wMaxPacketSize = htole16(maxPacket);
    return e;
}
usb_ss_ep_comp_descriptor ssComp() {
    usb_ss_ep_comp_descriptor c{};
    c.bLength = USB_DT_SS_EP_COMP_SIZE;
    c.bDescriptorType = USB_DT_SS_ENDPOINT_COMP;
    c.bMaxBurst = 15;
    return c;
}
FuncDesc funcDesc(uint16_t mp) { return {intfDesc(), epDesc(1 | USB_DIR_OUT, mp), epDesc(2 | USB_DIR_IN, mp)}; }
SsFuncDesc ssDesc() { return {intfDesc(), epDesc(1 | USB_DIR_OUT, 1024), ssComp(), epDesc(2 | USB_DIR_IN, 1024), ssComp()}; }

std::string usbController() {
    std::string c = device::prop("sys.usb.controller");
    if (!c.empty()) return c;
    for (auto& e : util::listDir("/sys/class/udc")) return e.name;
    return "";
}

bool waitProp(const std::string& key, const std::string& want, int ms) {
    for (int i = 0; i < ms / 50; i++) {
        if (device::prop(key) == want) return true;
        usleep(50 * 1000);
    }
    return false;
}

class UsbTransport : public Transport {
public:
    ~UsbTransport() override { shutdown(); }
    std::string peer() const override { return "USB"; }

    bool open(std::string& err) {
        prevConfig_ = device::prop("sys.usb.config");
        if (!prevConfig_.empty() && prevConfig_ != "none") {
            device::setProp("sys.usb.config", "none");  // stops adbd, unbinds the gadget
            waitProp("sys.usb.state", "none", 2000);
        }
        std::string ep0 = std::string(kFfs) + "/ep0";
        if (!util::exists(ep0)) manualSetup();
        ep0_ = ::open(ep0.c_str(), O_RDWR | O_CLOEXEC);
        if (ep0_ < 0) { err = ep0 + ": " + strerror(errno) + " (no FunctionFS for fastboot in this ramdisk)"; restore(); return false; }
        if (!writeDescriptors(err)) { restore(); return false; }
        out_ = ::open((std::string(kFfs) + "/ep1").c_str(), O_RDONLY | O_CLOEXEC);
        in_ = ::open((std::string(kFfs) + "/ep2").c_str(), O_WRONLY | O_CLOEXEC);
        if (out_ < 0 || in_ < 0) { err = std::string("bulk endpoints: ") + strerror(errno); restore(); return false; }
        stop_ = false;
        monitor_ = std::thread([this] { monitor(); });
        device::setProp("sys.usb.ffs.ready", "1");
        device::setProp("sys.usb.config", "fastboot");  // init binds the gadget (recovery init.rc)
        if (!waitProp("sys.usb.state", "fastboot", 1500) || util::readLine(std::string(kGadget) + "/UDC").empty()) bindManually();
        LOGI("fastboot: USB gadget ready (UDC %s)", util::readLine(std::string(kGadget) + "/UDC").c_str());
        return true;
    }

    ssize_t read(void* buf, size_t n) override {
        for (;;) {
            ssize_t r = ::read(out_, buf, std::min<size_t>(n, 1 << 20));
            if (r < 0 && errno == EINTR) continue;
            return r;
        }
    }
    bool write(const void* buf, size_t n) override {
        const char* p = (const char*)buf;
        while (n) {
            ssize_t r = ::write(in_, p, std::min<size_t>(n, 1 << 20));
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) return false;
            p += r;
            n -= (size_t)r;
        }
        return true;
    }
    // Unblocks a pending read (the function is disabled, so it fails with ESHUTDOWN).
    void interrupt() {
        device::setProp("sys.usb.config", "none");
        if (manualBound_) util::writeFile(std::string(kGadget) + "/UDC", "\n");
    }
    void shutdown() {
        if (ep0_ < 0 && out_ < 0) return;
        interrupt();
        stop_ = true;
        if (monitor_.joinable()) monitor_.join();
        for (int* fd : {&out_, &in_, &ep0_})
            if (*fd >= 0) { ::close(*fd); *fd = -1; }
        restore();
    }

private:
    bool writeDescriptors(std::string& err) {
        DescV2Os d{};
        d.head.magic = htole32(FUNCTIONFS_DESCRIPTORS_MAGIC_V2);
        d.head.flags = htole32(FUNCTIONFS_HAS_FS_DESC | FUNCTIONFS_HAS_HS_DESC | FUNCTIONFS_HAS_SS_DESC | FUNCTIONFS_HAS_MS_OS_DESC);
        d.head.length = htole32(sizeof(d));
        d.fsCount = htole32(3);
        d.hsCount = htole32(3);
        d.ssCount = htole32(5);
        d.osCount = htole32(1);
        d.fs = funcDesc(64);
        d.hs = funcDesc(512);
        d.ss = ssDesc();
        d.os.header.interface = 1;
        d.os.header.dwLength = htole32(sizeof(OsDesc));
        d.os.header.bcdVersion = htole16(1);
        d.os.header.wIndex = htole16(4);
        d.os.header.bCount = 1;
        d.os.compat.bFirstInterfaceNumber = 0;
        d.os.compat.Reserved1 = 1;
        memcpy(d.os.compat.CompatibleID, "WINUSB\0\0", 8);
        bool ok = ::write(ep0_, &d, sizeof(d)) == (ssize_t)sizeof(d);
        if (!ok) {  // kernels without MS OS descriptor support
            DescV2 d2{};
            d2.head.magic = htole32(FUNCTIONFS_DESCRIPTORS_MAGIC_V2);
            d2.head.flags = htole32(FUNCTIONFS_HAS_FS_DESC | FUNCTIONFS_HAS_HS_DESC | FUNCTIONFS_HAS_SS_DESC);
            d2.head.length = htole32(sizeof(d2));
            d2.fsCount = htole32(3);
            d2.hsCount = htole32(3);
            d2.ssCount = htole32(5);
            d2.fs = d.fs;
            d2.hs = d.hs;
            d2.ss = d.ss;
            ok = ::write(ep0_, &d2, sizeof(d2)) == (ssize_t)sizeof(d2);
        }
        if (!ok) { err = std::string("writing USB descriptors failed: ") + strerror(errno); return false; }
        Strings s{};
        s.head.magic = htole32(FUNCTIONFS_STRINGS_MAGIC);
        s.head.length = htole32(sizeof(s));
        s.head.str_count = htole32(1);
        s.head.lang_count = htole32(1);
        s.lang = htole16(0x0409);
        memcpy(s.str, "fastboot", sizeof(s.str));
        if (::write(ep0_, &s, sizeof(s)) != (ssize_t)sizeof(s)) { err = std::string("writing USB strings failed: ") + strerror(errno); return false; }
        return true;
    }

    // Control endpoint events. Setup requests nobody handles must be stalled, or hosts hang.
    void monitor() {
        while (!stop_) {
            pollfd p{ep0_, POLLIN, 0};
            if (poll(&p, 1, 200) <= 0) continue;
            usb_functionfs_event ev{};
            if (::read(ep0_, &ev, sizeof(ev)) != (ssize_t)sizeof(ev)) continue;
            switch (ev.type) {
                case FUNCTIONFS_ENABLE: LOGI("fastboot: USB host connected"); break;
                case FUNCTIONFS_DISABLE: LOGI("fastboot: USB host disconnected"); break;
                case FUNCTIONFS_SETUP:
                    if (ev.u.setup.bRequestType & USB_DIR_IN) { if (::write(ep0_, nullptr, 0) < 0) {} }
                    else if (::read(ep0_, nullptr, 0) < 0) {}
                    break;
                default: break;
            }
        }
    }

    // Ramdisks without AOSP's fastboot gadget rules: create the function ourselves.
    void manualSetup() {
        if (!util::isDir(kGadget)) {
            util::mkdirs("/config");
            mount("none", "/config", "configfs", 0, nullptr);
        }
        if (!util::isDir(kGadget)) return;
        std::string fn = std::string(kGadget) + "/functions/ffs.fastboot";
        if (!util::isDir(fn)) mkdir(fn.c_str(), 0770);
        util::mkdirs(kFfs);
        if (mount("fastboot", kFfs, "functionfs", 0, "uid=0,gid=0") != 0) LOGW("fastboot: mount functionfs: %s", strerror(errno));
    }
    void bindManually() {
        std::string g = kGadget;
        if (!util::isDir(g + "/functions/ffs.fastboot") || !util::isDir(g + "/configs/b.1")) return;
        LOGI("fastboot: binding the USB gadget ourselves");
        util::writeFile(g + "/UDC", "\n");
        util::writeFile(g + "/idVendor", "0x18D1");
        util::writeFile(g + "/idProduct", "0x4EE0");
        util::mkdirs(g + "/configs/b.1/strings/0x409");
        util::writeFile(g + "/configs/b.1/strings/0x409/configuration", "fastboot");
        for (auto& e : util::listDir(g + "/configs/b.1"))
            if (e.link) unlink((g + "/configs/b.1/" + e.name).c_str());
        if (symlink((g + "/functions/ffs.fastboot").c_str(), (g + "/configs/b.1/f1").c_str()) != 0) LOGW("fastboot: symlink f1: %s", strerror(errno));
        std::string udc = usbController();
        if (!udc.empty() && util::writeFile(g + "/UDC", udc)) manualBound_ = true;
    }
    void restore() {
        device::setProp("sys.usb.ffs.ready", "0");
        if (!prevConfig_.empty() && prevConfig_ != "fastboot") device::setProp("sys.usb.config", prevConfig_);
    }

    int ep0_ = -1, out_ = -1, in_ = -1;
    std::thread monitor_;
    std::atomic<bool> stop_{false};
    std::string prevConfig_;
    bool manualBound_ = false;
};

// ---- partitions -----------------------------------------------------------------------

struct Target {
    std::string name, dev;
    bool logical = false;
    uint64_t size = 0;
};

bool validName(const std::string& n) { return !n.empty() && n.size() < 64 && n.find('/') == std::string::npos && n.find("..") == std::string::npos; }

bool readMeta(lp::Metadata& m) {
    std::string e;
    return lp::read(lp::superDevice(), lp::currentSlotIndex(), m, e);
}

bool findTarget(const std::string& req, Target& t, bool map, std::string& err) {
    if (!validName(req)) { err = "bad partition name"; return false; }
    std::string slot = device::info().slot;
    std::vector<std::string> cands;
    if (!slot.empty() && !util::endsWith(req, "_a") && !util::endsWith(req, "_b")) cands.push_back(req + slot);
    cands.push_back(req);
    lp::Metadata m;
    bool haveLp = readMeta(m);
    for (auto& c : cands) {
        if (haveLp && m.find(c)) {
            t.name = c;
            t.logical = true;
            t.size = m.find(c)->bytes();
            t.dev = lp::mappedPath(c);
            if (map) {
                if (t.dev.empty() && !lp::map(m, c, true, err)) return false;
                t.dev = lp::mappedPath(c);
                if (t.dev.empty()) { err = c + " is empty (resize it first)"; return false; }
                if (!lp::ensureWritable(t.dev, err)) return false;
            }
            return true;
        }
        std::string by = Partitions::byNameDir() + "/" + c;
        if (util::exists(by)) {
            t.name = c;
            t.dev = by;
            t.size = Partitions::blockSize(by);
            if (map && !lp::ensureWritable(by, err)) return false;
            return true;
        }
    }
    err = "No partition named " + req;
    return false;
}

void unmountTarget(const Target& t) {
    char a[PATH_MAX], b[PATH_MAX];
    if (!realpath(t.dev.c_str(), a)) return;
    for (auto& p : Partitions::get().all())
        if (p.mounted && realpath(p.blockDev.c_str(), b) && strcmp(a, b) == 0) {
            LOGI("fastboot: unmounting %s", p.mountPoint.c_str());
            Partitions::get().unmount(p);
        }
}

std::vector<std::string> partitionNames() {
    std::vector<std::string> out;
    for (auto& e : util::listDir(Partitions::byNameDir())) out.push_back(e.name);
    lp::Metadata m;
    if (readMeta(m))
        for (auto& p : m.parts) out.push_back(p.name);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::string hex64(uint64_t v) {
    char b[32];
    snprintf(b, sizeof(b), "0x%" PRIx64, v);
    return b;
}

std::string partitionType(const std::string& name) {
    std::string base = name;
    for (auto s : {"_a", "_b"})
        if (util::endsWith(base, s)) base = base.substr(0, base.size() - 2);
    if (base == "userdata") base = "data";
    if (Partition* p = Partitions::get().find("/" + base))
        if (p->fsType == "ext4" || p->fsType == "f2fs") return p->fsType;
    return "raw";
}

}  // namespace

uint64_t maxDownloadSize() {
    device::Mem m = device::memory();
    uint64_t want = 256ull << 20;
    if (m.avail && m.avail / 3 < want) want = std::max<uint64_t>(32ull << 20, m.avail / 3 / (1 << 20) * (1 << 20));
    return want;
}

bool getvar(const std::string& name, std::string& v) {
    const device::Info& di = device::info();
    auto colon = name.find(':');
    std::string key = name.substr(0, colon), arg = colon == std::string::npos ? "" : name.substr(colon + 1);
    std::string slot = di.slot.empty() ? "" : di.slot.substr(1);
    Target t;
    std::string err;
    if (key == "version") v = "0.4";
    else if (key == "version-bootloader") v = device::prop("ro.bootloader", "unknown");
    else if (key == "version-baseband") v = device::prop("ro.build.expect.baseband", device::prop("gsm.version.baseband", "unknown"));
    else if (key == "version-os") v = di.android;
    else if (key == "version-vndk") v = device::prop("ro.vndk.version", di.sdk);
    else if (key == "product") v = di.device;
    else if (key == "serialno") v = di.serial;
    else if (key == "secure") v = device::prop("ro.secure", "1") == "1" ? "yes" : "no";
    else if (key == "is-userspace") v = "yes";
    else if (key == "unlocked") v = device::prop("ro.boot.verifiedbootstate") == "green" ? "no" : "yes";
    else if (key == "max-download-size" || key == "max-fetch-size") v = hex64(maxDownloadSize());
    else if (key == "current-slot") { if (slot.empty()) return false; v = slot; }
    else if (key == "slot-count") v = di.ab ? "2" : "0";
    else if (key == "has-slot") {
        if (arg.empty()) return false;
        std::string by = Partitions::byNameDir() + "/";
        lp::Metadata m;
        bool lp = readMeta(m);
        v = (util::exists(by + arg + "_a") || (lp && m.find(arg + "_a"))) ? "yes" : "no";
    } else if (key == "partition-size") {
        if (!findTarget(arg, t, false, err)) return false;
        v = hex64(t.size);
    } else if (key == "partition-type") {
        if (!findTarget(arg, t, false, err)) return false;
        v = partitionType(t.name);
    } else if (key == "is-logical") {
        if (!findTarget(arg, t, false, err)) return false;
        v = t.logical ? "yes" : "no";
    } else if (key == "super-partition-name") {
        std::string s = util::basename(lp::superDevice());
        if (!slot.empty() && util::endsWith(s, di.slot)) s = s.substr(0, s.size() - 2);
        v = s;
    } else if (key == "snapshot-update-status") {
        std::string why;
        v = !lp::busy(why) ? "none" : why.find("merg") != std::string::npos ? "merging" : "snapshotted";
    } else if (key == "battery-voltage") v = std::to_string(std::max(0, device::battery().voltageMv));
    else if (key == "battery-soc-ok") v = device::battery().percent < 0 || device::battery().percent >= 10 ? "yes" : "no";
    else if (key == "off-mode-charge") v = "0";
    else if (key == "hw-revision") v = device::prop("ro.revision", "unknown");
    else if (key == "variant") v = device::prop("ro.boot.hardware.sku", "");
    else if (key == "cpu-abi") v = device::prop("ro.product.cpu.abi", device::buildAbi());
    else if (key == "dynamic-partition") { lp::Metadata m; v = readMeta(m) ? "true" : "false"; }
    else if (key == "security-patch-level") v = di.patch;
    else if (key == "first-api-level") v = device::prop("ro.product.first_api_level", di.sdk);
    else if (key == "treble-enabled") v = device::prop("ro.treble.enabled", "true");
    else if (key == "recovery") v = "TWRP v2";
    else return false;
    return true;
}

namespace {

// ---- the protocol -----------------------------------------------------------------------

enum class Exit { Disconnected, Reboot, Recovery, Reconnect };

class Engine {
public:
    Engine(Transport& tr, Task& t, std::vector<uint8_t>& buf) : tr_(tr), t_(t), buf_(buf) {}

    Exit serve(std::string& rebootTo) {
        char cmd[kMaxCommand + 1];
        for (;;) {
            ssize_t n = tr_.read(cmd, kMaxCommand);
            if (n <= 0) return Exit::Disconnected;
            cmd[n] = 0;
            std::string c(cmd, strnlen(cmd, (size_t)n));
            LOGI("fastboot: %s", c.c_str());
            setState([&](State& s) {
                s.command = c;
                s.result.clear();
                s.commands++;
                s.history.push_back(c);
                if (s.history.size() > 40) s.history.erase(s.history.begin());
            });
            t_.setStatus("fastboot " + c.substr(0, 48));
            exit_ = Exit::Disconnected;
            done_ = false;
            if (!handle(c)) return Exit::Disconnected;  // transport broke while answering
            if (done_) {
                rebootTo = rebootTo_;
                return exit_;
            }
        }
    }

private:
    bool send(const char* kind, const std::string& msg) {
        std::string r = kind + msg.substr(0, kMaxResponse - 4);
        if (kind[0] == 'O' || kind[0] == 'F')
            setState([&](State& s) { s.result = r; s.done = s.total = 0; });
        return tr_.write(r.data(), r.size());
    }
    bool okay(const std::string& m = "") { return send("OKAY", m); }
    bool fail(const std::string& m) { t_.print("fastboot: FAIL %s", m.c_str()); return send("FAIL", m); }
    bool info(const std::string& m) {
        for (size_t i = 0; i < m.size() || i == 0; i += kMaxResponse - 4)
            if (!send("INFO", m.substr(i, kMaxResponse - 4))) return false;
        return true;
    }
    void progress(uint64_t done, uint64_t total) {
        setState([&](State& s) { s.done = done; s.total = total; });
        if (total) t_.setProgress(float(double(done) / double(total)));
    }

    bool handle(const std::string& c) {
        auto arg = [&](const char* prefix) { return c.substr(strlen(prefix)); };
        if (util::startsWith(c, "getvar:")) return doGetvar(arg("getvar:"));
        if (util::startsWith(c, "download:")) return doDownload(arg("download:"));
        if (util::startsWith(c, "flash:")) return doFlash(arg("flash:"));
        if (util::startsWith(c, "erase:")) return doErase(arg("erase:"));
        if (util::startsWith(c, "set_active:")) return doSetActive(arg("set_active:"));
        if (util::startsWith(c, "resize-logical-partition:")) return doLogical('r', arg("resize-logical-partition:"));
        if (util::startsWith(c, "create-logical-partition:")) return doLogical('c', arg("create-logical-partition:"));
        if (util::startsWith(c, "delete-logical-partition:")) return doLogical('d', arg("delete-logical-partition:"));
        if (util::startsWith(c, "update-super:")) return doUpdateSuper(arg("update-super:"));
        if (util::startsWith(c, "fetch:")) return doFetch(arg("fetch:"));
        if (util::startsWith(c, "oem ")) return doOem(util::trim(arg("oem ")));
        if (c == "reboot" || c == "continue") return finish(Exit::Reboot, "", "Rebooting");
        if (c == "reboot-bootloader") return finish(Exit::Reboot, "bootloader", "Rebooting to the bootloader");
        if (c == "reboot-recovery") return finish(Exit::Recovery, "recovery", "Back to TWRP v2");
        if (c == "reboot-fastboot") return finish(Exit::Reconnect, "", "Restarting fastbootd");
        if (util::startsWith(c, "snapshot-update")) {
            std::string why;
            if (!lp::busy(why)) return okay();
            return fail("Virtual A/B update pending (" + why + "): finish or cancel it in Android first");
        }
        if (c == "boot" || util::startsWith(c, "boot:")) return fail("fastboot boot needs the bootloader: fastboot reboot bootloader");
        if (util::startsWith(c, "flashing")) return fail("lock/unlock is done in the bootloader: fastboot reboot bootloader");
        if (util::startsWith(c, "gsi")) return fail("DSU (gsi) commands are not supported");
        return fail("unknown command");
    }

    bool finish(Exit e, const std::string& target, const std::string& msg) {
        t_.print("fastboot: %s", msg.c_str());
        exit_ = e;
        rebootTo_ = target;
        done_ = true;
        return okay();
    }

    bool doGetvar(const std::string& name) {
        if (name != "all") {
            std::string v;
            return getvar(name, v) ? okay(v) : fail("Variable not found");
        }
        const char* vars[] = {"version", "version-bootloader", "version-baseband", "version-os", "version-vndk", "product", "serialno",
                              "secure", "is-userspace", "unlocked", "max-download-size", "max-fetch-size", "current-slot", "slot-count",
                              "super-partition-name", "snapshot-update-status", "dynamic-partition", "security-patch-level",
                              "first-api-level", "battery-voltage", "battery-soc-ok", "off-mode-charge", "hw-revision", "cpu-abi",
                              "treble-enabled", "recovery"};
        for (auto k : vars) {
            std::string v;
            if (getvar(k, v) && !info(std::string(k) + ":" + v)) return false;
        }
        std::vector<std::string> bases;
        for (auto& p : partitionNames()) {
            for (auto k : {"partition-size", "partition-type", "is-logical"}) {
                std::string v;
                if (getvar(std::string(k) + ":" + p, v) && !info(std::string(k) + ":" + p + ":" + v)) return false;
            }
            if (util::endsWith(p, "_a")) bases.push_back(p.substr(0, p.size() - 2));
        }
        for (auto& b : bases)
            if (!info("has-slot:" + b + ":yes")) return false;
        return okay();
    }

    bool doDownload(const std::string& sz) {
        uint64_t n = strtoull(sz.c_str(), nullptr, 16);
        if (!n || n > maxDownloadSize()) return fail("bad download size (max " + hex64(maxDownloadSize()) + ")");
        try {
            buf_.resize((size_t)n);
        } catch (...) {
            return fail("not enough memory");
        }
        char d[24];
        snprintf(d, sizeof(d), "%08" PRIx64, n);
        if (!send("DATA", d)) return false;
        size_t got = 0;
        while (got < n) {
            ssize_t r = tr_.read(buf_.data() + got, (size_t)std::min<uint64_t>(n - got, 1 << 20));
            if (r <= 0) { buf_.clear(); return false; }
            got += (size_t)r;
            progress(got, n);
        }
        size_ = (size_t)n;
        t_.print("fastboot: received %s", util::humanSize(n).c_str());
        return okay();
    }

    bool doFlash(const std::string& name) {
        if (!size_) return fail("nothing downloaded");
        Target tg;
        std::string err;
        if (!findTarget(name, tg, true, err)) return fail(err);
        // Writing super itself (fastboot's "optimized" flashall/update does that) replaces the partition
        // table: nothing may keep using the old extents.
        char rs[PATH_MAX], rt[PATH_MAX];
        bool isSuper = !tg.logical && realpath(lp::superDevice().c_str(), rs) && realpath(tg.dev.c_str(), rt) && strcmp(rs, rt) == 0;
        if (isSuper && !lp::unmapAll(t_)) return fail("a logical partition is still in use; unmount it first");
        unmountTarget(tg);
        int fd = ::open(tg.dev.c_str(), O_RDWR | O_CLOEXEC);
        if (fd < 0) return fail("open " + tg.name + ": " + strerror(errno));
        bool ok;
        const uint8_t* d = buf_.data();
        uint32_t magic = size_ >= 4 ? uint32_t(d[0] | d[1] << 8 | d[2] << 16 | uint32_t(d[3]) << 24) : 0;
        t_.print("fastboot: writing %s to %s%s", util::humanSize(size_).c_str(), tg.name.c_str(), magic == 0xED26FF3A ? " (sparse)" : "");
        if (magic == 0xED26FF3A) ok = writeSparse(fd, tg, err);
        else if (tg.size && size_ > tg.size) { ok = false; err = "image (" + util::humanSize(size_) + ") is larger than " + tg.name + " (" + util::humanSize(tg.size) + ")"; }
        else {
            ok = true;
            for (size_t off = 0; off < size_ && ok; off += 1 << 20) {
                size_t k = std::min<size_t>(1 << 20, size_ - off);
                ok = pwriteAll(fd, d + off, k, off);
                progress(off + k, size_);
            }
            if (!ok) err = std::string("write failed: ") + strerror(errno);
        }
        if (ok && fsync(fd) != 0 && errno != EINVAL) { ok = false; err = std::string("fsync: ") + strerror(errno); }
        ::close(fd);
        if (!ok) return fail(err);
        if (isSuper) lp::simSyncFromSuper();
        t_.print("fastboot: %s flashed", tg.name.c_str());
        return okay();
    }

    // Android sparse image: one piece of a possibly split image; DONT_CARE leaves blocks alone.
    bool writeSparse(int fd, const Target& tg, std::string& err) {
        auto u16 = [&](size_t o) { return uint32_t(buf_[o] | buf_[o + 1] << 8); };
        auto u32 = [&](size_t o) { return uint32_t(buf_[o] | buf_[o + 1] << 8 | buf_[o + 2] << 16 | uint32_t(buf_[o + 3]) << 24); };
        if (size_ < 28) { err = "truncated sparse header"; return false; }
        uint32_t hdr = u16(8), chdr = u16(10), bs = u32(12), blocks = u32(16), chunks = u32(20);
        if (u16(4) != 1 || hdr < 28 || chdr < 12 || !bs || bs % 4) { err = "unsupported sparse image"; return false; }
        if (tg.size && uint64_t(blocks) * bs > tg.size) {
            err = "image (" + util::humanSize(uint64_t(blocks) * bs) + ") is larger than " + tg.name + " (" + util::humanSize(tg.size) + ")";
            return false;
        }
        size_t pos = hdr;
        uint64_t out = 0;
        std::vector<uint8_t> fill;
        for (uint32_t i = 0; i < chunks; i++) {
            if (pos + chdr > size_) { err = "truncated sparse chunk"; return false; }
            uint32_t type = u16(pos), n = u32(pos + 4), total = u32(pos + 8);
            size_t data = pos + chdr;
            uint64_t bytes = uint64_t(n) * bs;
            if (total < chdr || pos + total > size_) { err = "bad sparse chunk"; return false; }
            switch (type) {
                case 0xCAC1:
                    if (total - chdr != bytes) { err = "bad raw chunk"; return false; }
                    if (!pwriteAll(fd, buf_.data() + data, (size_t)bytes, out)) { err = std::string("write failed: ") + strerror(errno); return false; }
                    break;
                case 0xCAC2: {
                    if (total - chdr < 4) { err = "bad fill chunk"; return false; }
                    if (fill.empty()) fill.resize(1 << 20);
                    for (size_t k = 0; k < fill.size(); k += 4) memcpy(&fill[k], buf_.data() + data, 4);
                    for (uint64_t o = 0; o < bytes;) {
                        size_t k = (size_t)std::min<uint64_t>(fill.size(), bytes - o);
                        if (!pwriteAll(fd, fill.data(), k, out + o)) { err = std::string("write failed: ") + strerror(errno); return false; }
                        o += k;
                    }
                    break;
                }
                case 0xCAC3: case 0xCAC4: break;  // don't care / crc32
                default: err = "unknown sparse chunk type"; return false;
            }
            out += type == 0xCAC4 ? 0 : bytes;
            pos += total;
            progress(pos, size_);
        }
        return true;
    }

    bool doErase(const std::string& name) {
        Target tg;
        std::string err;
        if (!findTarget(name, tg, true, err)) return fail(err);
        unmountTarget(tg);
        int fd = ::open(tg.dev.c_str(), O_RDWR | O_CLOEXEC);
        if (fd < 0) return fail("open " + tg.name + ": " + strerror(errno));
        struct stat st;
        fstat(fd, &st);
        bool ok = true;
        if (S_ISBLK(st.st_mode)) {
            uint64_t r[2] = {0, tg.size};
            if (ioctl(fd, BLKDISCARD, r) != 0) LOGW("fastboot: BLKDISCARD %s: %s", tg.name.c_str(), strerror(errno));
            // Discarded blocks don't always read back as zero: clear the start and the end (superblocks, footers).
            for (uint64_t off : {uint64_t(0), tg.size > (1u << 20) ? tg.size - (1u << 20) : uint64_t(0)}) {
                uint64_t z[2] = {off, std::min<uint64_t>(16u << 20, tg.size - off)};
                if (ioctl(fd, BLKZEROOUT, z) != 0) {
                    std::vector<uint8_t> zeros((size_t)z[1], 0);
                    ok &= pwriteAll(fd, zeros.data(), zeros.size(), off);
                }
            }
        } else {
            ok = ftruncate(fd, 0) == 0 && ftruncate(fd, (off_t)tg.size) == 0;
        }
        fsync(fd);
        ::close(fd);
        if (!ok) return fail(std::string("erase failed: ") + strerror(errno));
        t_.print("fastboot: erased %s", tg.name.c_str());
        return okay();
    }

    bool doSetActive(const std::string& s) {
        std::string msg;
        if (s != "a" && s != "b" && s != "_a" && s != "_b") return fail("slot must be a or b");
        if (!device::setActiveSlot(s[0] == '_' ? s : "_" + s, msg)) return fail(msg);
        t_.print("fastboot: %s", msg.c_str());
        return okay();
    }

    bool doLogical(char op, const std::string& a) {
        auto parts = util::split(a, ':');
        if (parts.empty() || !validName(parts[0])) return fail("bad partition name");
        const std::string& name = parts[0];
        uint64_t size = parts.size() > 1 ? strtoull(parts[1].c_str(), nullptr, 0) : 0;
        bool ok = false;
        if (op == 'r') {
            if (parts.size() < 2) return fail("usage: resize-logical-partition:<name>:<size>");
            lp::Metadata m;
            if (readMeta(m) && m.find(name) && m.find(name)->bytes() == (size + m.blockSize - 1) / m.blockSize * m.blockSize) return okay();
            ok = lp::resizePartition(name, size, t_);
        } else if (op == 'c') {
            if (parts.size() < 2) return fail("usage: create-logical-partition:<name>:<size>");
            ok = lp::createPartition(name, "default", size, t_);
        } else {
            ok = lp::deletePartition(name, t_);
        }
        if (!ok) {
            std::vector<std::string> l = t_.lines(1);
            return fail(l.empty() ? "failed" : l.back());
        }
        return okay();
    }

    // update-super:<super>[:wipe] with super_empty.img downloaded: the new partition table. Without
    // wipe, the other slot's partitions are kept (fastbootd does the same).
    bool doUpdateSuper(const std::string& a) {
        if (!size_) return fail("download super_empty.img first");
        bool wipe = util::endsWith(a, ":wipe");
        std::string err;
        lp::Metadata nm, cur;
        if (!lp::parseImage(std::string((const char*)buf_.data(), size_), nm, err)) return fail("not a super_empty.img: " + err);
        std::string dev = lp::superDevice();
        bool haveCur = lp::read(dev, lp::currentSlotIndex(), cur, err);
        if (haveCur && (nm.devs.size() != cur.devs.size() || nm.devs[0].size != cur.devs[0].size || nm.maxSize != cur.maxSize ||
                        nm.slotCount != cur.slotCount || nm.devs[0].firstSector != cur.devs[0].firstSector))
            return fail("super_empty.img doesn't match this phone's super partition (another model?)");
        std::string why;
        if (lp::busy(why)) return fail(why);
        if (haveCur && !wipe) {
            const std::string slot = device::info().slot;
            for (auto& p : cur.parts) {
                if ((!slot.empty() && util::endsWith(p.name, slot)) || p.name == "scratch" || p.extents.empty()) continue;
                const std::string& g = cur.groups[p.group].name;
                if (g == "cow") continue;
                int gi = nm.groupIndex(g);
                if (gi < 0) { nm.groups.push_back(cur.groups[p.group]); gi = (int)nm.groups.size() - 1; }
                lp::Part np = p;
                np.group = (uint32_t)gi;
                if (lp::Part* e = nm.find(p.name)) *e = np;
                else nm.parts.push_back(np);
            }
        }
        if (!nm.validate(err)) return fail("new layout is invalid: " + err);
        std::string backup = lp::backupDir() + "/super_metadata_" + util::dateStamp() + "_update-super.bin";
        if (haveCur && !lp::saveMetadataRegion(dev, backup, err)) return fail("cannot back up the current metadata: " + err);
        // The partition table changes under any mapped partition: unmap them all (remapped on demand).
        if (!lp::unmapAll(t_)) return fail("a logical partition is still in use; unmount it first");
        if (!lp::write(dev, nm, err)) return fail("writing metadata failed: " + err);
        t_.print("fastboot: super partition table updated%s (backup: %s)", wipe ? " (wiped)" : "", backup.c_str());
        return okay();
    }

    bool doFetch(const std::string& a) {
        auto parts = util::split(a, ':');
        if (parts.empty()) return fail("usage: fetch:<partition>[:offset:size]");
        Target tg;
        std::string err;
        if (!findTarget(parts[0], tg, false, err)) return fail(err);
        if (tg.dev.empty()) return fail(tg.name + " is not mapped");
        uint64_t off = parts.size() > 1 ? strtoull(parts[1].c_str(), nullptr, 0) : 0;
        uint64_t len = parts.size() > 2 ? strtoull(parts[2].c_str(), nullptr, 0) : tg.size - std::min(off, tg.size);
        if (off > tg.size || len > tg.size - off) return fail("range outside the partition");
        if (len > maxDownloadSize()) return fail("too large: fetch in pieces of max-fetch-size");
        int fd = ::open(tg.dev.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) return fail("open " + tg.name + ": " + strerror(errno));
        char d[24];
        snprintf(d, sizeof(d), "%08" PRIx64, len);
        if (!send("DATA", d)) { ::close(fd); return false; }
        std::vector<uint8_t> chunk(1 << 20);
        for (uint64_t done = 0; done < len;) {
            size_t k = (size_t)std::min<uint64_t>(chunk.size(), len - done);
            if (!preadAll(fd, chunk.data(), k, off + done)) memset(chunk.data(), 0, k);
            if (!tr_.write(chunk.data(), k)) { ::close(fd); return false; }
            done += k;
            progress(done, len);
        }
        ::close(fd);
        return okay();
    }

    bool doOem(const std::string& a) {
        auto sp = a.find(' ');
        std::string cmd = a.substr(0, sp), rest = sp == std::string::npos ? "" : util::trim(a.substr(sp + 1));
        if (cmd == "help" || cmd.empty()) {
            for (auto l : {"oem twrp <command>   run a TWRP v2 command (twrp help), e.g. oem twrp vbmeta disable",
                           "oem log [lines]      the end of the recovery log", "oem getprop <name>   a system property"})
                if (!info(l)) return false;
            return okay();
        }
        if (cmd == "getprop") return okay(device::prop(rest));
        if (cmd == "log") {
            for (auto& l : log::tail((size_t)std::max(1, rest.empty() ? 30 : atoi(rest.c_str()))))
                if (!info(l.text)) return false;
            return okay();
        }
        if (cmd == "twrp") {
            if (rest.empty() || util::startsWith(rest, "fastboot")) return fail("usage: oem twrp <command>");
            size_t cursor = t_.lines(1000000).size();
            const float rs = t_.rangeStart(), rl = t_.rangeLen();
            bool ok = ors::run({rest}, t_);
            t_.setRange(rs, rl);
            auto all = t_.lines(1000000);
            for (size_t i = std::min(cursor, all.size()); i < all.size(); i++)
                if (!info(all[i])) return false;
            return ok ? okay() : fail("'" + rest + "' failed");
        }
        return fail("unknown oem command (fastboot oem help)");
    }

    Transport& tr_;
    Task& t_;
    std::vector<uint8_t>& buf_;
    size_t size_ = 0;
    Exit exit_ = Exit::Disconnected;
    bool done_ = false;
    std::string rebootTo_;
};

}  // namespace

bool run(const Options& o, Task& t, std::string& rebootTo) {
    setState([](State& s) {
        s.transport.clear();
        s.peer.clear();
        s.connected = false;
        s.command.clear();
        s.result.clear();
        s.done = s.total = 0;
        s.history.clear();
        s.commands = 0;
    });
    rebootTo = "recovery";
    auto cancelled = [&t] { return t.cancelled(); };
    std::vector<uint8_t> buf;
    std::unique_ptr<UsbTransport> usb;
    int lfd = -1;
    std::string err;
    bool useUsb = o.usb && !device::isSim();
    if (useUsb) {
        usb = std::make_unique<UsbTransport>();
        if (!usb->open(err)) {
            t.print("USB fastboot is not available: %s", err.c_str());
            usb.reset();
            if (!o.tcpPort) return false;
            useUsb = false;
        }
    }
    if (!useUsb) {
        int port = o.tcpPort ? o.tcpPort : 5554;
        lfd = tcpListen(o.tcpBind, port, err);
        if (lfd < 0) { t.print("Cannot listen for fastboot over TCP: %s", err.c_str()); return false; }
        setState([&](State& s) { s.transport = "TCP " + o.tcpBind + ":" + std::to_string(port); });
        t.print("fastbootd: waiting on TCP port %d   (fastboot -s tcp:<this device's IP>:%d devices)", port, port);
    } else {
        setState([](State& s) { s.transport = "USB"; });
        t.print("fastbootd: waiting for a computer on USB   (fastboot devices)");
    }
    t.setStatus("Waiting for a computer");
    std::atomic<bool> finished{false};
    std::thread watcher;
    if (usb)
        watcher = std::thread([&] {  // a USB read can only be interrupted by disabling the function
            while (!finished && !t.cancelled()) usleep(200 * 1000);
            if (t.cancelled() && usb) usb->interrupt();
        });
    Exit last = Exit::Disconnected;
    while (!t.cancelled()) {
        std::unique_ptr<Transport> tcp;
        Transport* tr = usb.get();
        if (!usb) {
            tcp = tcpAccept(lfd, cancelled);
            if (!tcp) break;
            tr = tcp.get();
        }
        setState([&](State& s) { s.connected = true; s.peer = tr->peer(); });
        if (!usb) t.print("fastbootd: %s connected", tr->peer().c_str());
        Engine e(*tr, t, buf);
        std::string target;
        last = e.serve(target);
        setState([](State& s) { s.connected = false; });
        if (last == Exit::Reboot || last == Exit::Recovery) { rebootTo = target; break; }
        if (last == Exit::Reconnect && usb) {  // re-enumerate, like a restart of fastbootd
            usb->shutdown();
            usleep(300 * 1000);
            if (!usb->open(err)) { t.print("USB fastboot restart failed: %s", err.c_str()); break; }
        } else if (usb && last == Exit::Disconnected) {
            usleep(300 * 1000);  // cable unplugged (or host gone): wait for the next one
        }
        if (!usb && last == Exit::Disconnected) t.print("fastbootd: disconnected");
    }
    finished = true;
    if (watcher.joinable()) watcher.join();
    if (usb) usb->shutdown();
    if (lfd >= 0) close(lfd);
    buf.clear();
    buf.shrink_to_fit();
    if (last == Exit::Reboot) {
        t.print("Rebooting%s...", rebootTo.empty() ? "" : (" to " + rebootTo).c_str());
        device::reboot(rebootTo);
        return true;
    }
    t.print(t.cancelled() ? "Left fastboot mode." : "fastbootd: back to TWRP v2");
    return true;
}

}  // namespace tw::fastboot
