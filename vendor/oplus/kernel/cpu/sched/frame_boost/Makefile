# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2018-2020 Oplus. All rights reserved.

LINUXINCLUDE += -I$(srctree)/

obj-$(CONFIG_OPLUS_FEATURE_FRAME_BOOST) += oplus_bsp_frame_boost.o
ifdef CONFIG_OPLUS_FEATURE_MULTI_FBG
$(info multi_rtg)
oplus_bsp_frame_boost-y := ./multi_rtg/frame_boost.o ./multi_rtg/frame_sysctl.o ./multi_rtg/frame_info.o ./multi_rtg/frame_group.o ./multi_rtg/cluster_boost.o ./multi_rtg/frame_debug.o
else
$(info frame_boost_V1)
oplus_bsp_frame_boost-y := ./frame_boost_V1/frame_boost.o ./frame_boost_V1/frame_sysctl.o ./frame_boost_V1/frame_info.o ./frame_boost_V1/frame_group.o ./frame_boost_V1/cluster_boost.o ./frame_boost_V1/frame_debug.o
endif
