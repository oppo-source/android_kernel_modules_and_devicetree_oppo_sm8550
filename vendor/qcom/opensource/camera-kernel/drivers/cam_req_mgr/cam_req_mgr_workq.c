// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2016-2021, The Linux Foundation. All rights reserved.
 */

#include "cam_req_mgr_workq.h"
#include "cam_debug_util.h"
#include "cam_common_util.h"
#ifdef OPLUS_FEATURE_CAMERA_COMMON
#include <linux/sched/types.h>
struct sched_param {
	int sched_priority;
};
#endif

#define WORKQ_ACQUIRE_LOCK(workq, flags) {\
	if ((workq)->in_irq) \
		spin_lock_irqsave(&(workq)->lock_bh, (flags)); \
	else \
		spin_lock_bh(&(workq)->lock_bh); \
}

#define WORKQ_RELEASE_LOCK(workq, flags) {\
	if ((workq)->in_irq) \
		spin_unlock_irqrestore(&(workq)->lock_bh, (flags)); \
	else	\
		spin_unlock_bh(&(workq)->lock_bh); \
}

#ifdef OPLUS_FEATURE_CAMERA_COMMON
static int cam_req_mgr_thread(void *data)
{
	struct cam_req_mgr_core_workq *workq = (struct cam_req_mgr_core_workq *)data;
	struct sched_param param = { .sched_priority = 1 };//prio=98
	sched_setscheduler_nocheck(current, SCHED_FIFO, &param);
	set_current_state(TASK_INTERRUPTIBLE);
	schedule();

	while(1)
	{
		set_current_state(TASK_INTERRUPTIBLE);
		if (!kthread_should_stop()) {
			cam_req_mgr_process_workq(&(workq->work));
			schedule();
		}
		set_current_state(TASK_RUNNING);
		if (kthread_should_stop())
			break;
		cam_req_mgr_process_workq(&(workq->work));
	}

	return 0;
}
#endif

struct crm_workq_task *cam_req_mgr_workq_get_task(
	struct cam_req_mgr_core_workq *workq)
{
	struct crm_workq_task *task = NULL;
	unsigned long flags = 0;

	if (!workq)
		return NULL;

	WORKQ_ACQUIRE_LOCK(workq, flags);
	if (list_empty(&workq->task.empty_head))
		goto end;

	task = list_first_entry(&workq->task.empty_head,
		struct crm_workq_task, entry);
	if (task) {
		atomic_sub(1, &workq->task.free_cnt);
		list_del_init(&task->entry);
	}

end:
	WORKQ_RELEASE_LOCK(workq, flags);

	return task;
}

static void cam_req_mgr_workq_put_task(struct crm_workq_task *task)
{
	struct cam_req_mgr_core_workq *workq =
		(struct cam_req_mgr_core_workq *)task->parent;
	unsigned long flags = 0;

	list_del_init(&task->entry);
	task->cancel = 0;
	task->process_cb = NULL;
	task->priv = NULL;
	WORKQ_ACQUIRE_LOCK(workq, flags);
	list_add_tail(&task->entry,
		&workq->task.empty_head);
	atomic_add(1, &workq->task.free_cnt);
	WORKQ_RELEASE_LOCK(workq, flags);
}

void cam_req_mgr_workq_flush(struct cam_req_mgr_core_workq *workq)
{
	if (!workq) {
		CAM_ERR(CAM_CRM, "workq is null");
		return;
	}

	atomic_set(&workq->flush, 1);
	cancel_work_sync(&workq->work);
	atomic_set(&workq->flush, 0);
}

/**
 * cam_req_mgr_process_task() - Process the enqueued task
 * @task: pointer to task workq thread shall process
 */
static int cam_req_mgr_process_task(struct crm_workq_task *task)
{
	struct cam_req_mgr_core_workq *workq = NULL;

	if (!task)
		return -EINVAL;

	workq = (struct cam_req_mgr_core_workq *)task->parent;
	if (task->process_cb)
		task->process_cb(task->priv, task->payload);
	else
		CAM_WARN(CAM_CRM, "FATAL:no task handler registered for workq");
	cam_req_mgr_workq_put_task(task);

	return 0;
}

/**
 * cam_req_mgr_process_workq() - main loop handling
 * @w: workqueue task pointer
 */
