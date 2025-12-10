# SPDX-License-Identifier: GPL-2.0-only
ccflags-y       := -I $(srctree)/mm
obj-$(CONFIG_HYBRIDSWAP_ZRAM) += oplus_bsp_hybridswap_zram.o

obj-$(CONFIG_CRYPTO_ZSTDN) += zstd/
obj-$(CONFIG_CRYPTO_ZSTDN_O) += zstd_o/
obj-$(CONFIG_CRYPTO_LZ4K) += lz4k/

oplus_bsp_hybridswap_zram-y	:=	zcomp.o zram_drv.o
oplus_bsp_hybridswap_zram-$(CONFIG_KCOMPRESSD)	+=  kcompressd.o
oplus_bsp_hybridswap_zram-$(CONFIG_HYBRIDSWAP) += hybridswap/hybridmain.o
oplus_bsp_hybridswap_zram-$(CONFIG_HYBRIDSWAP_SWAPD) += hybridswap/hybridswapd.o hybridswap/display.o
oplus_bsp_hybridswap_zram-$(CONFIG_OPLUS_FEATURE_MM_EZRECLAIMD) += hybridswap/ezreclaimd.o
oplus_bsp_hybridswap_zram-$(CONFIG_HYBRIDSWAP_CORE) += hybridswap/hybridswap.o
