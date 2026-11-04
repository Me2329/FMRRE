#include "sys/adbd.h"

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <cinttypes>
#include <cstring>
#include <endian.h>
#include <fcntl.h>
#include <linux/usb/functionfs.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <thread>
#include <unistd.h>

#include "core/log.h"
#include "core/task.h"
#include "core/util.h"
#include "platform/device.h"
#include "sys/partitions.h"

namespace tw::adbd {

namespace {

constexpr uint32_t A_CNXN = 0x4e584e43, A_OPEN = 0x4e45504f, A_OKAY = 0x59414b4f, A_CLSE = 0x45534c43, A_WRTE = 0x45545257;
constexpr uint32_t kVersion = 0x01000001;  // A_VERSION_SKIP_CHECKSUM
constexpr uint32_t kMaxPayload = 1024 * 1024;
constexpr uint64_t kMaxPackage = uint64_t(64) << 30;

// ---- links ---------------------------------------------------------------------------------------

class Link {
public:
    virtual ~Link() = default;
    virtual ssize_t read(void* buf, size_t n) = 0;  // <= 0: gone
    virtual bool write(const void* buf, size_t n) = 0;
    virtual bool separatePayload() const = 0;       // USB: header and data are separate transfers
    virtual void interrupt() = 0;                    // unblock read() from another thread
    bool readFull(void* buf, size_t n) {
        char* p = (char*)buf;
        while (n) {
            ssize_t r = read(p, n);
            if (r <= 0) return false;
            p += r;
            n -= size_t(r);
        }
        return true;
    }
};

class TcpLink : public Link {
public:
    explicit TcpLink(int fd) : fd_(fd) {}
    ~TcpLink() override { close(fd_); }
    ssize_t read(void* buf, size_t n) override {
        for (;;) {
            ssize_t r = recv(fd_, buf, n, 0);
            if (r < 0 && errno == EINTR) continue;
            return r;
        }
    }
    bool write(const void* buf, size_t n) override {
        const char* p = (const char*)buf;
        while (n) {
            ssize_t r = send(fd_, p, n, MSG_NOSIGNAL);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) return false;
            p += r;
            n -= size_t(r);
        }
        return true;
    }
    bool separatePayload() const override { return false; }
    void interrupt() override { ::shutdown(fd_, SHUT_RDWR); }

private:
    int fd_;
};

// USB: the adb function of the recovery's gadget (FunctionFS), which adbd uses the rest of the time.
constexpr const char* kFfs = "/dev/usb-ffs/adb";
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
struct OsCompat {  // Microsoft OS descriptors: Windows binds WinUSB without a driver...
    usb_os_desc_header header;
    usb_ext_compat_desc compat;
} __attribute__((packed));
struct OsProp {  // ...and adb.exe finds the device by this interface GUID
    usb_os_desc_header header;
    usb_ext_prop_desc prop;
    uint16_t name[20];  // "DeviceInterfaceGUID", UTF-16LE
    uint32_t dataLength;
    uint16_t data[39];  // "{F72FE0D4-CBCB-407D-8814-9ED673D0DD6B}"
} __attribute__((packed));
struct DescHead {
    usb_functionfs_descs_head_v2 head;
    uint32_t fsCount, hsCount, ssCount;
} __attribute__((packed));
struct DescBody {
    FuncDesc fs, hs;
    SsFuncDesc ss;
} __attribute__((packed));
struct Strings {
    usb_functionfs_strings_head head;
    uint16_t lang;
    char str[sizeof("ADB Interface")];
} __attribute__((packed));

usb_interface_descriptor intfDesc() {
    usb_interface_descriptor d{};
    d.bLength = USB_DT_INTERFACE_SIZE;
    d.bDescriptorType = USB_DT_INTERFACE;
    d.bNumEndpoints = 2;
    d.bInterfaceClass = 0xff;     // vendor specific
    d.bInterfaceSubClass = 0x42;  // adb
    d.bInterfaceProtocol = 0x01;
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
    c.bMaxBurst = 4;
    return c;
}

bool waitProp(const std::string& key, const std::string& want, int ms) {
    for (int i = 0; i < ms / 50; i++) {
        if (device::prop(key) == want) return true;
        usleep(50 * 1000);
    }
    return false;
}

class UsbLink : public Link {
public:
    ~UsbLink() override { shutdown(); }