void cam_req_mgr_process_workq(struct work_struct *w)
{
	struct cam_req_mgr_core_workq *workq = NULL;
	struct crm_workq_task         *task;
	int32_t                        i = CRM_TASK_PRIORITY_0;
	unsigned long                  flags = 0;
	ktime_t                        sched_start_time;

	if (!w) {
		CAM_ERR(CAM_CRM, "NULL task pointer can not schedule");
		return;
	}
	workq = (struct cam_req_mgr_core_workq *)
		container_of(w, struct cam_req_mgr_core_workq, work);

	cam_common_util_thread_switch_delay_detect(
		"CRM workq schedule",
		workq->workq_scheduled_ts,
		CAM_WORKQ_SCHEDULE_TIME_THRESHOLD);
	sched_start_time = ktime_get();
	while (i < CRM_TASK_PRIORITY_MAX) {
		WORKQ_ACQUIRE_LOCK(workq, flags);
		while (!list_empty(&workq->task.process_head[i])) {
			task = list_first_entry(&workq->task.process_head[i],
				struct crm_workq_task, entry);
			atomic_sub(1, &workq->task.pending_cnt);
			list_del_init(&task->entry);
			WORKQ_RELEASE_LOCK(workq, flags);
			if (!unlikely(atomic_read(&workq->flush)))
				cam_req_mgr_process_task(task);
			CAM_DBG(CAM_CRM, "processed task %pK free_cnt %d",
				task, atomic_read(&workq->task.free_cnt));
			WORKQ_ACQUIRE_LOCK(workq, flags);
		}
		WORKQ_RELEASE_LOCK(workq, flags);
		i++;
	}
	cam_common_util_thread_switch_delay_detect(
		"CRM workq execution",
		sched_start_time,
		CAM_WORKQ_EXE_TIME_THRESHOLD);
}

int cam_req_mgr_workq_enqueue_task(struct crm_workq_task *task,
	void *priv, int32_t prio)
{
	int rc = 0;
	struct cam_req_mgr_core_workq *workq = NULL;
	unsigned long flags = 0;

	if (!task) {
		CAM_WARN(CAM_CRM, "NULL task pointer can not schedule");
		return -EINVAL;
	}
	workq = (struct cam_req_mgr_core_workq *)task->parent;
	if (!workq) {
		CAM_DBG(CAM_CRM, "NULL workq pointer suspect mem corruption");
		return -EINVAL;
	}

	if (task->cancel == 1 || atomic_read(&workq->flush)) {
		rc = 0;
		goto abort;
	}
	task->priv = priv;
	task->priority =
		(prio < CRM_TASK_PRIORITY_MAX && prio >= CRM_TASK_PRIORITY_0)
		? prio : CRM_TASK_PRIORITY_0;

	WORKQ_ACQUIRE_LOCK(workq, flags);
	if (!workq->job) {
		rc = -EINVAL;
		WORKQ_RELEASE_LOCK(workq, flags);
		goto abort;
	}

	list_add_tail(&task->entry,
		&workq->task.process_head[task->priority]);

	atomic_add(1, &workq->task.pending_cnt);
	CAM_DBG(CAM_CRM, "enq task %pK pending_cnt %d",
		task, atomic_read(&workq->task.pending_cnt));

	workq->workq_scheduled_ts = ktime_get();
#ifdef OPLUS_FEATURE_CAMERA_COMMON
	if (workq->thread) {
		wake_up_process(workq->thread);
	} else {
		queue_work(workq->job, &workq->work);
	}
#else
		queue_work(workq->job, &workq->work);
#endif
	WORKQ_RELEASE_LOCK(workq, flags);

	return rc;
abort:
	cam_req_mgr_workq_put_task(task);
	CAM_INFO(CAM_CRM, "task aborted and queued back to pool");
	return rc;
}

