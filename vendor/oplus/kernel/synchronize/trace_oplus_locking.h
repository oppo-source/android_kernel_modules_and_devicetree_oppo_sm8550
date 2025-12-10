/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2024 Oplus. All rights reserved.
 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM oplus_locking

#if !defined(_TRACE_OPLUS_LOCKING_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_OPLUS_LOCKING_H

#include <linux/sched.h>
#include <linux/types.h>
#include <linux/tracepoint.h>

TRACE_EVENT(reader_opt_rspin_result,

	TP_PROTO(struct task_struct *p, bool result),

	TP_ARGS(p, result),

	TP_STRUCT__entry(
		__field(int,		pid)
		__array(char,		comm, TASK_COMM_LEN)
		__field(bool,		res)),

	TP_fast_assign(
		__entry->pid			= p->pid;
		memcpy(__entry->comm, p->comm, TASK_COMM_LEN);
		__entry->res			= result;),

	TP_printk("pid=%d comm=%s rspin=%d",
		__entry->pid, __entry->comm, __entry->res)
);

#endif /*_TRACE_OPLUS_LOCKING_H */

#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .

#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE trace_oplus_locking
/* This part must be outside protection */
#include <trace/define_trace.h>