    bool open(std::string& err) {
        prevConfig_ = device::prop("sys.usb.config");
        if (!prevConfig_.empty() && prevConfig_ != "none") {
            device::setProp("sys.usb.config", "none");  // stops adbd, unbinds the gadget
            waitProp("sys.usb.state", "none", 2000);
        }
        device::setProp("ctl.stop", "adbd");
        std::string ep0 = std::string(kFfs) + "/ep0";
        if (!util::exists(ep0)) manualSetup();
        for (int i = 0; i < 20 && ep0_ < 0; i++) {  // adbd may still be letting go of it
            ep0_ = ::open(ep0.c_str(), O_RDWR | O_CLOEXEC);
            if (ep0_ < 0) usleep(100 * 1000);
        }
        if (ep0_ < 0) {
            err = ep0 + ": " + strerror(errno) + " (no adb FunctionFS in this ramdisk)";
            restore();
            return false;
        }
        if (!writeDescriptors(err)) {
            restore();
            return false;
        }
        out_ = ::open((std::string(kFfs) + "/ep1").c_str(), O_RDONLY | O_CLOEXEC);
        in_ = ::open((std::string(kFfs) + "/ep2").c_str(), O_WRONLY | O_CLOEXEC);
        if (out_ < 0 || in_ < 0) {
            err = std::string("bulk endpoints: ") + strerror(errno);
            restore();
            return false;
        }
        stop_ = false;
        monitor_ = std::thread([this] { monitor(); });
        device::setProp("sys.usb.ffs.ready", "1");
        device::setProp("sys.usb.config", "sideload");  // init binds the gadget (recovery's init.rc)
        std::string udc = util::readLine(std::string(kGadget) + "/UDC");
        if (!waitProp("sys.usb.state", "sideload", 1500) || udc.empty() || udc == "none") bindManually();
        LOGI("adbd: USB ready (UDC %s)", util::readLine(std::string(kGadget) + "/UDC").c_str());
        return true;
    }
    ssize_t read(void* buf, size_t n) override {
        for (;;) {
            ssize_t r = ::read(out_, buf, std::min<size_t>(n, kMaxPayload));
            if (r < 0 && errno == EINTR) continue;
            return r;
        }
    }
    bool write(const void* buf, size_t n) override {
        const char* p = (const char*)buf;
        while (n) {
            ssize_t r = ::write(in_, p, std::min<size_t>(n, kMaxPayload));
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) return false;
            p += r;
            n -= size_t(r);
        }
        return true;
    }
    bool separatePayload() const override { return true; }
    void interrupt() override {
        device::setProp("sys.usb.config", "none");
        if (manualBound_) util::writeFile(std::string(kGadget) + "/UDC", "\n");
    }
    void shutdown() {
        if (ep0_ < 0 && out_ < 0) return;
        interrupt();
        stop_ = true;
        if (monitor_.joinable()) monitor_.join();
        for (int* fd : {&out_, &in_, &ep0_})
            if (*fd >= 0) ::close(*fd), *fd = -1;
        restore();
    }

private:
    bool writeDescriptors(std::string& err) {
        DescBody body{{intfDesc(), epDesc(1 | USB_DIR_OUT, 64), epDesc(2 | USB_DIR_IN, 64)},
                      {intfDesc(), epDesc(1 | USB_DIR_OUT, 512), epDesc(2 | USB_DIR_IN, 512)},
                      {intfDesc(), epDesc(1 | USB_DIR_OUT, 1024), ssComp(), epDesc(2 | USB_DIR_IN, 1024), ssComp()}};
        OsCompat compat{};
        compat.header.interface = 0;
        compat.header.dwLength = htole32(sizeof(OsCompat));
        compat.header.bcdVersion = htole16(1);
        compat.header.wIndex = htole16(4);
        compat.header.bCount = 1;
        compat.compat.bFirstInterfaceNumber = 0;
        compat.compat.Reserved1 = 1;
        memcpy(compat.compat.CompatibleID, "WINUSB\0\0", 8);
        OsProp prop{};
        prop.header.interface = 0;
        prop.header.dwLength = htole32(sizeof(OsProp));
        prop.header.bcdVersion = htole16(1);
        prop.header.wIndex = htole16(5);
        prop.header.wCount = htole16(1);
        prop.prop.dwSize = htole32(sizeof(OsProp) - sizeof(usb_os_desc_header));
        prop.prop.dwPropertyDataType = htole32(1);  // REG_SZ
        prop.prop.wPropertyNameLength = htole16(sizeof(prop.name));
        const char* name = "DeviceInterfaceGUID";
        for (size_t i = 0; i < 20; i++) prop.name[i] = htole16(uint16_t(i < strlen(name) ? name[i] : 0));
        prop.dataLength = htole32(sizeof(prop.data));
        const char* guid = "{F72FE0D4-CBCB-407D-8814-9ED673D0DD6B}";
        for (size_t i = 0; i < 39; i++) prop.data[i] = htole16(uint16_t(i < strlen(guid) ? guid[i] : 0));
        // With both Microsoft descriptors, then only the compatible ID, then none (older kernels refuse them).
        bool ok = false;
        for (int os = 2; os >= 0 && !ok; os--) {
            std::string d;
            DescHead h{};
            h.head.magic = htole32(FUNCTIONFS_DESCRIPTORS_MAGIC_V2);
            h.head.flags = htole32(FUNCTIONFS_HAS_FS_DESC | FUNCTIONFS_HAS_HS_DESC | FUNCTIONFS_HAS_SS_DESC |
                                   (os ? FUNCTIONFS_HAS_MS_OS_DESC : 0));
            h.fsCount = htole32(3);
            h.hsCount = htole32(3);
            h.ssCount = htole32(5);
            d.append((const char*)&h, sizeof(h));
            if (os) {
                uint32_t n = htole32(uint32_t(os));
                d.append((const char*)&n, 4);
            }
            d.append((const char*)&body, sizeof(body));
            if (os >= 1) d.append((const char*)&compat, sizeof(compat));
            if (os >= 2) d.append((const char*)&prop, sizeof(prop));
            uint32_t len = htole32(uint32_t(d.size()));
            memcpy(&d[offsetof(usb_functionfs_descs_head_v2, length)], &len, 4);
            ok = ::write(ep0_, d.data(), d.size()) == ssize_t(d.size());
        }
        if (!ok) {
            err = std::string("writing USB descriptors failed: ") + strerror(errno);
            return false;
        }
        Strings s{};
        s.head.magic = htole32(FUNCTIONFS_STRINGS_MAGIC);
        s.head.length = htole32(sizeof(s));
        s.head.str_count = htole32(1);
        s.head.lang_count = htole32(1);
        s.lang = htole16(0x0409);
        memcpy(s.str, "ADB Interface", sizeof(s.str));
        if (::write(ep0_, &s, sizeof(s)) != ssize_t(sizeof(s))) {
            err = std::string("writing USB strings failed: ") + strerror(errno);
            return false;
        }
        return true;
    }
    void monitor() {  // control requests nobody handles must be stalled, or hosts hang
        while (!stop_) {
            pollfd p{ep0_, POLLIN, 0};
            if (poll(&p, 1, 200) <= 0) continue;
            usb_functionfs_event ev{};
            if (::read(ep0_, &ev, sizeof(ev)) != ssize_t(sizeof(ev))) continue;
            if (ev.type == FUNCTIONFS_ENABLE) LOGI("adbd: USB host connected");
            if (ev.type == FUNCTIONFS_DISABLE) LOGI("adbd: USB host disconnected");
            if (ev.type == FUNCTIONFS_SETUP) {
                if (ev.u.setup.bRequestType & USB_DIR_IN) {
                    if (::write(ep0_, nullptr, 0) < 0) {}
                } else if (::read(ep0_, nullptr, 0) < 0) {
                }
            }
        }
    }
    void manualSetup() {  // ramdisks whose init doesn't set the adb function up
        if (!util::isDir(kGadget)) {
            util::mkdirs("/config");
            mount("none", "/config", "configfs", 0, nullptr);
        }
        if (!util::isDir(kGadget)) return;
        std::string fn = std::string(kGadget) + "/functions/ffs.adb";
        if (!util::isDir(fn)) mkdir(fn.c_str(), 0770);
        util::mkdirs(kFfs);
        if (mount("adb", kFfs, "functionfs", 0, "uid=0,gid=0") != 0) LOGW("adbd: mount functionfs: %s", strerror(errno));
    }
    void bindManually() {
        std::string g = kGadget;
        if (!util::isDir(g + "/functions/ffs.adb") || !util::isDir(g + "/configs/b.1")) return;
        LOGI("adbd: binding the USB gadget ourselves");
        util::writeFile(g + "/UDC", "\n");
        util::writeFile(g + "/idVendor", "0x18D1");
        util::writeFile(g + "/idProduct", "0xD001");
        util::mkdirs(g + "/configs/b.1/strings/0x409");
        util::writeFile(g + "/configs/b.1/strings/0x409/configuration", "adb");
        for (auto& e : util::listDir(g + "/configs/b.1"))
            if (e.link) unlink((g + "/configs/b.1/" + e.name).c_str());
        if (symlink((g + "/functions/ffs.adb").c_str(), (g + "/configs/b.1/f1").c_str()) != 0) LOGW("adbd: symlink: %s", strerror(errno));
        std::string udc = device::prop("sys.usb.controller");
        if (udc.empty())
            for (auto& e : util::listDir("/sys/class/udc")) udc = e.name;
        if (!udc.empty() && util::writeFile(g + "/UDC", udc)) manualBound_ = true;
    }
    void restore() {
        device::setProp("sys.usb.ffs.ready", "0");
        device::setProp("sys.usb.config", prevConfig_.empty() || prevConfig_ == "none" ? "adb" : prevConfig_);  // adbd again
    }

