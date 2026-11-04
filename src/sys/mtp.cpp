// MTP responder (PTP + the MTP 1.1 object properties Windows Explorer relies on). One session, one
// transaction at a time, every number little-endian on the wire whatever the CPU.
#include "sys/mtp.h"

#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <endian.h>
#include <fcntl.h>
#include <functional>
#include <linux/usb/functionfs.h>
#include <map>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "core/log.h"
#include "core/task.h"
#include "core/util.h"
#include "platform/device.h"
#include "sys/partitions.h"

namespace tw::mtp {

namespace {

// ---- protocol constants ------------------------------------------------------------------------------

enum : uint16_t { kCommand = 1, kData = 2, kResponse = 3 };
enum : uint16_t {
    OpGetDeviceInfo = 0x1001, OpOpenSession = 0x1002, OpCloseSession = 0x1003, OpGetStorageIDs = 0x1004,
    OpGetStorageInfo = 0x1005, OpGetNumObjects = 0x1006, OpGetObjectHandles = 0x1007, OpGetObjectInfo = 0x1008,
    OpGetObject = 0x1009, OpDeleteObject = 0x100B, OpSendObjectInfo = 0x100C, OpSendObject = 0x100D,
    OpGetDevicePropDesc = 0x1014, OpGetDevicePropValue = 0x1015, OpSetDevicePropValue = 0x1016, OpMoveObject = 0x1019,
    OpCopyObject = 0x101A, OpGetPartialObject = 0x101B, OpGetObjectPropsSupported = 0x9801, OpGetObjectPropDesc = 0x9802,
    OpGetObjectPropValue = 0x9803, OpSetObjectPropValue = 0x9804, OpGetObjectPropList = 0x9805,
    OpGetObjectReferences = 0x9810, OpSetObjectReferences = 0x9811, OpGetPartialObject64 = 0x95C1,
};
enum : uint16_t {
    RcOK = 0x2001, RcGeneralError = 0x2002, RcSessionNotOpen = 0x2003, RcOperationNotSupported = 0x2005,
    RcInvalidStorageID = 0x2008, RcInvalidObjectHandle = 0x2009, RcDevicePropNotSupported = 0x200A, RcStoreFull = 0x200C,
    RcAccessDenied = 0x200F, RcNoValidObjectInfo = 0x2015, RcInvalidParentObject = 0x201A, RcInvalidParameter = 0x201D,
    RcSessionAlreadyOpen = 0x201E, RcInvalidObjectPropCode = 0xA801, RcInvalidDataset = 0xA806,
    RcSpecByGroupUnsupported = 0xA807, RcSpecByDepthUnsupported = 0xA808,
};
enum : uint16_t { FmtUndefined = 0x3000, FmtFolder = 0x3001, FmtText = 0x3004, FmtMp3 = 0x3009, FmtJpeg = 0x3801, FmtPng = 0x380B, FmtMp4 = 0xB982 };
enum : uint16_t {
    PropStorageID = 0xDC01, PropObjectFormat = 0xDC02, PropProtection = 0xDC03, PropObjectSize = 0xDC04,
    PropFileName = 0xDC07, PropDateModified = 0xDC09, PropParent = 0xDC0B, PropUid = 0xDC41, PropName = 0xDC44,
};
enum : uint16_t { TU8 = 0x0002, TU16 = 0x0004, TU32 = 0x0006, TU64 = 0x0008, TU128 = 0x000A, TStr = 0xFFFF };
enum : uint16_t { DpBattery = 0x5001, DpSyncPartner = 0xD401, DpFriendlyName = 0xD402 };
constexpr uint32_t kAll = 0xFFFFFFFF;
constexpr size_t kChunk = 1 << 20;  // a multiple of every USB packet size: only the last write of a transfer is short

const uint16_t kOps[] = {OpGetDeviceInfo, OpOpenSession, OpCloseSession, OpGetStorageIDs, OpGetStorageInfo, OpGetNumObjects,
                         OpGetObjectHandles, OpGetObjectInfo, OpGetObject, OpDeleteObject, OpSendObjectInfo, OpSendObject,
                         OpGetDevicePropDesc, OpGetDevicePropValue, OpSetDevicePropValue, OpMoveObject, OpCopyObject,
                         OpGetPartialObject, OpGetObjectPropsSupported, OpGetObjectPropDesc, OpGetObjectPropValue,
                         OpSetObjectPropValue, OpGetObjectPropList, OpGetObjectReferences, OpSetObjectReferences,
                         OpGetPartialObject64};
const uint16_t kProps[] = {PropStorageID, PropObjectFormat, PropProtection, PropObjectSize, PropFileName,
                           PropDateModified, PropParent, PropUid, PropName};
const uint16_t kFormats[] = {FmtUndefined, FmtFolder, FmtText, FmtMp3, FmtJpeg, FmtPng, FmtMp4};
const uint16_t kDevProps[] = {DpBattery, DpSyncPartner, DpFriendlyName};

// ---- strings: MTP uses a length byte and UTF-16LE with a terminating NUL --------------------------------

std::u16string toUtf16(const std::string& s) {
    std::u16string out;
    for (size_t i = 0; i < s.size();) {
        uint32_t c = (uint8_t)s[i];
        size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (!len || i + len > s.size()) { out.push_back(0xFFFD); i++; continue; }
        if (len > 1) c &= 0x3F >> (len - 1);
        bool ok = true;
        for (size_t k = 1; k < len; k++) {
            uint8_t b = (uint8_t)s[i + k];
            ok &= (b & 0xC0) == 0x80;
            c = (c << 6) | (b & 0x3F);
        }
        i += ok ? len : 1;
        if (!ok) { out.push_back(0xFFFD); continue; }
        if (c >= 0x10000) {
            c -= 0x10000;
            out.push_back(char16_t(0xD800 + (c >> 10)));
            out.push_back(char16_t(0xDC00 + (c & 0x3FF)));
        } else {
            out.push_back(char16_t(c));
        }
    }
    return out;
}

std::string toUtf8(const std::u16string& u) {
    std::string out;
    for (size_t i = 0; i < u.size(); i++) {
        uint32_t c = u[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < u.size() && u[i + 1] >= 0xDC00 && u[i + 1] < 0xE000)
            c = 0x10000 + ((c - 0xD800) << 10) + (u[++i] - 0xDC00);
        if (c < 0x80) out.push_back(char(c));
        else if (c < 0x800) { out.push_back(char(0xC0 | c >> 6)); out.push_back(char(0x80 | (c & 0x3F))); }
        else if (c < 0x10000) { out.push_back(char(0xE0 | c >> 12)); out.push_back(char(0x80 | ((c >> 6) & 0x3F))); out.push_back(char(0x80 | (c & 0x3F))); }
        else {
            out.push_back(char(0xF0 | c >> 18));
            out.push_back(char(0x80 | ((c >> 12) & 0x3F)));
            out.push_back(char(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(char(0x80 | (c & 0x3F)));
        }
    }
    return out;
}

struct Out {
    std::string b;
    void u8(uint8_t v) { b.push_back(char(v)); }
    void u16(uint16_t v) { u8(uint8_t(v)); u8(uint8_t(v >> 8)); }
    void u32(uint32_t v) { u16(uint16_t(v)); u16(uint16_t(v >> 16)); }
    void u64(uint64_t v) { u32(uint32_t(v)); u32(uint32_t(v >> 32)); }
    void str(const std::string& s) {
        std::u16string u = toUtf16(s);
        if (u.size() > 254) u.resize(254);
        if (u.empty()) { u8(0); return; }
        u8(uint8_t(u.size() + 1));
        for (char16_t c : u) u16(c);
        u16(0);
    }
    void a16(const uint16_t* v, size_t n) { u32(uint32_t(n)); for (size_t i = 0; i < n; i++) u16(v[i]); }
    void a32(const std::vector<uint32_t>& v) { u32(uint32_t(v.size())); for (auto x : v) u32(x); }
};

struct In {
    const uint8_t* p;
    size_t n, i = 0;
    bool ok = true;
    bool need(size_t k) {
        if (n - i < k) { ok = false; i = n; return false; }
        return true;
    }
    uint8_t u8() { return need(1) ? p[i++] : 0; }
    uint16_t u16() {
        if (!need(2)) return 0;
        uint16_t v = uint16_t(p[i] | p[i + 1] << 8);
        i += 2;
        return v;
    }
    uint32_t u32() { uint32_t lo = u16(); return lo | uint32_t(u16()) << 16; }
    uint64_t u64() { uint64_t lo = u32(); return lo | uint64_t(u32()) << 32; }
    std::string str() {
        uint8_t cnt = u8();
        std::u16string u;
        for (int k = 0; k < cnt && ok; k++) {
            char16_t c = u16();
            if (c) u.push_back(c);
        }
        return toUtf8(u);
    }
};

std::string mtpDate(int64_t t) {
    time_t tt = (time_t)t;
    struct tm tm{};
    localtime_r(&tt, &tm);
    char b[32];
    snprintf(b, sizeof b, "%04d%02d%02dT%02d%02d%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    return b;
}

uint16_t formatOf(const std::string& name, bool dir) {
    if (dir) return FmtFolder;
    std::string l = util::toLower(name);
    if (util::endsWith(l, ".txt") || util::endsWith(l, ".log")) return FmtText;
    if (util::endsWith(l, ".mp3")) return FmtMp3;
    if (util::endsWith(l, ".jpg") || util::endsWith(l, ".jpeg")) return FmtJpeg;
    if (util::endsWith(l, ".png")) return FmtPng;
    if (util::endsWith(l, ".mp4")) return FmtMp4;
    return FmtUndefined;
}

// A file name from the computer: one path component, nothing that climbs out of the folder.
bool safeName(const std::string& n) {
    return !n.empty() && n != "." && n != ".." && n.find('/') == std::string::npos && n.find('\0') == std::string::npos && n.size() < 256;
}

// ---- global state ------------------------------------------------------------------------------------

std::mutex g_mu;
Status g_status;
std::atomic<bool> g_stop{false};
std::thread g_thread;

void setStatus(const std::function<void(Status&)>& f) {
    std::lock_guard<std::mutex> l(g_mu);
    f(g_status);
}

// ---- transports --------------------------------------------------------------------------------------

class Link {
public:
    virtual ~Link() = default;
    virtual ssize_t read(void* buf, size_t n) = 0;  // part of a transfer; < 0: gone (USB: 0 is a zero-length packet)
    virtual bool write(const void* buf, size_t n) = 0;
    virtual void endOfData(uint64_t total) { (void)total; }
    virtual bool usb() const { return false; }
    virtual void interrupt() {}
    virtual bool cancelled() { return false; }  // the computer sent a Cancel request (USB)
    virtual void clearCancel() {}
    virtual std::string peer() const = 0;
};

class TcpLink : public Link {
public:
    TcpLink(int fd, std::string peer) : fd_(fd), peer_(std::move(peer)) {}
    ~TcpLink() override { close(fd_); }
    std::string peer() const override { return peer_; }
    void interrupt() override { ::shutdown(fd_, SHUT_RDWR); }
    ssize_t read(void* buf, size_t n) override {
        for (;;) {
            pollfd p{fd_, POLLIN, 0};
            int r = poll(&p, 1, 250);
            if (g_stop) return -1;
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) continue;
            ssize_t k = recv(fd_, buf, n, 0);
            if (k < 0 && errno == EINTR) continue;
            return k > 0 ? k : -1;
        }
    }
    bool write(const void* buf, size_t n) override {
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

private:
    int fd_;
    std::string peer_;
};

// USB through FunctionFS: interface class 6/1/1 (PTP, with the MTP vendor extension), bulk out, bulk in and
// an interrupt endpoint for events, plus Microsoft OS descriptors so Windows loads its MTP driver.
constexpr const char* kFfs = "/dev/usb-ffs/mtp";
constexpr const char* kGadget = "/config/usb_gadget/g1";
constexpr const char* kLinkName = "fmrre_mtp";

struct FuncDesc {
    usb_interface_descriptor intf;
    usb_endpoint_descriptor_no_audio sink, source, intr;
} __attribute__((packed));
struct SsFuncDesc {
    usb_interface_descriptor intf;
    usb_endpoint_descriptor_no_audio sink;
    usb_ss_ep_comp_descriptor sinkComp;
    usb_endpoint_descriptor_no_audio source;
    usb_ss_ep_comp_descriptor sourceComp;
    usb_endpoint_descriptor_no_audio intr;
    usb_ss_ep_comp_descriptor intrComp;
} __attribute__((packed));
struct OsDesc {
    usb_os_desc_header header;
    usb_ext_compat_desc compat;
} __attribute__((packed));
struct DescV2Os {
    usb_functionfs_descs_head_v2 head;
    uint32_t fsCount, hsCount, ssCount, osCount;
    FuncDesc fs, hs;
    SsFuncDesc ss;
    OsDesc os;
} __attribute__((packed));
struct DescV2 {
    usb_functionfs_descs_head_v2 head;
    uint32_t fsCount, hsCount, ssCount;
    FuncDesc fs, hs;
    SsFuncDesc ss;
} __attribute__((packed));
struct Strings {
    usb_functionfs_strings_head head;
    uint16_t lang;
    char str[sizeof("MTP")];
} __attribute__((packed));

usb_endpoint_descriptor_no_audio ep(uint8_t addr, uint8_t type, uint16_t maxPacket, uint8_t interval) {
    usb_endpoint_descriptor_no_audio e{};
    e.bLength = USB_DT_ENDPOINT_SIZE;
    e.bDescriptorType = USB_DT_ENDPOINT;
    e.bEndpointAddress = addr;
    e.bmAttributes = type;
    e.wMaxPacketSize = htole16(maxPacket);
    e.bInterval = interval;
    return e;
}
usb_interface_descriptor intf() {
    usb_interface_descriptor d{};
    d.bLength = USB_DT_INTERFACE_SIZE;
    d.bDescriptorType = USB_DT_INTERFACE;
    d.bNumEndpoints = 3;
    d.bInterfaceClass = 6;  // still image (PTP); the MTP extension is announced in GetDeviceInfo
    d.bInterfaceSubClass = 1;
    d.bInterfaceProtocol = 1;
    d.iInterface = 1;
    return d;
}
usb_ss_ep_comp_descriptor comp(uint8_t burst, uint16_t perInterval) {
    usb_ss_ep_comp_descriptor c{};
    c.bLength = USB_DT_SS_EP_COMP_SIZE;
    c.bDescriptorType = USB_DT_SS_ENDPOINT_COMP;
    c.bMaxBurst = burst;
    c.wBytesPerInterval = htole16(perInterval);
    return c;
}
FuncDesc funcDesc(uint16_t mp) {
    return {intf(), ep(1 | USB_DIR_OUT, USB_ENDPOINT_XFER_BULK, mp, 0), ep(2 | USB_DIR_IN, USB_ENDPOINT_XFER_BULK, mp, 0),
            ep(3 | USB_DIR_IN, USB_ENDPOINT_XFER_INT, 28, 6)};
}

class UsbLink : public Link {
public:
    ~UsbLink() override { close(); }
    std::string peer() const override { return "USB"; }
    bool usb() const override { return true; }

    bool open(std::string& err) {
        std::string ep0 = std::string(kFfs) + "/ep0";
        if (!util::exists(ep0)) createFunction();
        ep0_ = ::open(ep0.c_str(), O_RDWR | O_CLOEXEC);
        if (ep0_ < 0) { err = ep0 + ": " + strerror(errno) + " (no USB gadget configfs for MTP in this ramdisk)"; close(); return false; }
        if (!descriptors(err)) { close(); return false; }
        out_ = ::open((std::string(kFfs) + "/ep1").c_str(), O_RDONLY | O_CLOEXEC);
        in_ = ::open((std::string(kFfs) + "/ep2").c_str(), O_WRONLY | O_CLOEXEC);
        intr_ = ::open((std::string(kFfs) + "/ep3").c_str(), O_WRONLY | O_CLOEXEC);
        if (out_ < 0 || in_ < 0) { err = std::string("MTP endpoints: ") + strerror(errno); close(); return false; }
        stopMon_ = false;
        monitor_ = std::thread([this] { monitor(); });
        if (!compose(err)) { close(); return false; }
        return true;
    }
    ssize_t read(void* buf, size_t n) override {
        for (;;) {
            ssize_t r = ::read(out_, buf, n);
            if (r < 0 && errno == EINTR) continue;
            return r;
        }
    }
    bool write(const void* buf, size_t n) override {
        const char* p = (const char*)buf;
        while (n) {
            ssize_t r = ::write(in_, p, n);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) return false;
            p += r;
            n -= (size_t)r;
        }
        return true;
    }
    // A transfer that is a whole number of packets needs a zero-length packet to end.
    void endOfData(uint64_t total) override {
        if (total % maxPacket_ == 0 && ::write(in_, nullptr, 0) < 0) LOGW("mtp: zero-length packet: %s", strerror(errno));
    }
    void interrupt() override {
        if (bound_) util::writeFile(std::string(kGadget) + "/UDC", "\n");  // pending reads fail: the server loop ends
    }
    bool cancelled() override { return cancel; }
    void clearCancel() override { cancel = false; }
    std::atomic<bool> cancel{false};

private:
    void createFunction() {
        if (!util::isDir(kGadget)) {
            util::mkdirs("/config");
            mount("none", "/config", "configfs", 0, nullptr);
        }
        std::string fn = std::string(kGadget) + "/functions/ffs.mtp";
        if (util::isDir(kGadget) && !util::isDir(fn) && mkdir(fn.c_str(), 0770) == 0) createdFn_ = true;
        util::mkdirs(kFfs);
        if (mount("mtp", kFfs, "functionfs", 0, "uid=0,gid=0") == 0) mounted_ = true;
        else LOGW("mtp: mount functionfs: %s", strerror(errno));
    }

    bool descriptors(std::string& err) {
        DescV2Os d{};
        d.head.magic = htole32(FUNCTIONFS_DESCRIPTORS_MAGIC_V2);
        d.head.flags = htole32(FUNCTIONFS_HAS_FS_DESC | FUNCTIONFS_HAS_HS_DESC | FUNCTIONFS_HAS_SS_DESC | FUNCTIONFS_HAS_MS_OS_DESC);
        d.head.length = htole32(sizeof(d));
        d.fsCount = htole32(4);
        d.hsCount = htole32(4);
        d.ssCount = htole32(7);
        d.osCount = htole32(1);
        d.fs = funcDesc(64);
        d.hs = funcDesc(512);
        d.ss = {intf(), ep(1 | USB_DIR_OUT, USB_ENDPOINT_XFER_BULK, 1024, 0), comp(6, 0), ep(2 | USB_DIR_IN, USB_ENDPOINT_XFER_BULK, 1024, 0),
                comp(6, 0), ep(3 | USB_DIR_IN, USB_ENDPOINT_XFER_INT, 28, 6), comp(0, 28)};
        d.os.header.interface = 1;
        d.os.header.dwLength = htole32(sizeof(OsDesc));
        d.os.header.bcdVersion = htole16(1);
        d.os.header.wIndex = htole16(4);
        d.os.header.bCount = 1;
        d.os.compat.bFirstInterfaceNumber = 0;
        d.os.compat.Reserved1 = 1;
        memcpy(d.os.compat.CompatibleID, "MTP\0\0\0\0\0", 8);
        bool ok = ::write(ep0_, &d, sizeof(d)) == (ssize_t)sizeof(d);
        if (!ok) {  // kernels without MS OS descriptor support
            DescV2 d2{};
            d2.head.magic = htole32(FUNCTIONFS_DESCRIPTORS_MAGIC_V2);
            d2.head.flags = htole32(FUNCTIONFS_HAS_FS_DESC | FUNCTIONFS_HAS_HS_DESC | FUNCTIONFS_HAS_SS_DESC);
            d2.head.length = htole32(sizeof(d2));
            d2.fsCount = htole32(4);
            d2.hsCount = htole32(4);
            d2.ssCount = htole32(7);
            d2.fs = d.fs;
            d2.hs = d.hs;
            d2.ss = d.ss;
            ok = ::write(ep0_, &d2, sizeof(d2)) == (ssize_t)sizeof(d2);
        }
        if (!ok) { err = std::string("writing the MTP USB descriptors failed: ") + strerror(errno); return false; }
        Strings s{};
        s.head.magic = htole32(FUNCTIONFS_STRINGS_MAGIC);
        s.head.length = htole32(sizeof(s));
        s.head.str_count = htole32(1);
        s.head.lang_count = htole32(1);
        s.lang = htole16(0x0409);
        memcpy(s.str, "MTP", sizeof(s.str));
        if (::write(ep0_, &s, sizeof(s)) != (ssize_t)sizeof(s)) { err = std::string("writing the MTP USB strings failed: ") + strerror(errno); return false; }
        return true;
    }

    // Adds MTP to the USB configuration next to what is there (adb keeps working), then re-binds the gadget.
    bool compose(std::string& err) {
        std::string g = kGadget, cfg;
        for (auto& e : util::listDir(g + "/configs"))
            if (e.dir) { cfg = g + "/configs/" + e.name; break; }
        if (cfg.empty()) { err = "no USB gadget configuration in /config/usb_gadget/g1"; return false; }
        udc_ = util::readLine(g + "/UDC");
        if (udc_.empty() || udc_ == "none") udc_ = device::prop("sys.usb.controller");
        if (udc_.empty())
            for (auto& e : util::listDir("/sys/class/udc")) { udc_ = e.name; break; }
        if (udc_.empty()) { err = "no USB device controller (/sys/class/udc is empty)"; return false; }
        util::writeFile(g + "/UDC", "\n");
        link_ = cfg + "/" + kLinkName;
        unlink(link_.c_str());
        if (symlink((g + "/functions/ffs.mtp").c_str(), link_.c_str()) != 0) {
            err = std::string("adding MTP to the USB configuration: ") + strerror(errno);
            util::writeFile(g + "/UDC", udc_);
            return false;
        }
        prevPid_ = util::readLine(g + "/idProduct");
        util::writeFile(g + "/idProduct", "0x4EE2");  // the usual product id for "MTP + adb"
        if (!util::writeFile(g + "/UDC", udc_)) {
            err = "binding the USB gadget failed";
            return false;
        }
        bound_ = true;
        std::string speed = util::readLine("/sys/class/udc/" + udc_ + "/current_speed");
        maxPacket_ = speed.find("super") != std::string::npos ? 1024 : speed.find("full") != std::string::npos ? 64 : 512;
        LOGI("mtp: USB gadget ready (UDC %s)", udc_.c_str());
        return true;
    }

    // Control requests: MTP's class requests (cancel, device status, reset); everything else is stalled.
    void monitor() {
        while (!stopMon_) {
            pollfd p{ep0_, POLLIN, 0};
            if (poll(&p, 1, 200) <= 0) continue;
            usb_functionfs_event ev{};
            if (::read(ep0_, &ev, sizeof(ev)) != (ssize_t)sizeof(ev)) continue;
            if (ev.type == FUNCTIONFS_ENABLE) {
                std::string speed = util::readLine("/sys/class/udc/" + udc_ + "/current_speed");
                maxPacket_ = speed.find("super") != std::string::npos ? 1024 : speed.find("full") != std::string::npos ? 64 : 512;
                setStatus([](Status& s) { s.connected = true; s.peer = "a computer (USB)"; });
                continue;
            }
            if (ev.type == FUNCTIONFS_DISABLE) {
                setStatus([](Status& s) { s.connected = false; s.peer.clear(); });
                continue;
            }
            if (ev.type != FUNCTIONFS_SETUP) continue;
            const usb_ctrlrequest& r = ev.u.setup;
            uint16_t len = le16toh(r.wLength);
            bool in = r.bRequestType & USB_DIR_IN;
            if ((r.bRequestType & USB_TYPE_MASK) == USB_TYPE_CLASS && r.bRequest == 0x67 && in) {  // Get Device Status
                uint8_t st[4] = {4, 0, uint8_t(RcOK & 0xFF), uint8_t(RcOK >> 8)};
                if (::write(ep0_, st, std::min<size_t>(len, 4)) < 0) {}
            } else if ((r.bRequestType & USB_TYPE_MASK) == USB_TYPE_CLASS && r.bRequest == 0x64 && !in) {  // Cancel Request
                std::vector<uint8_t> b(len);
                if (::read(ep0_, b.data(), len) >= 0) cancel = true;
            } else if ((r.bRequestType & USB_TYPE_MASK) == USB_TYPE_CLASS && r.bRequest == 0x66 && !in) {  // Device Reset
                if (::read(ep0_, nullptr, 0) < 0) {}
                cancel = true;
            } else if (in) {
                if (::read(ep0_, nullptr, 0) < 0) {}  // reading on an IN request stalls it
            } else {
                if (::write(ep0_, nullptr, 0) < 0) {}  // writing on an OUT request stalls it
            }
        }
    }

    void close() {
        std::string g = kGadget;
        if (bound_) util::writeFile(g + "/UDC", "\n");
        if (!link_.empty()) unlink(link_.c_str());
        if (!prevPid_.empty()) util::writeFile(g + "/idProduct", prevPid_);
        if (bound_ && !udc_.empty()) util::writeFile(g + "/UDC", udc_);  // adb comes back on its own
        bound_ = false;
        link_.clear();
        prevPid_.clear();
        stopMon_ = true;
        if (monitor_.joinable()) monitor_.join();
        for (int* fd : {&out_, &in_, &intr_, &ep0_})
            if (*fd >= 0) { ::close(*fd); *fd = -1; }
        if (mounted_) { umount(kFfs); mounted_ = false; }
        if (createdFn_) { rmdir((g + "/functions/ffs.mtp").c_str()); createdFn_ = false; }
    }

    int ep0_ = -1, out_ = -1, in_ = -1, intr_ = -1;
    std::thread monitor_;
    std::atomic<bool> stopMon_{false};
    std::string udc_, link_, prevPid_;
    bool bound_ = false, mounted_ = false, createdFn_ = false;
    size_t maxPacket_ = 512;
};

// ---- the responder -----------------------------------------------------------------------------------

struct Storage {
    uint32_t id;
    std::string root, desc;
    bool removable;
};
struct Obj {
    uint32_t storage = 0, parent = 0;  // parent 0: in the storage's root
    std::string path;
};

class Server {
public:
    explicit Server(Link& l) : l_(l), rbuf_(kChunk) {}

    void serve() {
        for (;;) {
            uint8_t h[12];
            if (!readExact(h, 12)) return;
            In in{h, 12};
            uint32_t len = in.u32();
            uint16_t type = in.u16(), code = in.u16();
            uint32_t tid = in.u32();
            if (len < 12) { LOGW("mtp: bad container (length %u)", len); return; }
            if (type != kCommand) {  // what is left of a cancelled transaction: skip it
                if (!skip(len)) return;
                continue;
            }
            if (len > 12 + 5 * 4) { LOGW("mtp: bad command (length %u)", len); return; }
            std::vector<uint8_t> rest(len - 12);
            if (!rest.empty() && !readExact(rest.data(), rest.size())) return;
            uint32_t p[5] = {};
            for (size_t k = 0; k < 5 && 4 * k + 4 <= rest.size(); k++)
                p[k] = uint32_t(rest[4 * k]) | uint32_t(rest[4 * k + 1]) << 8 | uint32_t(rest[4 * k + 2]) << 16 | uint32_t(rest[4 * k + 3]) << 24;
            tid_ = tid;
            op_ = code;
            if (!handle(code, p, (rest.size()) / 4)) return;
        }
    }

private:
    // ---- I/O
    bool fill() {
        while (roff_ >= rlen_) {
            ssize_t r = l_.read(rbuf_.data(), rbuf_.size());
            if (r < 0) return false;
            if (r == 0) {
                if (!l_.usb()) return false;
                lastShort_ = true;  // a zero-length packet: the end of a transfer
                continue;
            }
            roff_ = 0;
            rlen_ = size_t(r);
            lastShort_ = size_t(r) < rbuf_.size();
        }
        return true;
    }
    bool readExact(void* dst, size_t n) {
        uint8_t* d = (uint8_t*)dst;
        while (n) {
            if (!fill()) return false;
            size_t k = std::min(n, rlen_ - roff_);
            memcpy(d, rbuf_.data() + roff_, k);
            roff_ += k;
            d += k;
            n -= k;
            setStatus([k](Status& s) { s.bytesIn += k; });
        }
        return true;
    }
    bool skip(uint32_t len) {
        uint64_t left = len == 0xFFFFFFFFu ? UINT64_MAX : uint64_t(len) - 12;
        while (left) {
            if (len == 0xFFFFFFFFu && roff_ >= rlen_ && lastShort_) return true;
            if (!fill()) return false;
            size_t k = (size_t)std::min<uint64_t>(left, rlen_ - roff_);
            roff_ += k;
            left -= k;
        }
        return true;
    }
    // The data phase the computer sends after some commands (small datasets).
    bool recvData(std::string& out) {
        uint8_t h[12];
        if (!readExact(h, 12)) return false;
        In in{h, 12};
        uint32_t len = in.u32();
        uint16_t type = in.u16();
        if (type != kData || len < 12 || len > (16u << 20)) return false;
        out.resize(len - 12);
        return out.empty() || readExact(&out[0], out.size());
    }
    bool sendData(const std::string& payload) {
        Out h;
        h.u32(uint32_t(12 + payload.size()));
        h.u16(kData);
        h.u16(op_);
        h.u32(tid_);
        std::string all = h.b + payload;
        if (!l_.write(all.data(), all.size())) return false;
        l_.endOfData(all.size());
        setStatus([&](Status& s) { s.bytesOut += all.size(); });
        return true;
    }
    bool respond(uint16_t rc, std::initializer_list<uint32_t> params = {}) {
        Out o;
        o.u32(uint32_t(12 + 4 * params.size()));
        o.u16(kResponse);
        o.u16(rc);
        o.u32(tid_);
        for (uint32_t v : params) o.u32(v);
        return l_.write(o.b.data(), o.b.size());
    }

    // ---- objects
    void scanStorages() {
        storages_.clear();
        Partitions& parts = Partitions::get();
        uint32_t n = 0;
        for (Partition* p : parts.storages()) {
            n++;  // ids stay the same when an SD card comes or goes
            std::string root;
            if (p->mountPoint == "/data") {
                std::string media = device::path("/data/media/0");
                if (!parts.isMounted(*p) || parts.dataEncrypted() || !util::isDir(media)) continue;
                root = media;
            } else {
                if (!parts.isMounted(*p) && !parts.mount(*p)) continue;
                root = p->contentPath();
            }
            if (!util::isDir(root)) continue;
            storages_.push_back({(n << 16) | 1, root, p->mountPoint == "/data" ? "Internal storage" : p->displayName, p->removable});
        }
    }
    const Storage* storage(uint32_t id) {
        for (auto& s : storages_)
            if (s.id == id) return &s;
        return nullptr;
    }
    uint32_t handleFor(uint32_t storage, uint32_t parent, const std::string& path) {
        auto it = byPath_.find(path);
        if (it != byPath_.end()) {
            objs_[it->second].parent = parent;
            return it->second;
        }
        uint32_t h = next_++;
        objs_[h] = {storage, parent, path};
        byPath_[path] = h;
        return h;
    }
    Obj* obj(uint32_t h) {
        auto it = objs_.find(h);
        if (it == objs_.end()) return nullptr;
        struct stat st;
        if (lstat(it->second.path.c_str(), &st) != 0) {  // gone from the disk
            forget(it->second.path);
            return nullptr;
        }
        return &it->second;
    }
    void forget(const std::string& path) {  // the object and everything below it
        for (auto it = byPath_.begin(); it != byPath_.end();) {
            if (it->first == path || util::startsWith(it->first, path + "/")) {
                objs_.erase(it->second);
                it = byPath_.erase(it);
            } else {
                ++it;
            }
        }
    }
    // After a rename / move: the object keeps its handle (computers go on using it), and so does everything inside it.
    void repath(uint32_t h, const std::string& to, uint32_t storage, uint32_t parent) {
        std::string from = objs_[h].path;
        std::vector<std::pair<std::string, uint32_t>> moved;
        for (auto it = byPath_.begin(); it != byPath_.end();) {
            if (it->first == from || util::startsWith(it->first, from + "/")) {
                moved.push_back(*it);
                it = byPath_.erase(it);
            } else {
                ++it;
            }
        }
        forget(to);  // whatever was at the destination before
        for (auto& [path, id] : moved) {
            std::string np = to + path.substr(from.size());
            objs_[id].path = np;
            objs_[id].storage = storage;
            byPath_[np] = id;
        }
        objs_[h].parent = parent;
    }
    // The folder for a parent handle (0 / 0xFFFFFFFF: the storage's root); "" if it isn't a folder.
    std::string folder(uint32_t storageId, uint32_t parent, uint32_t* storageOut = nullptr) {
        if (parent == 0 || parent == kAll) {
            const Storage* s = storage(storageId);
            if (!s && storageId == kAll && !storages_.empty()) s = &storages_[0];
            if (!s) return "";
            if (storageOut) *storageOut = s->id;
            return s->root;
        }
        Obj* o = obj(parent);
        if (!o || !util::isDir(o->path)) return "";
        if (storageOut) *storageOut = o->storage;
        return o->path;
    }
    std::vector<uint32_t> children(uint32_t storageId, uint32_t parent, uint16_t format) {
        std::vector<uint32_t> out;
        std::vector<uint32_t> ids;
        if (storageId == kAll && (parent == 0 || parent == kAll)) {
            for (auto& s : storages_) ids.push_back(s.id);
        } else {
            ids.push_back(storageId);
        }
        for (uint32_t id : ids) {
            uint32_t sid = id;
            std::string dir = folder(id, parent, &sid);
            if (dir.empty()) continue;
            for (auto& e : util::listDir(dir)) {
                if (e.name == "." || e.name == "..") continue;
                if (format && formatOf(e.name, e.dir) != format) continue;
                out.push_back(handleFor(sid, parent == kAll ? 0 : parent, dir + "/" + e.name));
            }
        }
        return out;
    }

    void objectInfo(Out& o, uint32_t h, const Obj& ob) {
        struct stat st{};
        stat(ob.path.c_str(), &st);
        bool dir = S_ISDIR(st.st_mode);
        std::string name = util::basename(ob.path);
        o.u32(ob.storage);
        o.u16(formatOf(name, dir));
        o.u16(0);  // protection: none
        o.u32(dir ? 0 : uint32_t(std::min<uint64_t>(uint64_t(st.st_size), 0xFFFFFFFFull)));
        o.u16(0);  // no thumbnail
        for (int k = 0; k < 6; k++) o.u32(0);
        o.u32(ob.parent);
        o.u16(dir ? 1 : 0);  // association type: generic folder
        o.u32(0);
        o.u32(0);
        o.str(name);
        o.str(mtpDate(st.st_mtime));
        o.str(mtpDate(st.st_mtime));
        o.str("");
        (void)h;
    }
    bool propValue(Out& o, uint32_t h, const Obj& ob, uint16_t prop, uint16_t* type = nullptr) {
        struct stat st{};
        stat(ob.path.c_str(), &st);
        bool dir = S_ISDIR(st.st_mode);
        auto t = [&](uint16_t x) { if (type) *type = x; };
        switch (prop) {
            case PropStorageID: t(TU32); o.u32(ob.storage); return true;
            case PropObjectFormat: t(TU16); o.u16(formatOf(ob.path, dir)); return true;
            case PropProtection: t(TU16); o.u16(0); return true;
            case PropObjectSize: t(TU64); o.u64(dir ? 0 : uint64_t(st.st_size)); return true;
            case PropFileName: case PropName: t(TStr); o.str(util::basename(ob.path)); return true;
            case PropDateModified: t(TStr); o.str(mtpDate(st.st_mtime)); return true;
            case PropParent: t(TU32); o.u32(ob.parent); return true;
            case PropUid: t(TU128); o.u64(uint64_t(ob.storage) << 32 | h); o.u64(0); return true;
            default: return false;
        }
    }
    static uint16_t propType(uint16_t prop) {
        switch (prop) {
            case PropStorageID: case PropParent: return TU32;
            case PropObjectFormat: case PropProtection: return TU16;
            case PropObjectSize: return TU64;
            case PropUid: return TU128;
            case PropFileName: case PropName: case PropDateModified: return TStr;
            default: return 0;
        }
    }

    bool streamFile(const std::string& path, uint64_t off, uint64_t count) {
        int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) return respond(RcAccessDenied);
        uint64_t total = 12 + count;
        Out h;
        h.u32(total > 0xFFFFFFFFull ? 0xFFFFFFFFu : uint32_t(total));
        h.u16(kData);
        h.u16(op_);
        h.u32(tid_);
        std::vector<uint8_t> buf(kChunk);
        memcpy(buf.data(), h.b.data(), 12);
        size_t have = 12;
        uint64_t left = count, at = off;
        bool ok = true;
        l_.clearCancel();
        while (ok) {
            size_t want = (size_t)std::min<uint64_t>(left, buf.size() - have);
            ssize_t r = want ? pread(fd, buf.data() + have, want, (off_t)at) : 0;
            if (r < 0) { ok = false; break; }
            if (r == 0 && want) {  // the file shrank while being sent: pad with zeros, the length is already promised
                memset(buf.data() + have, 0, want);
                r = ssize_t(want);
            }
            have += size_t(r);
            at += uint64_t(r);
            left -= uint64_t(r);
            if (!l_.write(buf.data(), have)) { ok = false; break; }
            setStatus([have](Status& s) { s.bytesOut += have; });
            have = 0;
            if (!left) break;
            if (l_.cancelled()) { ::close(fd); return true; }  // cancelled by the computer: no response
        }
        ::close(fd);
        if (!ok) return false;
        l_.endOfData(total);
        setStatus([](Status& s) { s.filesOut++; });
        return respond(RcOK, {uint32_t(std::min<uint64_t>(count, 0xFFFFFFFFull))});
    }

    // SendObject: the file arrives as a data phase.
    bool receiveFile(const std::string& path, uint64_t expected, uint16_t& rc) {
        uint8_t h[12];
        if (!readExact(h, 12)) return false;
        In in{h, 12};
        uint32_t len = in.u32();
        uint16_t type = in.u16();
        rc = RcOK;
        if (type != kData) { rc = RcInvalidDataset; return true; }
        int fd = path.empty() ? -1 : ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0664);
        if (!path.empty() && fd < 0) rc = errno == ENOSPC ? RcStoreFull : RcAccessDenied;
        // Over USB a file of 4 GiB or more has length 0xFFFFFFFF: it ends where the transfer ends.
        bool toEnd = len == 0xFFFFFFFFu && l_.usb();
        uint64_t left = len == 0xFFFFFFFFu ? (expected ? expected : UINT64_MAX) : uint64_t(len - 12);
        while (left) {
            if (toEnd && roff_ >= rlen_ && lastShort_) break;
            if (!fill()) { if (fd >= 0) ::close(fd); return false; }
            size_t k = (size_t)std::min<uint64_t>(left, rlen_ - roff_);
            if (fd >= 0 && rc == RcOK) {
                const uint8_t* p = rbuf_.data() + roff_;
                for (size_t done = 0; done < k;) {
                    ssize_t w = ::write(fd, p + done, k - done);
                    if (w < 0 && errno == EINTR) continue;
                    if (w <= 0) { rc = errno == ENOSPC ? RcStoreFull : RcGeneralError; break; }
                    done += size_t(w);
                }
            }
            roff_ += k;
            left -= k;
            setStatus([k](Status& s) { s.bytesIn += k; });
            if (toEnd && roff_ >= rlen_ && lastShort_) break;
        }
        if (fd >= 0) {
            if (fsync(fd) != 0 && rc == RcOK) rc = RcGeneralError;
            ::close(fd);
        }
        if (rc != RcOK && !path.empty()) unlink(path.c_str());
        else if (!path.empty()) setStatus([](Status& s) { s.filesIn++; });
        return true;
    }

    // ---- operations
    bool handle(uint16_t code, const uint32_t* p, size_t np) {
        (void)np;
        if (code != OpGetDeviceInfo && code != OpOpenSession && !session_) {
            if (code == OpSendObjectInfo || code == OpSendObject || code == OpSetObjectPropValue || code == OpSetDevicePropValue ||
                code == OpSetObjectReferences) {
                std::string skip;
                if (!recvData(skip)) return false;
            }
            return respond(RcSessionNotOpen);
        }
        switch (code) {
            case OpGetDeviceInfo: {
                const device::Info& di = device::info();
                Out o;
                o.u16(100);
                o.u32(6);  // Microsoft's MTP extension
                o.u16(100);
                o.str("microsoft.com: 1.0; android.com: 1.0;");
                o.u16(0);
                o.a16(kOps, sizeof kOps / 2);
                o.a16(nullptr, 0);  // events: none are sent
                o.a16(kDevProps, sizeof kDevProps / 2);
                o.a16(nullptr, 0);
                o.a16(kFormats, sizeof kFormats / 2);
                o.str(di.manufacturer.empty() ? "Android" : di.manufacturer);
                o.str(di.model.empty() ? "Phone" : di.model);
                o.str("FMRRE 2.0.0");
                o.str(di.serial.empty() ? "FMRRE" : di.serial);
                return sendData(o.b) && respond(RcOK);
            }
            case OpOpenSession:
                if (session_) return respond(RcSessionAlreadyOpen, {sessionId_});
                if (p[0] == 0) return respond(RcInvalidParameter);
                session_ = true;
                sessionId_ = p[0];
                objs_.clear();
                byPath_.clear();
                next_ = 1;
                scanStorages();
                return respond(RcOK);
            case OpCloseSession:
                session_ = false;
                return respond(RcOK);
            case OpGetStorageIDs: {
                scanStorages();
                std::vector<uint32_t> ids;
                for (auto& s : storages_) ids.push_back(s.id);
                Out o;
                o.a32(ids);
                return sendData(o.b) && respond(RcOK);
            }
            case OpGetStorageInfo: {
                const Storage* s = storage(p[0]);
                if (!s) return respond(RcInvalidStorageID);
                struct statvfs vf{};
                statvfs(s->root.c_str(), &vf);
                Out o;
                o.u16(s->removable ? 4 : 3);  // removable / fixed RAM
                o.u16(2);                     // generic hierarchical
                o.u16(0);                     // read-write
                uint64_t bs = vf.f_frsize ? vf.f_frsize : vf.f_bsize;
                o.u64(uint64_t(vf.f_blocks) * bs);
                o.u64(uint64_t(vf.f_bavail) * bs);
                o.u32(kAll);
                o.str(s->desc);
                o.str("fmrre-" + std::to_string(s->id));
                return sendData(o.b) && respond(RcOK);
            }
            case OpGetNumObjects:
                if (p[0] != kAll && !storage(p[0])) return respond(RcInvalidStorageID);
                return respond(RcOK, {uint32_t(children(p[0], p[2], uint16_t(p[1])).size())});
            case OpGetObjectHandles: {
                if (p[0] != kAll && !storage(p[0])) return respond(RcInvalidStorageID);
                if (p[2] != 0 && p[2] != kAll && !obj(p[2])) return respond(RcInvalidParentObject);
                Out o;
                o.a32(children(p[0], p[2], uint16_t(p[1])));
                return sendData(o.b) && respond(RcOK);
            }
            case OpGetObjectInfo: {
                Obj* ob = obj(p[0]);
                if (!ob) return respond(RcInvalidObjectHandle);
                Out o;
                objectInfo(o, p[0], *ob);
                return sendData(o.b) && respond(RcOK);
            }
            case OpGetObject: {
                Obj* ob = obj(p[0]);
                if (!ob || util::isDir(ob->path)) return respond(RcInvalidObjectHandle);
                return streamFile(ob->path, 0, util::fileSize(ob->path));
            }
            case OpGetPartialObject:
            case OpGetPartialObject64: {
                Obj* ob = obj(p[0]);
                if (!ob || util::isDir(ob->path)) return respond(RcInvalidObjectHandle);
                uint64_t off = code == OpGetPartialObject ? p[1] : (uint64_t(p[2]) << 32 | p[1]);
                uint64_t want = code == OpGetPartialObject ? p[2] : p[3], size = util::fileSize(ob->path);
                uint64_t count = off >= size ? 0 : std::min<uint64_t>(want, size - off);
                return streamFile(ob->path, off, count);
            }
            case OpDeleteObject: {
                if (p[0] == kAll) return respond(RcOperationNotSupported);  // "everything": no
                Obj* ob = obj(p[0]);
                if (!ob) return respond(RcInvalidObjectHandle);
                std::string path = ob->path;
                bool ok = util::isDir(path) ? util::removeTree(path) : unlink(path.c_str()) == 0;
                forget(path);
                return respond(ok ? RcOK : RcAccessDenied);
            }
            case OpSendObjectInfo: {
                std::string data;
                if (!recvData(data)) return false;
                pending_.clear();
                uint32_t sid = p[0];
                std::string dir = folder(p[0], p[1], &sid);
                if (dir.empty()) return respond(p[1] == 0 || p[1] == kAll ? RcInvalidStorageID : RcInvalidParentObject);
                if (data.size() < 53) return respond(RcInvalidDataset);
                In in{(const uint8_t*)data.data(), data.size()};
                in.i = 4;  // ObjectInfo: StorageID u32, ObjectFormat u16, Protection u16, CompressedSize u32 ...
                uint16_t fmt = in.u16();
                in.i = 8;
                uint32_t size = in.u32();
                in.i = 52;  // ... the file name follows the 52-byte fixed part
                std::string name = in.str();
                if (!in.ok || !safeName(name)) return respond(RcInvalidDataset);
                std::string path = dir + "/" + name;
                uint32_t parent = (p[1] == kAll) ? 0 : p[1];
                if (fmt == FmtFolder) {
                    if (!util::isDir(path) && mkdir(path.c_str(), 0775) != 0) return respond(errno == ENOSPC ? RcStoreFull : RcAccessDenied);
                    return respond(RcOK, {sid, parent, handleFor(sid, parent, path)});
                }
                struct statvfs vf{};
                if (statvfs(dir.c_str(), &vf) == 0 && size != kAll && uint64_t(vf.f_bavail) * vf.f_frsize < size) return respond(RcStoreFull);
                pending_ = path;
                pendingSize_ = size == kAll ? 0 : size;
                pendingHandle_ = handleFor(sid, parent, path);
                return respond(RcOK, {sid, parent, pendingHandle_});
            }
            case OpSendObject: {
                uint16_t rc = RcOK;
                std::string path = pending_;
                pending_.clear();
                if (!receiveFile(path, pendingSize_, rc)) return false;
                if (path.empty()) return respond(RcNoValidObjectInfo);
                if (rc != RcOK) forget(path);
                return respond(rc);
            }
            case OpMoveObject:
            case OpCopyObject: {
                Obj* ob = obj(p[0]);
                if (!ob) return respond(RcInvalidObjectHandle);
                uint32_t sid = p[1];
                std::string dir = folder(p[1], p[2], &sid);
                if (dir.empty()) return respond(RcInvalidParentObject);
                std::string from = ob->path, to = dir + "/" + util::basename(from);
                if (to == from) return respond(code == OpMoveObject ? RcOK : RcInvalidParentObject);
                if (code == OpCopyObject) {
                    if (util::isDir(from)) return respond(RcOperationNotSupported);
                    if (!util::copyFile(from, to)) return respond(RcAccessDenied);
                    return respond(RcOK, {handleFor(sid, p[2] == kAll ? 0 : p[2], to)});
                }
                if (rename(from.c_str(), to.c_str()) != 0) return respond(errno == EXDEV ? RcOperationNotSupported : RcAccessDenied);
                repath(p[0], to, sid, p[2] == kAll ? 0 : p[2]);
                return respond(RcOK);
            }
            case OpGetDevicePropDesc:
            case OpGetDevicePropValue: {
                Out v;
                uint16_t type;
                if (p[0] == DpFriendlyName || p[0] == DpSyncPartner) {
                    type = TStr;
                    v.str(p[0] == DpFriendlyName ? device::info().model + " (FMRRE)" : "");
                } else if (p[0] == DpBattery) {
                    type = TU8;
                    v.u8(uint8_t(std::max(0, device::battery().percent)));
                } else {
                    return respond(RcDevicePropNotSupported);
                }
                if (code == OpGetDevicePropValue) return sendData(v.b) && respond(RcOK);
                Out o;
                o.u16(uint16_t(p[0]));
                o.u16(type);
                o.u8(p[0] == DpBattery ? 0 : 1);  // battery: read-only
                o.b += v.b;                        // factory default
                o.b += v.b;                        // current
                if (p[0] == DpBattery) { o.u8(1); o.u8(0); o.u8(100); o.u8(1); }  // range 0..100
                else o.u8(0);
                return sendData(o.b) && respond(RcOK);
            }
            case OpSetDevicePropValue:
            case OpSetObjectReferences: {
                std::string skip;
                if (!recvData(skip)) return false;
                return respond(RcOK);
            }
            case OpGetObjectReferences: {
                if (!obj(p[0])) return respond(RcInvalidObjectHandle);
                Out o;
                o.a32({});
                return sendData(o.b) && respond(RcOK);
            }
            case OpGetObjectPropsSupported: {
                Out o;
                o.a16(kProps, sizeof kProps / 2);
                return sendData(o.b) && respond(RcOK);
            }
            case OpGetObjectPropDesc: {
                uint16_t prop = uint16_t(p[0]), type = propType(prop);
                if (!type) return respond(RcInvalidObjectPropCode);
                Out o;
                o.u16(prop);
                o.u16(type);
                o.u8(prop == PropFileName ? 1 : 0);  // the file name can be changed (rename)
                switch (type) {
                    case TU16: o.u16(0); break;
                    case TU32: o.u32(0); break;
                    case TU64: o.u64(0); break;
                    case TU128: o.u64(0); o.u64(0); break;
                    default: o.str(""); break;
                }
                o.u32(0);  // group
                o.u8(0);   // no form
                return sendData(o.b) && respond(RcOK);
            }
            case OpGetObjectPropValue: {
                Obj* ob = obj(p[0]);
                if (!ob) return respond(RcInvalidObjectHandle);
                Out o;
                if (!propValue(o, p[0], *ob, uint16_t(p[1]))) return respond(RcInvalidObjectPropCode);
                return sendData(o.b) && respond(RcOK);
            }
            case OpSetObjectPropValue: {
                std::string data;
                if (!recvData(data)) return false;
                Obj* ob = obj(p[0]);
                if (!ob) return respond(RcInvalidObjectHandle);
                if (p[1] != PropFileName && p[1] != PropName) return respond(RcAccessDenied);
                In in{(const uint8_t*)data.data(), data.size()};
                std::string name = in.str();
                if (!in.ok || !safeName(name)) return respond(RcInvalidParameter);
                std::string from = ob->path, to = util::dirname(from) + "/" + name;
                if (rename(from.c_str(), to.c_str()) != 0) return respond(RcAccessDenied);
                repath(p[0], to, ob->storage, ob->parent);
                return respond(RcOK);
            }
            case OpGetObjectPropList: {
                // params: handle, format, property (0xFFFFFFFF = all), group, depth (0: the object, 1: its children)
                if (p[3] != 0) return respond(RcSpecByGroupUnsupported);
                std::vector<uint32_t> handles;
                if (p[4] == 0 && p[0] != 0 && p[0] != kAll) {
                    if (!obj(p[0])) return respond(RcInvalidObjectHandle);
                    handles.push_back(p[0]);
                } else if (p[4] == 1) {
                    if (p[0] != 0 && p[0] != kAll && !obj(p[0])) return respond(RcInvalidObjectHandle);
                    handles = children(kAll, p[0], uint16_t(p[1]));
                } else {
                    return respond(RcSpecByDepthUnsupported);
                }
                std::vector<uint16_t> props;
                if (p[2] == kAll) props.assign(kProps, kProps + sizeof kProps / 2);
                else if (propType(uint16_t(p[2]))) props.push_back(uint16_t(p[2]));
                else return respond(RcInvalidObjectPropCode);
                Out o;
                o.u32(0);  // element count, filled in below
                uint32_t count = 0;
                for (uint32_t h : handles) {
                    Obj* ob = obj(h);
                    if (!ob) continue;
                    for (uint16_t prop : props) {
                        Out v;
                        uint16_t type = 0;
                        if (!propValue(v, h, *ob, prop, &type)) continue;
                        o.u32(h);
                        o.u16(prop);
                        o.u16(type);
                        o.b += v.b;
                        count++;
                    }
                }
                for (int k = 0; k < 4; k++) o.b[k] = char(count >> (8 * k));
                return sendData(o.b) && respond(RcOK);
            }
            default:
                return respond(RcOperationNotSupported);
        }
    }

    Link& l_;
    std::vector<uint8_t> rbuf_;
    size_t roff_ = 0, rlen_ = 0;
    bool lastShort_ = false;
    uint32_t tid_ = 0;
    uint16_t op_ = 0;
    bool session_ = false;
    uint32_t sessionId_ = 0;
    std::vector<Storage> storages_;
    std::map<uint32_t, Obj> objs_;
    std::map<std::string, uint32_t> byPath_;
    uint32_t next_ = 1;
    std::string pending_;
    uint64_t pendingSize_ = 0;
    uint32_t pendingHandle_ = 0;
};

// ---- serving -----------------------------------------------------------------------------------------

int listenTcp(const Options& o, std::string& err) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) { err = strerror(errno); return -1; }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)o.tcpPort);
    if (inet_pton(AF_INET, o.tcpBind.c_str(), &a.sin_addr) != 1) a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (sockaddr*)&a, sizeof(a)) != 0 || listen(fd, 1) != 0) {
        err = "MTP port " + std::to_string(o.tcpPort) + ": " + strerror(errno);
        close(fd);
        return -1;
    }
    return fd;
}

