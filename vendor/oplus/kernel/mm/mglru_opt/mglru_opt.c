// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2020-2025 Oplus. All rights reserved.
 */

#define pr_fmt(fmt) "mglru_opt: " fmt

#include <linux/module.h>
#include <trace/hooks/vmscan.h>
#include <trace/hooks/mm.h>
#include <linux/swap.h>
#include <linux/proc_fs.h>
#include <linux/gfp.h>
#include <linux/types.h>
#include <linux/printk.h>
#include <linux/cgroup.h>
#include <linux/kernel.h>
#include <linux/version.h>
#include <linux/vmstat.h>
#include <linux/oom.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <uapi/linux/sched/types.h>
#include <linux/cpufreq.h>
#include <linux/mm.h>
#include <linux/mm_inline.h>
#include <linux/pgtable.h>
#include <linux/mman.h>
#include <linux/init.h>
#include <linux/string.h>
#include <linux/pagewalk.h>
#include <linux/hugetlb.h>
#include <asm/tlb.h>
#include <asm/tlbflush.h>
#include <linux/percpu.h>
#include <linux/local_lock.h>
#include <linux/pagevec.h>
#include <linux/jump_label.h>

#include "mglru_opt.h"
#include "../../mm/internal.h"
#if IS_ENABLED(CONFIG_OPLUS_FEATURE_MM_OSVELTE)
#include "../mm_osvelte/mm-config.h"
#endif /* CONFIG_OPLUS_FEATURE_MM_OSVELTE */

/*
  FIXME:
  Temporarily modified to differentiate between platforms,
  Because some kernels do not have the respin vendor hook yet.
*/
#if IS_ENABLED(CONFIG_OPLUS_FEATURE_MGLRU_OPT)
static atomic_t mglru_opt_enable;

typedef unsigned long (*kallsyms_lookup_name_t)(const char *name);
typedef void (*activate_page_t)(struct page *page);
typedef void (*deactivate_file_page_t)(struct page *page);
typedef void (*__lru_cache_activate_page_t)(struct page *page);

static kallsyms_lookup_name_t kallsyms_lookup_name_dup = NULL;
static activate_page_t activate_page_dup = NULL;
static deactivate_file_page_t deactivate_file_page_dup = NULL;
static __lru_cache_activate_page_t __lru_cache_activate_page_dup = NULL;
static struct static_key *p_lru_gen_caps;

struct tracepoint *tp_android_vh_lru_cache_add_page_activate;
struct tracepoint *tp_android_vh_filemap_fault_pre_page_locked;
struct tracepoint *tp_android_vh_filemap_page_mapped;
struct tracepoint *tp_android_vh_zap_pte_range_page_remove_rmap;

int mglru_opt_enabled(void)
{
	return likely(atomic_read(&mglru_opt_enable));
}

static inline bool lru_gen_enabled_dup(void)
{
	/*
	 * -1 means the first static_key_slow_inc() is in progress.
	 *  static_key_enabled() must return true, so return 1 here.
	 */
	int n = atomic_read(&p_lru_gen_caps->enabled);
	return n == 0 ? 0 : 1;
}

static void __nocfi lru_cache_add_page_activate(void *data,
	struct page *page, bool *bypass)
{
	if (lru_gen_enabled_dup())
		*bypass = true;
}

static void __nocfi filemap_fault_page_activate(void *data,
	struct page *page)
{
	if (lru_gen_enabled_dup() &&
	    lru_gen_in_fault() &&
	    !(current->flags & PF_MEMALLOC) &&
	    !PageActive(page) &&
	    !PageUnevictable(page)) {
		if (PageLRU(page))
			activate_page_dup(page);
		else /* still in lru cache */
			__lru_cache_activate_page_dup(page);
	}
}

static void __nocfi hook_page_remove_rmap_ptes(void *data,
	struct page *page)
{
	/* move unmapped file folios to the tail of min gen */
	if (lru_gen_enabled_dup() && !PageAnon(page) && !page_mapped(page))
		deactivate_file_page_dup(page);
}

