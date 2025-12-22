// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2020-2025 Oplus. All rights reserved.
 */

#ifndef _MGLRU_OPT_H
#define _MGLRU_OPT_H
#include <asm/setup.h>
/*FIXME:Temporarily modified to differentiate between platforms*/
#if IS_ENABLED(CONFIG_OPLUS_FEATURE_MGLRU_OPT)
enum debug_event_item {
	SUCCESS_COUNT,
	FAIL_COUNT,
	NR_DEBUG_EVENT_ITEMS
};

char *debug_event_text[NR_DEBUG_EVENT_ITEMS] = {
	"success_count",
	"fail_count",
};

struct debug_event_state {
	unsigned long event[NR_DEBUG_EVENT_ITEMS];
};
DEFINE_PER_CPU(struct debug_event_state, debug_event_states) = {{0}};

inline void count_debug_events(enum debug_event_item item, long delta)
{
	this_cpu_add(debug_event_states.event[item], delta);
}

inline void count_debug_event(enum debug_event_item item)
{
	count_debug_events(item, 1);
}

static void all_debug_events(unsigned long *ret)
{
	int cpu;
	int i;

	memset(ret, 0, NR_DEBUG_EVENT_ITEMS * sizeof(unsigned long));

	cpus_read_lock();
	for_each_online_cpu(cpu) {
		struct debug_event_state *this = &per_cpu(debug_event_states,
							   cpu);

		for (i = 0; i < NR_DEBUG_EVENT_ITEMS; i++)
			ret[i] += this->event[i];
	}
	cpus_read_unlock();
}
#endif
#endif /* _MGLRU_OPT_H */