int cam_req_mgr_workq_create(char *name, int32_t num_tasks,
	struct cam_req_mgr_core_workq **workq, enum crm_workq_context in_irq,
	int flags, void (*func)(struct work_struct *w))
{
	int32_t i, wq_flags = 0, max_active_tasks = 0;
	struct crm_workq_task  *task;
	struct cam_req_mgr_core_workq *crm_workq = NULL;
	char buf[128] = "crm_workq-";

	if (!*workq) {
		crm_workq = kzalloc(sizeof(struct cam_req_mgr_core_workq),
			GFP_KERNEL);
		if (crm_workq == NULL)
			return -ENOMEM;

		wq_flags |= WQ_UNBOUND;
		if (flags & CAM_WORKQ_FLAG_HIGH_PRIORITY)
			wq_flags |= WQ_HIGHPRI;

		if (flags & CAM_WORKQ_FLAG_SERIAL)
			max_active_tasks = 1;

		strlcat(buf, name, sizeof(buf));
		CAM_DBG(CAM_CRM, "create workque crm_workq-%s", name);
		crm_workq->job = alloc_workqueue(buf,
			wq_flags, max_active_tasks, NULL);
		if (!crm_workq->job) {
			kfree(crm_workq);
			return -ENOMEM;
		}

		/* Workq attributes initialization */
		strlcpy(crm_workq->workq_name, buf, sizeof(crm_workq->workq_name));
		INIT_WORK(&crm_workq->work, func);
		spin_lock_init(&crm_workq->lock_bh);
		CAM_DBG(CAM_CRM, "LOCK_DBG workq %s lock %pK",
			name, &crm_workq->lock_bh);
#ifdef OPLUS_FEATURE_CAMERA_COMMON
		mutex_init(&crm_workq->rt_lock);
		if (strstr(crm_workq->workq_name, "CRMCORE")) {
			mutex_lock(&crm_workq->rt_lock);
			crm_workq->thread = kthread_run(cam_req_mgr_thread, crm_workq, "%s",
				crm_workq->workq_name);
			CAM_INFO(CAM_CRM, "create workqueue thread crm_workq->thread %p", crm_workq->thread);
			mutex_unlock(&crm_workq->rt_lock);
			if (!crm_workq->thread) {
				CAM_ERR(CAM_CRM, "create workqueue thread failed: %s", crm_workq->workq_name);
			}
		} else {
			mutex_lock(&crm_workq->rt_lock);
			crm_workq->thread = NULL;
			mutex_unlock(&crm_workq->rt_lock);
		}
#endif
		/* Task attributes initialization */
		atomic_set(&crm_workq->task.pending_cnt, 0);
		atomic_set(&crm_workq->task.free_cnt, 0);
		for (i = CRM_TASK_PRIORITY_0; i < CRM_TASK_PRIORITY_MAX; i++)
			INIT_LIST_HEAD(&crm_workq->task.process_head[i]);
		INIT_LIST_HEAD(&crm_workq->task.empty_head);
		atomic_set(&crm_workq->flush, 0);
		crm_workq->in_irq = in_irq;
		crm_workq->task.num_task = num_tasks;
		crm_workq->task.pool = kcalloc(crm_workq->task.num_task,
				sizeof(struct crm_workq_task), GFP_KERNEL);
		if (!crm_workq->task.pool) {
			CAM_WARN(CAM_CRM, "Insufficient memory %zu",
				sizeof(struct crm_workq_task) *
				crm_workq->task.num_task);
			kfree(crm_workq);
			return -ENOMEM;
		}

		for (i = 0; i < crm_workq->task.num_task; i++) {
			task = &crm_workq->task.pool[i];
			task->parent = (void *)crm_workq;
			/* Put all tasks in free pool */
			INIT_LIST_HEAD(&task->entry);
			cam_req_mgr_workq_put_task(task);
		}
		*workq = crm_workq;
		CAM_DBG(CAM_CRM, "free tasks %d",
			atomic_read(&crm_workq->task.free_cnt));
	}

	return 0;
}

void cam_req_mgr_workq_destroy(struct cam_req_mgr_core_workq **crm_workq)
{
	unsigned long flags = 0;
	struct workqueue_struct   *job;
	struct cam_req_mgr_core_workq *workq;
	int i;

	if (crm_workq && *crm_workq) {
		workq = *crm_workq;
		CAM_DBG(CAM_CRM, "destroy workque %s", workq->workq_name);
#ifdef OPLUS_FEATURE_CAMERA_COMMON
		if (workq->thread) {
			mutex_lock(&workq->rt_lock);
			CAM_INFO(CAM_CRM, "stop workqueue thread workq->thread %p", workq->thread);
			kthread_stop(workq->thread);
			workq->thread = NULL;
			mutex_unlock(&workq->rt_lock);
		}
#endif
		WORKQ_ACQUIRE_LOCK(workq, flags);
		/* prevent any processing of callbacks */
		atomic_set(&workq->flush, 1);
		if (workq->job) {
			job = workq->job;
			workq->job = NULL;
			WORKQ_RELEASE_LOCK(workq, flags);
			destroy_workqueue(job);
			WORKQ_ACQUIRE_LOCK(workq, flags);
		}
		/* Destroy workq payload data */
		kfree(workq->task.pool[0].payload);
		kfree(workq->task.pool);

		/* Leave lists in stable state after freeing pool */
		INIT_LIST_HEAD(&workq->task.empty_head);
		for (i = 0; i < CRM_TASK_PRIORITY_MAX; i++)
			INIT_LIST_HEAD(&workq->task.process_head[i]);
		*crm_workq = NULL;
		WORKQ_RELEASE_LOCK(workq, flags);
		kfree(workq);
	}
}
