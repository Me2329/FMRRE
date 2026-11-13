#pragma once
// The link layer of FMRRE's own adb: the message framing, a TCP link (the simulator and the tests) and the USB
// link - the adb function of the recovery's gadget through FunctionFS. Shared by the sideload receiver (adbd.cpp)
// and the adb server (adbserver.cpp: shell, file transfer).
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <endian.h>
#include <fcntl.h>
#include <linux/usb/functionfs.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <string>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

#include "core/log.h"
#include "core/util.h"
#include "platform/device.h"

namespace tw::adbd::detail {

constexpr uint32_t A_CNXN = 0x4e584e43, A_OPEN = 0x4e45504f, A_OKAY = 0x59414b4f, A_CLSE = 0x45534c43, A_WRTE = 0x45545257;
constexpr uint32_t kVersion = 0x01000001;  // A_VERSION_SKIP_CHECKSUM
constexpr uint32_t kMaxPayload = 1024 * 1024;

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
constexpr const char* kLegacyGadget = "/sys/class/android_usb/android0";

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

inline usb_interface_descriptor intfDesc() {
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
inline usb_endpoint_descriptor_no_audio epDesc(uint8_t addr, uint16_t maxPacket) {
    usb_endpoint_descriptor_no_audio e{};
    e.bLength = USB_DT_ENDPOINT_SIZE;
    e.bDescriptorType = USB_DT_ENDPOINT;
    e.bEndpointAddress = addr;
    e.bmAttributes = USB_ENDPOINT_XFER_BULK;
    e.wMaxPacketSize = htole16(maxPacket);
    return e;
}
inline usb_ss_ep_comp_descriptor ssComp() {
    usb_ss_ep_comp_descriptor c{};
    c.bLength = USB_DT_SS_EP_COMP_SIZE;
    c.bDescriptorType = USB_DT_SS_ENDPOINT_COMP;
    c.bMaxBurst = 4;
    return c;
}

inline bool waitProp(const std::string& key, const std::string& want, int ms) {
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
        if (ep0_ < 0 && openAndroidAdb()) return true;
        if (ep0_ < 0) {
            err = ep0 + ": " + strerror(errno) + " (no adb FunctionFS in this ramdisk, and no /dev/android_adb)";
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
        if (!waitProp("sys.usb.state", "sideload", 1500) || udc.empty() || udc == "none") {
            bindManually();
            bindLegacy();
        }
        udc = util::readLine(std::string(kGadget) + "/UDC");
        LOGI("adbd: USB ready (%s)", legacyBound_ ? "android_usb gadget" : ("UDC " + udc).c_str());
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
        if (legacyBound_) util::writeFile(std::string(kLegacyGadget) + "/enable", "0");
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
    // Kernels with the old f_adb gadget driver (no FunctionFS for adb): the adb messages go through one character
    // device, a USB transfer per read and write - what adbd of the Android 4-8 era used.
    bool openAndroidAdb() {
        const char* dev = "/dev/android_adb";
        if (!util::exists(dev) || !util::isDir(kLegacyGadget)) return false;
        bindLegacy();
        int fd = -1;
        for (int i = 0; i < 20 && fd < 0; i++) {
            fd = ::open(dev, O_RDWR | O_CLOEXEC);
            if (fd < 0) usleep(100 * 1000);
        }
        if (fd < 0) {
            LOGW("adbd: %s: %s", dev, strerror(errno));
            return false;
        }
        out_ = fd;
        in_ = dup(fd);
        LOGI("adbd: USB ready (android_usb gadget, /dev/android_adb)");
        return true;
    }
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
    // Kernels from before configfs gadgets (3.x / 4.4: the Galaxy S8 and its time) have the "android_usb" gadget:
    // the adb function is chosen and the gadget switched on through sysfs - as AOSP's recovery of that time did.
    void bindLegacy() {
        const std::string a = kLegacyGadget;
        if (!util::isDir(a)) return;
        LOGI("adbd: switching the android_usb gadget to adb");
        util::writeFile(a + "/enable", "0");
        util::writeFile(a + "/f_ffs/aliases", "adb");
        std::string vid = util::trim(util::readLine(a + "/idVendor"));
        if (vid.empty() || vid == "0000") {
            util::writeFile(a + "/idVendor", "18D1");
            util::writeFile(a + "/idProduct", "D001");
        }
        if (!util::writeFile(a + "/functions", "adb")) util::writeFile(a + "/functions", "ffs");
        legacyBound_ = util::writeFile(a + "/enable", "1");
    }
    void restore() {
        device::setProp("sys.usb.ffs.ready", "0");
        device::setProp("sys.usb.config", prevConfig_.empty() || prevConfig_ == "none" ? "adb" : prevConfig_);  // adbd again
    }

    int ep0_ = -1, out_ = -1, in_ = -1;
    std::thread monitor_;
    std::atomic<bool> stop_{false};
    std::string prevConfig_;
    bool manualBound_ = false, legacyBound_ = false;
};

struct Packet {
    uint32_t cmd = 0, a0 = 0, a1 = 0;
    std::string data;
};

inline uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
inline void put32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = uint8_t(v >> (8 * i));
}

// One adb message: 24-byte header, then the payload (on USB as a transfer of its own).
inline bool sendPacket(Link& link, uint32_t cmd, uint32_t a0, uint32_t a1, const std::string& data = "") {
    uint8_t h[24];
    uint32_t sum = 0;
    for (unsigned char c : data) sum += c;
    put32(h, cmd);
    put32(h + 4, a0);
    put32(h + 8, a1);
    put32(h + 12, uint32_t(data.size()));
    put32(h + 16, sum);
    put32(h + 20, cmd ^ 0xffffffffu);
    if (!link.separatePayload()) {
        std::string all((const char*)h, 24);
        all += data;
        return link.write(all.data(), all.size());
    }
    return link.write(h, 24) && (data.empty() || link.write(data.data(), data.size()));
}
inline bool recvPacket(Link& link, Packet& p) {
    uint8_t h[24];
    if (!link.readFull(h, 24)) return false;
    p.cmd = le32(h);
    p.a0 = le32(h + 4);
    p.a1 = le32(h + 8);
    uint32_t len = le32(h + 12);
    if ((p.cmd ^ 0xffffffffu) != le32(h + 20) || len > kMaxPayload) {
        LOGW("adbd: damaged message from the computer");
        return false;
    }
    p.data.resize(len);
    return !len || link.readFull(&p.data[0], len);
}

}  // namespace tw::adbd::detail