std::atomic<UsbLink*> g_activeUsb{nullptr};

void wake();

// The serving thread must not outlive the program (a joinable std::thread at exit aborts it).
struct StopAtExit {
    ~StopAtExit() {
        if (!g_thread.joinable()) return;
        g_stop = true;
        wake();
        g_thread.join();
    }
} g_stopAtExit;

struct Endpoint {
    std::unique_ptr<UsbLink> usb;
    int lfd = -1;
};

// Opens what MTP is served on - here, so the caller hears at once when that isn't possible.
bool prepare(const Options& o, Endpoint& ep, std::string& err) {
    if (o.tcpPort) {
        ep.lfd = listenTcp(o, err);
        if (ep.lfd < 0) return false;
        setStatus([&](Status& s) { s.transport = "TCP " + o.tcpBind + ":" + std::to_string(o.tcpPort); });
        return true;
    }
    ep.usb = std::make_unique<UsbLink>();
    if (!ep.usb->open(err)) { ep.usb.reset(); return false; }
    setStatus([](Status& s) { s.transport = "USB"; });
    return true;
}

// Serves until g_stop. USB: one link for the whole time; TCP: one computer after another.
void loop(Endpoint& ep) {
    LOGI("mtp: serving %s", ep.lfd >= 0 ? "over TCP" : "over USB");
    if (ep.usb) g_activeUsb = ep.usb.get();
    while (!g_stop) {
        if (ep.usb) {
            Server(*ep.usb).serve();
            if (!g_stop) usleep(200 * 1000);  // cable out, host reset: wait for the next session
            continue;
        }
        pollfd pf{ep.lfd, POLLIN, 0};
        if (poll(&pf, 1, 250) <= 0) continue;
        sockaddr_in a{};
        socklen_t al = sizeof(a);
        int fd = accept(ep.lfd, (sockaddr*)&a, &al);
        if (fd < 0) continue;
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        char ip[INET_ADDRSTRLEN] = "?";
        inet_ntop(AF_INET, &a.sin_addr, ip, sizeof(ip));
        TcpLink link(fd, ip);
        setStatus([&](Status& s) { s.connected = true; s.peer = ip; });
        Server(link).serve();
        setStatus([](Status& s) { s.connected = false; s.peer.clear(); });
    }
    g_activeUsb = nullptr;
    ep.usb.reset();  // gives the USB gadget back to adb
    if (ep.lfd >= 0) close(ep.lfd);
    ep.lfd = -1;
}