static int __nocfi register_mglru_opt_vendor_hooks(void)
{
	int ret = 0;

	ret = tracepoint_probe_register(tp_android_vh_lru_cache_add_page_activate, lru_cache_add_page_activate, NULL);
	if (ret != 0) {
		pr_err("register_trace_android_vh_lru_cache_add_page_activate failed! ret=%d\n",
				ret);
		goto out;
	}

	ret = tracepoint_probe_register(tp_android_vh_filemap_fault_pre_page_locked, filemap_fault_page_activate, NULL);
	if (ret != 0) {
		pr_err("register_trace_android_vh_filemap_fault_pre_page_locked failed! ret=%d\n",
				ret);
		goto out;
	}

	ret = tracepoint_probe_register(tp_android_vh_filemap_page_mapped, filemap_fault_page_activate, NULL);
	if (ret != 0) {
		pr_err("register_trace_android_vh_filemap_page_mapped failed! ret=%d\n",
				ret);
		goto out;
	}

	ret = tracepoint_probe_register(tp_android_vh_zap_pte_range_page_remove_rmap, hook_page_remove_rmap_ptes, NULL);
	if (ret != 0) {
		pr_err("register_trace_android_vh_zap_pte_range_page_remove_rmap failed! ret=%d\n",
				ret);
		goto out;
	}

out:
	return ret;
}

static void __nocfi unregister_mglru_opt_vendor_hooks(void)
{
	if(tp_android_vh_zap_pte_range_page_remove_rmap)
		tracepoint_probe_unregister(tp_android_vh_zap_pte_range_page_remove_rmap, hook_page_remove_rmap_ptes, NULL);
	if(tp_android_vh_filemap_page_mapped)
		tracepoint_probe_unregister(tp_android_vh_filemap_page_mapped, filemap_fault_page_activate, NULL);
	if(tp_android_vh_filemap_fault_pre_page_locked)
		tracepoint_probe_unregister(tp_android_vh_filemap_fault_pre_page_locked, filemap_fault_page_activate,
		NULL);
	if(tp_android_vh_lru_cache_add_page_activate)
		tracepoint_probe_unregister(tp_android_vh_lru_cache_add_page_activate, lru_cache_add_page_activate, NULL);
}

static int mglru_proc_stat_show(struct seq_file *s, void *v)
{
	int i;
	unsigned long events[NR_DEBUG_EVENT_ITEMS];

	all_debug_events(events);

	seq_printf(s, "mglru_opt_enabled %d\n", mglru_opt_enabled() && lru_gen_enabled_dup());
	seq_printf(s, "lru_gen_enabled %d\n", lru_gen_enabled_dup());

	for (i = 0; i < NR_DEBUG_EVENT_ITEMS; i++)
		seq_printf(s, "%s %lu\n", debug_event_text[i], events[i]);

	return 0;
}

static void *get_symbol_address(const char *symbol_name) {
	struct kprobe kp = {
		.symbol_name = symbol_name
	};
	int ret;
	void *addr;

	ret = register_kprobe(&kp);
	if (ret) {
		pr_err("get %s addr from kprobe failed! ret=%d\n", symbol_name, ret);
		return NULL;
	}

	addr = (void *)kp.addr;

	unregister_kprobe(&kp);
	return addr;
}

