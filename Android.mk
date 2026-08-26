# Build spi-virtio.ko

LOCAL_PATH := $(call my-dir)

ifeq ($(ENABLE_VENDOR_CUTTLEFISH), true)

LOCAL_MODULE_DDK_BUILD := true
LOCAL_MODULE_DDK_ALLOW_UNSAFE_HEADERS := true
LOCAL_MODULE_KO_DIRS := spi-virtio.ko

KBUILD_OPTIONS := MODNAME=spi-virtio
KBUILD_OPTIONS += BOARD_PLATFORM=$(TARGET_BOARD_PLATFORM)

# VIRTIO_SPI_ROOT needs to be a absolute since it will be used
# for header files. $(TOP) cannot be used here since it will be
# resolved as "." which won't work for Kbuild.
KBUILD_OPTIONS += VIRTIO_SPI_ROOT=$(abspath $(LOCAL_PATH))

DLKM_DIR   := device/qcom/common/dlkm

include $(CLEAR_VARS)
# For incremental compilation
LOCAL_SRC_FILES           := $(wildcard $(LOCAL_PATH)/**/*) $(wildcard $(LOCAL_PATH)/*)
LOCAL_MODULE              := spi-virtio.ko
LOCAL_MODULE_KBUILD_NAME  := spi-virtio.ko
LOCAL_MODULE_TAGS         := optional
LOCAL_MODULE_PATH         := $(KERNEL_MODULES_OUT)

include $(DLKM_DIR)/Build_external_kernelmodule.mk


endif # ENABLE_VENDOR_CUTTLEFISH