    int ep0_ = -1, out_ = -1, in_ = -1;
    std::thread monitor_;
    std::atomic<bool> stop_{false};
    std::string prevConfig_;
    bool manualBound_ = false;
};

struct Packet {
    uint32_t cmd = 0, a0 = 0, a1 = 0;
    std::string data;
};

uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
void put32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = uint8_t(v >> (8 * i));
}

// Where the package goes: the first place with room for it (and a margin).
std::string pickDir(uint64_t size, std::string& why) {
    auto& P = Partitions::get();
    struct Cand {
        std::string dir;
        uint64_t margin;
    };
    std::vector<Cand> cands;
    Partition* data = P.find("/data");
    if (data && P.isMounted(*data) && !P.dataEncrypted()) cands.push_back({P.appDir() + "/sideload", 64ull << 20});
    Partition* cache = P.find("/cache");
    if (cache && P.isMounted(*cache)) cands.push_back({cache->contentPath() + "/sideload", 16ull << 20});
    cands.push_back({device::path("/tmp/sideload"), 256ull << 20});  // RAM: leave plenty for the install
    uint64_t best = 0;
    for (auto& c : cands) {
        util::mkdirs(c.dir);
        unlink((c.dir + "/package.zip").c_str());
        struct statvfs vf;
        if (statvfs(c.dir.c_str(), &vf) != 0) continue;
        uint64_t freeB = uint64_t(vf.f_bavail) * vf.f_frsize;
        best = std::max(best, freeB);
        if (freeB >= size + c.margin) return c.dir;
    }
    why = "no room for a " + util::humanSize(size) + " package (at most " + util::humanSize(best) +
          " free) - copy it to the phone over MTP and install it from storage";
    return "";
}

}  // namespace