void wake() {  // a blocked USB read only returns when the gadget goes away
    if (UsbLink* u = g_activeUsb.load()) u->interrupt();
}

}  // namespace

Status status() {
    std::lock_guard<std::mutex> l(g_mu);
    return g_status;
}

bool running() { return status().running; }

bool start(const Options& o, std::string& err) {
    if (running()) return true;
    if (g_thread.joinable()) g_thread.join();
    g_stop = false;
    setStatus([](Status& s) { s = Status(); });
    auto ep = std::make_shared<Endpoint>();
    if (!prepare(o, *ep, err)) {
        setStatus([&](Status& s) { s.error = err; });
        return false;
    }
    setStatus([](Status& s) { s.running = true; });
    g_thread = std::thread([ep] {
        loop(*ep);
        setStatus([](Status& s) { s.running = false; s.connected = false; });
    });
    return true;
}

void stop() {
    g_stop = true;
    wake();
    if (g_thread.joinable()) g_thread.join();
    setStatus([](Status& s) { s.running = false; s.connected = false; });
}

bool run(const Options& o, Task& t) {
    g_stop = false;
    setStatus([](Status& s) { s = Status(); });
    t.setStatus("MTP");
    Endpoint ep;
    std::string err;
    if (!prepare(o, ep, err)) {
        t.print("MTP: %s", err.c_str());
        return false;
    }
    setStatus([](Status& s) { s.running = true; });
    if (o.tcpPort) t.print("MTP on TCP %s:%d - waiting for a computer", o.tcpBind.c_str(), o.tcpPort);
    else t.print("MTP on USB - the phone shows up in the computer's file manager");
    std::thread watch([&t] {
        while (!g_stop && !t.cancelled()) usleep(200 * 1000);
        g_stop = true;
        wake();
    });
    loop(ep);
    g_stop = true;
    watch.join();
    setStatus([](Status& s) { s.running = false; s.connected = false; });
    return true;
}

}  // namespace tw::mtp
