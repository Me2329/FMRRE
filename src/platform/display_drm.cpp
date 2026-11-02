// DRM/KMS backend using dumb buffers (same approach as AOSP minui's drm
// backend). Works on virtually every Android device from the last ~6 years.
#include "platform/display.h"
#include "core/log.h"

#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <vector>

namespace tw {

namespace {

int xioctl(int fd, unsigned long req, void* arg) {
    int r;
    do { r = ioctl(fd, req, arg); } while (r == -1 && (errno == EINTR || errno == EAGAIN));
    return r;
}

struct DumbFb {
    uint32_t handle = 0, fbId = 0, pitch = 0;
    uint64_t size = 0;
    uint8_t* map = nullptr;
};

class DrmDisplay : public Display {
public:
    ~DrmDisplay() override {
        for (auto& b : bufs_) {
            if (b.map) munmap(b.map, b.size);
            if (b.fbId) xioctl(fd_, DRM_IOCTL_MODE_RMFB, &b.fbId);
            if (b.handle) {
                drm_mode_destroy_dumb d{};
                d.handle = b.handle;
                xioctl(fd_, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
            }
        }
        if (fd_ >= 0) close(fd_);
    }

    bool init() override {
        for (int i = 0; i < 4 && fd_ < 0; i++) {
            std::string p = "/dev/dri/card" + std::to_string(i);
            fd_ = open(p.c_str(), O_RDWR | O_CLOEXEC);
            if (fd_ >= 0 && !setup()) {
                close(fd_);
                fd_ = -1;
            }
        }
        return fd_ >= 0;
    }

    int width() const override { return mode_.hdisplay; }
    int height() const override { return mode_.vdisplay; }
    const char* name() const override { return "drm"; }

    void present(const Canvas& c) override {
        DumbFb& b = bufs_[back_];
        const uint32_t* src = c.pixels();
        int w = std::min(c.width(), (int)mode_.hdisplay), h = std::min(c.height(), (int)mode_.vdisplay);
        for (int y = 0; y < h; y++) {
            uint32_t* dst = (uint32_t*)(b.map + size_t(y) * b.pitch);
            if (!rotate180_) {
                memcpy(dst, src + size_t(y) * c.width(), size_t(w) * 4);
            } else {
                const uint32_t* s = src + size_t(c.height() - 1 - y) * c.width();
                for (int x = 0; x < w; x++) dst[x] = s[c.width() - 1 - x];
            }
        }
        setCrtc(b.fbId);
        drm_mode_fb_dirty_cmd dirty{};
        dirty.fb_id = b.fbId;
        xioctl(fd_, DRM_IOCTL_MODE_DIRTYFB, &dirty);  // command-mode panels need this; harmless otherwise
        back_ ^= 1;
    }

    void setBlank(bool blank) override {
        if (blank) {
            drm_mode_crtc crtc{};
            crtc.crtc_id = crtcId_;
            xioctl(fd_, DRM_IOCTL_MODE_SETCRTC, &crtc);
        } else {
            setCrtc(bufs_[back_ ^ 1].fbId);
        }
    }

private:
    bool setCrtc(uint32_t fbId) {
        drm_mode_crtc crtc{};
        crtc.crtc_id = crtcId_;
        crtc.fb_id = fbId;
        crtc.set_connectors_ptr = (uint64_t)(uintptr_t)&connId_;
        crtc.count_connectors = 1;
        crtc.mode = mode_;
        crtc.mode_valid = 1;
        return xioctl(fd_, DRM_IOCTL_MODE_SETCRTC, &crtc) == 0;
    }

    bool setup() {
        xioctl(fd_, DRM_IOCTL_SET_MASTER, nullptr);
        drm_mode_card_res res{};
        if (xioctl(fd_, DRM_IOCTL_MODE_GETRESOURCES, &res) != 0 || res.count_connectors == 0) return false;
        std::vector<uint32_t> fbs(res.count_fbs), crtcs(res.count_crtcs), conns(res.count_connectors),
            encs(res.count_encoders);
        res.fb_id_ptr = (uint64_t)(uintptr_t)fbs.data();
        res.crtc_id_ptr = (uint64_t)(uintptr_t)crtcs.data();
        res.connector_id_ptr = (uint64_t)(uintptr_t)conns.data();
        res.encoder_id_ptr = (uint64_t)(uintptr_t)encs.data();
        if (xioctl(fd_, DRM_IOCTL_MODE_GETRESOURCES, &res) != 0) return false;

        for (uint32_t cid : conns) {
            drm_mode_get_connector conn{};
            conn.connector_id = cid;
            if (xioctl(fd_, DRM_IOCTL_MODE_GETCONNECTOR, &conn) != 0) continue;
            if (conn.connection != 1 || conn.count_modes == 0) continue;  // 1 = connected
            std::vector<drm_mode_modeinfo> modes(conn.count_modes);
            std::vector<uint32_t> cencs(conn.count_encoders);
            std::vector<uint32_t> props(conn.count_props);
            std::vector<uint64_t> pvals(conn.count_props);
            conn.modes_ptr = (uint64_t)(uintptr_t)modes.data();
            conn.encoders_ptr = (uint64_t)(uintptr_t)cencs.data();
            conn.props_ptr = (uint64_t)(uintptr_t)props.data();
            conn.prop_values_ptr = (uint64_t)(uintptr_t)pvals.data();
            if (xioctl(fd_, DRM_IOCTL_MODE_GETCONNECTOR, &conn) != 0) continue;

            mode_ = modes[0];
            for (auto& m : modes)
                if (m.type & DRM_MODE_TYPE_PREFERRED) { mode_ = m; break; }

            uint32_t encId = conn.encoder_id ? conn.encoder_id : (cencs.empty() ? 0 : cencs[0]);
            drm_mode_get_encoder enc{};
            enc.encoder_id = encId;
            if (encId && xioctl(fd_, DRM_IOCTL_MODE_GETENCODER, &enc) == 0) {
                crtcId_ = enc.crtc_id;
                if (!crtcId_) {
                    for (size_t i = 0; i < crtcs.size(); i++)
                        if (enc.possible_crtcs & (1u << i)) { crtcId_ = crtcs[i]; break; }
                }
            }
            if (!crtcId_ && !crtcs.empty()) crtcId_ = crtcs[0];
            connId_ = cid;
            break;
        }
        if (!connId_ || !crtcId_) return false;

        for (auto& b : bufs_) {
            drm_mode_create_dumb cd{};
            cd.width = mode_.hdisplay;
            cd.height = mode_.vdisplay;
            cd.bpp = 32;
            if (xioctl(fd_, DRM_IOCTL_MODE_CREATE_DUMB, &cd) != 0) return false;
            b.handle = cd.handle;
            b.pitch = cd.pitch;
            b.size = cd.size;
            drm_mode_fb_cmd fb{};
            fb.width = mode_.hdisplay;
            fb.height = mode_.vdisplay;
            fb.pitch = cd.pitch;
            fb.bpp = 32;
            fb.depth = 24;
            fb.handle = cd.handle;
            if (xioctl(fd_, DRM_IOCTL_MODE_ADDFB, &fb) != 0) return false;
            b.fbId = fb.fb_id;
            drm_mode_map_dumb md{};
            md.handle = cd.handle;
            if (xioctl(fd_, DRM_IOCTL_MODE_MAP_DUMB, &md) != 0) return false;
            void* p = mmap(nullptr, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, md.offset);
            if (p == MAP_FAILED) return false;
            b.map = (uint8_t*)p;
            memset(b.map, 0, b.size);
        }
        if (!setCrtc(bufs_[0].fbId)) {
            LOGW("drm: SETCRTC failed (%s)", strerror(errno));
            return false;
        }
        LOGI("drm: %ux%u@%u connector=%u crtc=%u", mode_.hdisplay, mode_.vdisplay, mode_.vrefresh, connId_, crtcId_);
        return true;
    }

    int fd_ = -1;
    uint32_t connId_ = 0, crtcId_ = 0;
    drm_mode_modeinfo mode_{};
    DumbFb bufs_[2];
    int back_ = 1;
};

}  // namespace

std::unique_ptr<Display> createDrmDisplay() { return std::make_unique<DrmDisplay>(); }

std::unique_ptr<Display> openHardwareDisplay() {
    auto d = createDrmDisplay();
    if (d->init()) return d;
    LOGI("display: DRM unavailable, trying fbdev");
    d = createFbDisplay();
    if (d->init()) return d;
    return nullptr;
}

}  // namespace tw
