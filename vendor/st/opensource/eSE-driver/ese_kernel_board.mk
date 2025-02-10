#Kalama specific build rules
ifeq ($(TARGET_BOARD_PLATFORM),kalama)
TARGET_USES_ST_ESE := true
BOARD_VENDOR_KERNEL_MODULES += $(KERNEL_MODULES_OUT)/stm_st54se_gpio.ko
endif
