LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)

KBUILD_OPTIONS += KBUILD_EXTRA_SYMBOLS=$(PWD)/$(call intermediates-dir-for,DLKM,sec-module-symvers)/Module.symvers

LOCAL_REQUIRED_MODULES := sec-module-symvers
LOCAL_ADDITIONAL_DEPENDENCIES += $(call intermediates-dir-for,DLKM,sec-module-symvers)/Module.symvers

LOCAL_MODULE_PATH := $(KERNEL_MODULES_OUT)
LOCAL_MODULE := stm_nfc_i2c.ko
LOCAL_SRC_FILES   := $(wildcard $(LOCAL_PATH)/**/*) $(wildcard $(LOCAL_PATH)/*)

DLKM_DIR := $(TOP)/device/qcom/common/dlkm

NFC_DLKM_ENABLED := false

########## Check and set local DLKM flag based on system-wide global flags ##########
ifeq ($(TARGET_KERNEL_DLKM_DISABLE), true)
  ifeq ($(TARGET_KERNEL_DLKM_NFC_OVERRIDE), true)
    NFC_DLKM_ENABLED := true
  endif
else
  NFC_DLKM_ENABLED := true
endif

########## Build kernel module based on local DLKM flag status ##########
ifeq ($(NFC_DLKM_ENABLED), true)
  include $(DLKM_DIR)/Build_external_kernelmodule.mk
endif