struct Sideload::Impl {
    std::unique_ptr<Link> link;
    uint32_t maxData = 4096;
    uint32_t localId = 0x464d52, remoteId = 0;  // "FMR"
    bool streamOpen = false, finished = false;
    std::string pathOut;
    std::thread watch;
    std::atomic<bool> stopWatch{false};

    bool send(uint32_t cmd, uint32_t a0, uint32_t a1, const std::string& data = "") {
        uint8_t h[24];
        uint32_t sum = 0;
        for (unsigned char c : data) sum += c;
        put32(h, cmd);
        put32(h + 4, a0);
        put32(h + 8, a1);
        put32(h + 12, uint32_t(data.size()));
        put32(h + 16, sum);
        put32(h + 20, cmd ^ 0xffffffffu);
        if (!link->separatePayload()) {
            std::string all((const char*)h, 24);
            return link->write((all + data).data(), 24 + data.size());
        }
        return link->write(h, 24) && (data.empty() || link->write(data.data(), data.size()));
    }
    bool recv(Packet& p) {
        uint8_t h[24];
        if (!link->readFull(h, 24)) return false;
        p.cmd = le32(h);
        p.a0 = le32(h + 4);
        p.a1 = le32(h + 8);
        uint32_t len = le32(h + 12);
        if ((p.cmd ^ 0xffffffffu) != le32(h + 20) || len > kMaxPayload) {
            LOGW("adbd: damaged message from the computer");
            return false;
        }
        p.data.resize(len);
        return !len || link->readFull(&p.data[0], len);
    }
    void stopWatcher() {
        stopWatch = true;
        if (watch.joinable()) watch.join();
    }
    void close() {
        stopWatcher();
        link.reset();  // USB: gives the gadget back to adbd
    }
};

