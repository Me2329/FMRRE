# --- TWRP v2 in a device tree -------------------------------------------------
# device.mk:
PRODUCT_PACKAGES += twrpv2_recovery
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/recovery/twrp.fstab:$(TARGET_COPY_OUT_RECOVERY)/root/system/etc/twrp.fstab

# BoardConfig.mk (usual recovery flags still apply):
TARGET_RECOVERY_PIXEL_FORMAT := RGBX_8888
BOARD_USES_RECOVERY_AS_BOOT ?= false
# TWRP v2 needs these tools in the recovery ramdisk for formatting:
#   mke2fs / make_f2fs (from e2fsprogs / f2fs-tools), toybox (tar, getprop, setprop)
