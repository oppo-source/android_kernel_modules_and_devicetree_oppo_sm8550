// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2020-2025 Oplus. All rights reserved.
 */

#include "internal.h"
#if IS_ENABLED(CONFIG_DRM_PANEL_NOTIFY) || IS_ENABLED(CONFIG_QCOM_PANEL_EVENT_NOTIFIER)
#include <linux/soc/qcom/panel_event_notifier.h>
#include <linux/of.h>
#include <drm/drm_panel.h>
#elif IS_ENABLED(CONFIG_DRM_MSM) || IS_ENABLED(CONFIG_DRM_OPLUS_NOTIFY)
#include <linux/msm_drm_notify.h>
#elif IS_ENABLED(CONFIG_OPLUS_MTK_DRM_GKI_NOTIFY)
#include <linux/mtk_panel_ext.h>
#include <linux/mtk_disp_notify.h>
#endif

#if IS_ENABLED(CONFIG_DRM_PANEL_NOTIFY) || IS_ENABLED(CONFIG_QCOM_PANEL_EVENT_NOTIFIER)
struct panel_notify_context {
	bool is_fold_dev;
	void *notifier_cookie;
	void *notifier_cookie_second;
	struct drm_panel *active_panel;
	struct drm_panel *active_panel_second;
};
#endif

#if IS_ENABLED(CONFIG_DRM_MSM) || IS_ENABLED(CONFIG_DRM_OPLUS_NOTIFY) || IS_ENABLED(CONFIG_OPLUS_MTK_DRM_GKI_NOTIFY)
static struct notifier_block fb_notif;
#elif IS_ENABLED(CONFIG_DRM_PANEL_NOTIFY) || IS_ENABLED(CONFIG_QCOM_PANEL_EVENT_NOTIFIER)
static struct panel_notify_context notif_cxt;
#endif

atomic_t display_off = ATOMIC_LONG_INIT(0);

#if IS_ENABLED(CONFIG_DRM_PANEL_NOTIFY) || IS_ENABLED(CONFIG_QCOM_PANEL_EVENT_NOTIFIER)
/*
 * xueying-sensor-22003.dtsi: ssc_interactive { is-folding-device; };
 * whiteswan-22001-cape-oplus-sensor.dtsi: ssc_interactive { is-folding-device; };
 */
static int get_dev_type(void)
{
	struct device_node *node = NULL;

	node = of_find_node_by_name(NULL, "ssc_interactive");
	if (!node) {
		log_err("ssc_interactive dts info missing\n");
		return -ENOENT;
	} else {
		if (of_property_read_bool(node, "is-folding-device"))
			notif_cxt.is_fold_dev = true;
		else
			notif_cxt.is_fold_dev = false;
	}

	log_info("get_dev_type: is-folding-device - %s\n", notif_cxt.is_fold_dev ? "yes" : "no");

	return 0;
}

static void get_active_panel(void)
{
	int i;
	int count;
	int count_sec;
	struct device_node *np = NULL;
	struct device_node *node = NULL;
	struct drm_panel *panel = NULL;
	struct device_node *node_sec = NULL;
	struct drm_panel *panel_sec = NULL;

	np = of_find_node_by_name(NULL, "oplus,dsi-display-dev");
	if (!np) {
		log_err("oplus,dsi-display-dev node missing\n");
		return;
	}

	log_info("oplus,dsi-display-dev node found\n");

	count = of_count_phandle_with_args(np, "oplus,dsi-panel-primary", NULL);
	if (count <= 0) {
		log_err("oplus,dsi-panel-primary missing\n");
		goto err_out;
	}

	for (i = 0; i < count; i++) {
		node = of_parse_phandle(np, "oplus,dsi-panel-primary", i);
		panel = of_drm_find_panel(node);
		of_node_put(node);
		if (!IS_ERR(panel)) {
			notif_cxt.active_panel = panel;
			log_info("active panel found\n");
		}
	}

	/* for folding phone */
	if (notif_cxt.is_fold_dev) {
		count_sec = of_count_phandle_with_args(np, "oplus,dsi-panel-secondary", NULL);
		if (count_sec <= 0) {
			log_err("oplus,dsi-panel-secondary missing\n");
			goto err_out;
		}

		for (i = 0; i < count_sec; i++) {
			node_sec = of_parse_phandle(np, "oplus,dsi-panel-secondary", i);
			panel_sec = of_drm_find_panel(node_sec);
			of_node_put(node_sec);
			if (!IS_ERR(panel_sec)) {
				notif_cxt.active_panel_second = panel_sec;
				log_info("active secondary panel found\n");
			}
		}
	}

err_out:
	of_node_put(np);
}

static void bright_fb_notifier_callback(enum panel_event_notifier_tag tag,
	struct panel_event_notification *notification, void *client_data)
{
	if (!notification) {
		log_info("%s, invalid notify\n", __func__);
		return;
	}