Sideload::Sideload() : d_(new Impl) { reboot = "-"; }

Sideload::~Sideload() {
    if (d_->link && !d_->finished) finish(false);
    d_->close();
}

std::string Sideload::receive(const Options& o, Task& t) {
    Impl& d = *d_;
    // ---- wait for the computer
    int lfd = -1;
    if (o.tcpPort) {
        lfd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        int one = 1;
        setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(uint16_t(o.tcpPort));
        inet_pton(AF_INET, o.tcpBind.c_str(), &a.sin_addr);
        if (lfd < 0 || bind(lfd, (sockaddr*)&a, sizeof a) != 0 || listen(lfd, 2) != 0) {
            t.print("adb: can't listen on %s:%d: %s", o.tcpBind.c_str(), o.tcpPort, strerror(errno));
            if (lfd >= 0) ::close(lfd);
            return "";
        }
        t.print("Waiting for adb on %s:%d (adb connect %s:%d, then adb sideload <file>)", o.tcpBind.c_str(), o.tcpPort,
                o.tcpBind.c_str(), o.tcpPort);
    } else {
        auto u = std::make_unique<UsbLink>();
        std::string err;
        if (!u->open(err)) {
            t.print("adb sideload over USB: %s", err.c_str());
            return "";
        }
        d.link = std::move(u);
    }
    // Cancel: unblock whatever the session is waiting for.
    d.stopWatch = false;
    d.watch = std::thread([&d, &t] {
        while (!d.stopWatch) {
            if (t.cancelled() && d.link) {
                d.link->interrupt();
                return;
            }
            usleep(200 * 1000);
        }
    });
    struct ListenCloser {
        int fd;
        ~ListenCloser() {
            if (fd >= 0) ::close(fd);
        }
    } lc{lfd};
    int64_t deadline = util::nowMs() + 10 * 60 * 1000;
    t.setStatus("Waiting for the computer");
    for (;;) {
        if (t.cancelled() || util::nowMs() > deadline) {
            if (!t.cancelled()) t.print("No computer started a sideload in 10 minutes");
            d.close();
            return "";
        }
        if (!d.link) {  // TCP: the next connection
            pollfd p{lfd, POLLIN, 0};
            if (poll(&p, 1, 250) <= 0) continue;
            int c = accept4(lfd, nullptr, nullptr, SOCK_CLOEXEC);
            if (c < 0) continue;
            int one = 1;
            setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
            d.link = std::make_unique<TcpLink>(c);
        }
        Packet p;
        if (!d.recv(p)) {
            if (o.tcpPort) {  // the adb server went away; it reconnects
                d.stopWatcher();
                d.link.reset();
                d.stopWatch = false;
                d.watch = std::thread([&d, &t] {
                    while (!d.stopWatch) {
                        if (t.cancelled() && d.link) return d.link->interrupt();
                        usleep(200 * 1000);
                    }
                });
                continue;
            }
            if (!t.cancelled()) t.print("adb: the USB connection ended");
            d.close();
            return "";
        }
        if (p.cmd == A_CNXN) {  // the handshake: no authentication in recovery, like minadbd
            d.maxData = std::min<uint32_t>(std::max<uint32_t>(p.a1, 4096), kMaxPayload);
            const auto& di = device::info();
            std::string name = device::prop("ro.product.name");
            std::string banner = "sideload::ro.product.name=" + (name.empty() ? di.device : name) + ";ro.product.model=" + di.model +
                                 ";ro.product.device=" + di.device + ";features=";
            d.send(A_CNXN, std::min(p.a0, kVersion), kMaxPayload, banner);
            t.print("Computer connected - waiting for adb sideload");
            continue;
        }
        if (p.cmd != A_OPEN) continue;
        std::string svc = p.data.substr(0, p.data.find('\0'));
        d.remoteId = p.a0;
        uint64_t size = 0, block = 0;
        bool host = util::startsWith(svc, "sideload-host:"), old = !host && util::startsWith(svc, "sideload:");
        if (util::startsWith(svc, "reboot:")) {  // adb reboot [recovery|bootloader|...]
            d.send(A_OKAY, d.localId, d.remoteId);
            d.send(A_CLSE, d.localId, d.remoteId);
            reboot = svc.substr(7);
            t.print("The computer asked to reboot%s", reboot.empty() ? "" : (" to " + reboot).c_str());
            d.finished = true;
            d.close();
            return "";
        }
        if (host || old) {
            auto f = util::split(svc.substr(svc.find(':') + 1), ':');
            size = f.empty() ? 0 : strtoull(f[0].c_str(), nullptr, 10);
            block = host && f.size() > 1 ? strtoull(f[1].c_str(), nullptr, 10) : 0;
        }
        if ((!host && !old) || size == 0 || size > kMaxPackage || (host && (block == 0 || block > kMaxPayload))) {
            d.send(A_CLSE, 0, d.remoteId);  // not a service of ours
            continue;
        }
        std::string why, dir = pickDir(size, why);
        if (dir.empty()) {
            t.print("adb sideload: %s", why.c_str());
            d.send(A_CLSE, 0, d.remoteId);
            continue;
        }
        std::string path = dir + "/package.zip";
        int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (fd < 0) {
            t.print("adb sideload: can't write %s: %s", path.c_str(), strerror(errno));
            d.send(A_CLSE, 0, d.remoteId);
            continue;
        }
        d.send(A_OKAY, d.localId, d.remoteId);
        d.streamOpen = true;
        t.print("Receiving %s into %s", util::humanSize(size).c_str(), dir.c_str());
        t.setStatus("Receiving the package");
        uint64_t got = 0;
        bool ok = true;
        char det[64];
        auto progress = [&]() {
            t.setSubProgress(float(double(got) / double(size)));
            snprintf(det, sizeof det, "%s / %s", util::humanSize(got).c_str(), util::humanSize(size).c_str());
            t.setDetail(det);
        };
        // Data for our stream: written where it belongs, acknowledged so the computer sends more.
        auto take = [&](const Packet& q, uint64_t at, uint64_t limit) {
            if (q.data.size() > limit - at) return false;
            size_t done = 0;
            while (done < q.data.size()) {
                ssize_t w = pwrite(fd, q.data.data() + done, q.data.size() - done, off_t(at + done));
                if (w <= 0) return false;
                done += size_t(w);
            }
            return d.send(A_OKAY, d.localId, d.remoteId);
        };
        if (host) {  // ask for each block; the computer sends it (in one or more WRTEs)
            uint64_t blocks = (size + block - 1) / block;
            for (uint64_t b = 0; b < blocks && ok; b++) {
                char req[32];
                snprintf(req, sizeof req, "%08" PRIu64, b);
                if (!d.send(A_WRTE, d.localId, d.remoteId, std::string(req, 8))) { ok = false; break; }
                uint64_t start = b * block, need = std::min(block, size - start), have = 0;
                bool acked = false;
                while (ok && (have < need || !acked)) {
                    Packet q;
                    if (!d.recv(q)) { ok = false; break; }
                    if (q.cmd == A_OKAY && q.a1 == d.localId) acked = true;
                    else if (q.cmd == A_WRTE && q.a1 == d.localId) {
                        ok = take(q, start + have, start + need);
                        have += q.data.size();
                    } else if (q.cmd == A_CLSE || q.cmd == A_CNXN) {
                        ok = false;
                    }
                }
                got = start + have;
                progress();
            }
        } else {  // the old protocol: the whole file, pushed
            while (ok && got < size) {
                Packet q;
                if (!d.recv(q)) { ok = false; break; }
                if (q.cmd == A_WRTE && q.a1 == d.localId) {
                    ok = take(q, got, size);
                    got += q.data.size();
                    progress();
                } else if (q.cmd == A_CLSE || q.cmd == A_CNXN) {
                    ok = false;
                }
            }
        }
        if (fsync(fd) != 0) ok = false;
        ::close(fd);
        if (!ok || got != size) {
            unlink(path.c_str());
            if (!t.cancelled()) t.print("adb sideload: the transfer stopped after %s of %s", util::humanSize(got).c_str(), util::humanSize(size).c_str());
            d.close();
            return "";
        }
        t.print("Received %s", util::humanSize(size).c_str());
        if (old) {  // nothing to report later with the old protocol: the transfer was all
            d.send(A_CLSE, d.localId, d.remoteId);
            d.finished = true;
            d.close();
        }
        d.pathOut = path;
        return path;
    }
}

void Sideload::finish(bool installed) {
    Impl& d = *d_;
    if (d.finished || !d.link || !d.streamOpen) {
        d.close();
        return;
    }
    d.finished = true;
    // adb sideload exits 0 on DONEDONE, 1 on FAILFAIL (adb_sideload_install). A computer that went away meanwhile
    // can't make this wait for ever: the link is cut after 5 seconds.
    d.stopWatcher();
    d.stopWatch = false;
    d.watch = std::thread([&d] {
        for (int i = 0; i < 50 && !d.stopWatch; i++) usleep(100 * 1000);
        if (!d.stopWatch && d.link) d.link->interrupt();
    });
    if (d.send(A_WRTE, d.localId, d.remoteId, installed ? "DONEDONE" : "FAILFAIL")) {
        for (int i = 0; i < 4; i++) {  // its acknowledgement, then close
            Packet q;
            if (!d.recv(q) || (q.cmd == A_OKAY && q.a1 == d.localId) || q.cmd == A_CLSE) break;
        }
        d.send(A_CLSE, d.localId, d.remoteId);
    }
    d.close();
}

}  // namespace tw::adbd
