ifeq ($(CONFIG_ARCH_KALAMA),y)
dtbo-y += kalama-ipa.dtbo
dtbo-y += kalama-pinn-ipa.dtbo
endif

ifeq ($(CONFIG_ARCH_CROW),y)
dtbo-y += crow-ipa.dtbo
endif

ifeq ($(CONFIG_ARCH_TRINKET),y)
dtbo-y += trinket-ipa.dtbo
endif

always-y	:= $(dtb-y) $(dtbo-y)
subdir-y	:= $(dts-dirs)
clean-files	:= *.dtb *.dtbo