	switch (notification->notif_type) {
	case DRM_PANEL_EVENT_BLANK:
		atomic_set(&display_off, 1);
		break;
	case DRM_PANEL_EVENT_UNBLANK:
		atomic_set(&display_off, 0);
		break;
	default:
		break;
	}
}
#elif IS_ENABLED(CONFIG_DRM_MSM) || IS_ENABLED(CONFIG_DRM_OPLUS_NOTIFY)
static int bright_fb_notifier_callback(struct notifier_block *self,
		unsigned long event, void *data)
{
	struct msm_drm_notifier *evdata = data;
	int *blank;

	if (evdata && evdata->data) {
		blank = evdata->data;

		if (*blank ==  MSM_DRM_BLANK_POWERDOWN)
			atomic_set(&display_off, 1);
		else if (*blank == MSM_DRM_BLANK_UNBLANK)
			atomic_set(&display_off, 0);
	}

	return NOTIFY_OK;
}
#elif IS_ENABLED(CONFIG_OPLUS_MTK_DRM_GKI_NOTIFY)
static int mtk_bright_fb_notifier_callback(struct notifier_block *self,
		unsigned long event, void *data)
{
	int *blank = (int *)data;

	if (!blank) {
		log_err("get disp stat err, blank is NULL!\n");
		return 0;
	}

	if (*blank == MTK_DISP_BLANK_POWERDOWN)
		atomic_set(&display_off, 1);
	else if (*blank == MTK_DISP_BLANK_UNBLANK)
		atomic_set(&display_off, 0);
	return NOTIFY_OK;
}
#endif

void register_panel_event_notifier(void)
{
#if IS_ENABLED(CONFIG_DRM_PANEL_NOTIFY) || IS_ENABLED(CONFIG_QCOM_PANEL_EVENT_NOTIFIER)
	int ret;
	void *cookie = ERR_PTR(-EINVAL);

	ret = get_dev_type();
	if (ret) {
		log_err("get_dev_type failed, ret = %d\n", ret);
	}

	get_active_panel();

	if (notif_cxt.active_panel)
		cookie = panel_event_notifier_register(PANEL_EVENT_NOTIFICATION_PRIMARY,
			PANEL_EVENT_NOTIFIER_CLIENT_PRIMARY_MM, notif_cxt.active_panel,
			bright_fb_notifier_callback, NULL);

	if (!IS_ERR(cookie)) {
		log_info("%s primary succeed\n", __func__);
		notif_cxt.notifier_cookie = cookie;
	} else {
		log_err("%s primary failed\n", __func__);
		return;
	}

	/* for folding phone */
	cookie = ERR_PTR(-EINVAL);
	if (notif_cxt.active_panel_second)
		cookie = panel_event_notifier_register(PANEL_EVENT_NOTIFICATION_SECONDARY,
			PANEL_EVENT_NOTIFIER_CLIENT_SECONDARY_MM, notif_cxt.active_panel_second,
			bright_fb_notifier_callback, NULL);

	if (!IS_ERR(cookie)) {
		log_info("%s secondary succeed\n", __func__);
		notif_cxt.notifier_cookie_second = cookie;
	} else {
		log_err("%s secondary failed\n", __func__);
	}
#elif IS_ENABLED(CONFIG_DRM_MSM) || IS_ENABLED(CONFIG_DRM_OPLUS_NOTIFY)
	fb_notif.notifier_call = bright_fb_notifier_callback;
	if (msm_drm_register_client(&fb_notif))
		log_err("msm_drm_register_client failed\n");
#elif IS_ENABLED(CONFIG_OPLUS_MTK_DRM_GKI_NOTIFY)
	fb_notif.notifier_call = mtk_bright_fb_notifier_callback;
	if (mtk_disp_notifier_register("Oplus_hybridswap", &fb_notif))
		log_err("mtk_disp_notifier_register failed\n");
#endif
}

void unregister_panel_event_notifier(void)
{
#if IS_ENABLED(CONFIG_DRM_PANEL_NOTIFY) || IS_ENABLED(CONFIG_QCOM_PANEL_EVENT_NOTIFIER)
	if (notif_cxt.notifier_cookie_second) {
		panel_event_notifier_unregister(notif_cxt.notifier_cookie_second);
		notif_cxt.notifier_cookie_second = NULL;
	}
	if (notif_cxt.notifier_cookie) {
		panel_event_notifier_unregister(notif_cxt.notifier_cookie);
		notif_cxt.notifier_cookie = NULL;
	}
#elif IS_ENABLED(CONFIG_DRM_MSM) || IS_ENABLED(CONFIG_DRM_OPLUS_NOTIFY)
	msm_drm_unregister_client(&fb_notif);
#elif IS_ENABLED(CONFIG_OPLUS_MTK_DRM_GKI_NOTIFY)
	mtk_disp_notifier_unregister(&fb_notif);
#endif
}
