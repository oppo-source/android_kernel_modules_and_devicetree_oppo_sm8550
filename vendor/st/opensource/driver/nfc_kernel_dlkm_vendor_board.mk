# Build NFC kernel driver
ifeq ($(call is-board-platform-in-list, kalama),true)
BOARD_VENDOR_KERNEL_MODULES += $(KERNEL_MODULES_OUT)/stm_nfc_i2c.ko
endif