#define GET_SYMBOL(symbol) \
	do { \
	symbol##_dup = (symbol##_t)get_symbol_address(#symbol); \
	if (!symbol##_dup) { \
		pr_err("Failed to get %s address!\n", #symbol); \
		return -1; \
	} \
	pr_debug("Successfully get %s addr: 0x%px\n", #symbol, symbol##_dup); \
	} while (0)

static __nocfi int get_all_symbol(void)
{
	GET_SYMBOL(kallsyms_lookup_name);

	tp_android_vh_lru_cache_add_page_activate =
		(struct tracepoint *)kallsyms_lookup_name_dup("__tracepoint_android_vh_lru_cache_add_page_activate");
	if (!tp_android_vh_lru_cache_add_page_activate) {
		pr_err("fail get __tracepoint_android_vh_lru_cache_add_page_activate\n");
		return -1;
	}
	pr_debug("suceesfully get __tracepoint_android_vh_lru_cache_add_page_activate addr:0x%px\n",
		tp_android_vh_lru_cache_add_page_activate);

	tp_android_vh_filemap_fault_pre_page_locked =
		(struct tracepoint *)kallsyms_lookup_name_dup("__tracepoint_android_vh_filemap_fault_pre_page_locked");
	if (!tp_android_vh_filemap_fault_pre_page_locked) {
		pr_err("fail get __tracepoint_android_vh_filemap_fault_pre_page_locked\n");
		return -1;
	}
	pr_debug("suceesfully get __tracepoint_android_vh_filemap_fault_pre_page_locked addr:0x%px\n",
		tp_android_vh_filemap_fault_pre_page_locked);

	tp_android_vh_filemap_page_mapped =
		(struct tracepoint *)kallsyms_lookup_name_dup("__tracepoint_android_vh_filemap_page_mapped");
	if (!tp_android_vh_filemap_page_mapped) {
		pr_err("fail get __tracepoint_android_vh_filemap_page_mapped\n");
		return -1;
	}
	pr_debug("suceesfully get __tracepoint_android_vh_filemap_page_mapped addr:0x%px\n",
		tp_android_vh_filemap_page_mapped);

	tp_android_vh_zap_pte_range_page_remove_rmap =
		(struct tracepoint *)kallsyms_lookup_name_dup("__tracepoint_android_vh_zap_pte_range_page_remove_rmap");
	if (!tp_android_vh_zap_pte_range_page_remove_rmap) {
		pr_err("fail get __tracepoint_android_vh_zap_pte_range_page_remove_rmap\n");
		return -1;
	}
	pr_debug("suceesfully get __tracepoint_android_vh_zap_pte_range_page_remove_rmap addr:0x%px\n",
		tp_android_vh_zap_pte_range_page_remove_rmap);

	p_lru_gen_caps = (struct static_key *)kallsyms_lookup_name_dup("lru_gen_caps");
	if (!p_lru_gen_caps) {
		pr_err("fail get lru_gen_caps\n");
		return -1;
	}
	pr_debug("suceesfully get lru_gen_caps addr:0x%px,lru_gen_enabled:%d\n", p_lru_gen_caps, lru_gen_enabled_dup());

	GET_SYMBOL(activate_page);
	GET_SYMBOL(deactivate_file_page);
	GET_SYMBOL(__lru_cache_activate_page);

	return 0;
}

static int __init mglru_opt_init(void)
{
	int ret = 0;
	struct proc_dir_entry *root_dir_entry;

#if IS_ENABLED(CONFIG_OPLUS_FEATURE_MM_OSVELTE)
	struct config_oplus_bsp_mglru_opt *config;

	/* check cmdline if rus disble */
	if (oplus_test_mm_feature_disable(COMFD1_MGLRU_OPT)) {
		pr_info("mglru_opt diabled by cmdline\n");
		return 0;
	}

	config = oplus_read_mm_config(module_name_mglru_opt);
	if (!config || !config->enable) {
		pr_info("%s is disabled in config\n", module_name_mglru_opt);
		return 0;
	}
#endif /* CONFIG_OPLUS_FEATURE_MM_OSVELTE */

	ret = get_all_symbol();
	if(ret != 0) {
		pr_err("Failed to get_all_symbol, disable mglru_opt\n");
		return ret;
	}

	/*Create debug entry*/
	root_dir_entry = proc_mkdir("oplus_mem", NULL);
	proc_create_single((root_dir_entry ?
				"mglru_opt_debug" : "oplus_mem/mglru_opt_debug"),
				0400, root_dir_entry, mglru_proc_stat_show);


	ret = register_mglru_opt_vendor_hooks();
	if (ret != 0)
		return ret;

	atomic_set(&mglru_opt_enable, true);
	pr_info("mglru_opt_init succeed!\n");
	return 0;
}

static void __exit mglru_opt_exit(void)
{
	unregister_mglru_opt_vendor_hooks();
	atomic_set(&mglru_opt_enable, false);
	pr_info("mglru_opt_exit succeed!\n");
}
#else
static int __init mglru_opt_init(void)
{
	pr_info("mglru_opt_init not support!\n");
	return 0;
}
static void __exit mglru_opt_exit(void)
{
}
#endif

module_init(mglru_opt_init);
module_exit(mglru_opt_exit);

MODULE_LICENSE("GPL v2");
