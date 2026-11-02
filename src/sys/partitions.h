#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace tw {

class Task;

struct Partition {
    std::string mountPoint;   // e.g. /data   (images: /boot)
    std::string fsType;       // ext4, f2fs, vfat, exfat, erofs, emmc (raw image)
    std::string blockDev;     // resolved block device
    std::string displayName;  // "Data"
    std::string name;         // short id used for backup file names: "data"
    std::string mountOpts;
    bool backup = false;
    bool wipe = false;
    bool flashImg = false;
    bool removable = false;
    bool storage = false;     // can hold backups / be browsed
    bool logical = false;     // dynamic partition (super)
    bool readOnly = false;    // erofs etc.

    bool isImage() const { return fsType == "emmc" || fsType == "mtd"; }
    // Where the content lives right now (mount point, or sandbox in sim mode).
    std::string contentPath() const;

    // runtime info (refreshed by Partitions::refresh)
    bool present = false;
    bool mounted = false;
    uint64_t size = 0, used = 0, avail = 0;
};

class Partitions {
public:
    static Partitions& get();

    void load();
    std::vector<Partition>& all() { return parts_; }
    Partition* find(const std::string& mountPointOrName);
    void refresh();
    std::string fstabPath() const { return fstabPath_; }

    bool mount(Partition& p, std::string* err = nullptr);
    bool unmount(Partition& p, std::string* err = nullptr);
    bool isMounted(const Partition& p) const;
    void mountAllStorage();
    void unmountAll();

    // Wipe = delete content (keeps internal storage on /data).
    bool wipe(Partition& p, Task& t);
    // Format = mkfs (destroys everything, including /data/media).
    bool format(Partition& p, Task& t, const std::string& newFs = "");
    bool wipeDalvik(Task& t);
    // Filesystem tools (fsck / resize2fs / resize.f2fs ...). The partition is unmounted first.
    bool repair(Partition& p, Task& t);
    bool resizeFs(Partition& p, Task& t);
    std::string fsInfo(Partition& p);
    // Detect microSD / USB OTG drives that appeared after boot (also adds a USB OTG entry when the fstab has none).
    int rescanRemovable();

    // Storage handling: where backups and screenshots go.
    std::vector<Partition*> storages();
    std::string storagePath();  // e.g. /data/media/0
    void setStorage(const std::string& mountPoint);
    std::string currentStorage() const { return storage_; }
    // /data is mounted but internal storage is not readable (FBE, not decrypted).
    bool dataEncrypted();

    static std::string byNameDir();
    static uint64_t blockSize(const std::string& dev);

private:
    void addFromTwrpLine(const std::vector<std::string>& f);
    void addFromAospLine(const std::vector<std::string>& f);
    void addImagesFromByName();
    void finalize(Partition& p);
    std::vector<Partition> parts_;
    std::string fstabPath_;
    std::string storage_ = "/data";
    std::vector<std::string> simMounted_;
};

}  // namespace tw
