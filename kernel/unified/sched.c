/*
 * Copyright (c) 2016 Wind River Systems, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <kernel.h>
#include <nano_private.h>
#include <atomic.h>
#include <sched.h>
#include <wait_q.h>

/* set the bit corresponding to prio in ready q bitmap */
static void _set_ready_q_prio_bit(int prio)
{
	int bmap_index = _get_ready_q_prio_bmap_index(prio);
	uint32_t *bmap = &_nanokernel.ready_q.prio_bmap[bmap_index];

	*bmap |= _get_ready_q_prio_bit(prio);
}

/* clear the bit corresponding to prio in ready q bitmap */
static void _clear_ready_q_prio_bit(int prio)
{
	int bmap_index = _get_ready_q_prio_bmap_index(prio);
	uint32_t *bmap = &_nanokernel.ready_q.prio_bmap[bmap_index];

	*bmap &= ~_get_ready_q_prio_bit(prio);
}

/*
 * Add thread to the ready queue, in the slot for its priority; the thread
 * must not be on a wait queue.
 */
void _add_thread_to_ready_q(struct tcs *thread)
{
	int q_index = _get_ready_q_q_index(thread->prio);
	sys_dlist_t *q = &_nanokernel.ready_q.q[q_index];

	_set_ready_q_prio_bit(thread->prio);
	sys_dlist_append(q, &thread->k_q_node);
}

/* remove thread from the ready queue */
void _remove_thread_from_ready_q(struct tcs *thread)
{
	int q_index = _get_ready_q_q_index(thread->prio);
	sys_dlist_t *q = &_nanokernel.ready_q.q[q_index];

	sys_dlist_remove(&thread->k_q_node);
	if (sys_dlist_is_empty(q)) {
		_clear_ready_q_prio_bit(thread->prio);
	}
}

/* reschedule threads if the scheduler is not locked */
/* not callable from ISR */
/* must be called with interrupts locked */
void _reschedule_threads(int key)
{
	K_DEBUG("rescheduling threads\n");

	if (unlikely(_nanokernel.current->sched_locked > 0)) {
		K_DEBUG("aborted: scheduler was locked\n");
		irq_unlock(key);
		return;
	}

	if (_must_switch_threads()) {
		K_DEBUG("context-switching out %p\n", _current);
		_Swap(key);
	} else {
		irq_unlock(key);
	}
}

/* application API: lock the scheduler */
void k_sched_unlock(void)
{
	__ASSERT(_nanokernel.current->sched_locked > 0, "");
	__ASSERT(!_is_in_isr(), "");

	int key = irq_lock();

	atomic_dec(&_nanokernel.current->sched_locked);

	K_DEBUG("scheduler unlocked (%p:%d)\n",
		_current, _current->sched_locked);

	_reschedule_threads(key);
}

/*
 * Callback for sys_dlist_insert_at() to find the correct insert point in a
 * wait queue (priority-based).
 */
static int _is_wait_q_insert_point(sys_dnode_t *dnode_info, void *insert_prio)
{
	struct tcs *waitq_node = CONTAINER_OF(dnode_info, struct tcs, k_q_node);

	return _is_prio_higher((int)insert_prio, waitq_node->prio);
}

/* convert milliseconds to ticks */

#define ceiling(numerator, divider) \
	(((numerator) + ((divider) - 1)) / (divider))

int32_t _ms_to_ticks(int32_t ms)
{
	int64_t ms_ticks_per_sec = (int64_t)ms * sys_clock_ticks_per_sec;

	return (int32_t)ceiling(ms_ticks_per_sec, MSEC_PER_SEC);
}

/* pend the specified thread: it must *not* be in the ready queue */
/* must be called with interrupts locked */
void _pend_thread(struct tcs *thread, _wait_q_t *wait_q, int32_t timeout)
{
	sys_dlist_t *dlist = (sys_dlist_t *)wait_q;

	sys_dlist_insert_at(dlist, &thread->k_q_node,
			    _is_wait_q_insert_point, (void *)thread->prio);

	_mark_thread_as_pending(thread);

	if (timeout != K_FOREVER) {
		_mark_thread_as_timing(thread);
		_TIMEOUT_ADD(thread, wait_q, _ms_to_ticks(timeout));
	}
}

/* pend the current thread */
/* must be called with interrupts locked */
void _pend_current_thread(_wait_q_t *wait_q, int32_t timeout)
{
	_remove_thread_from_ready_q(_current);
	_pend_thread(_current, wait_q, timeout);
}

/* find which one is the next thread to run */
/* must be called with interrupts locked */
struct tcs *_get_next_ready_thread(void)
{
	int prio = _get_highest_ready_prio();
	int q_index = _get_ready_q_q_index(prio);
	sys_dlist_t *list = &_nanokernel.ready_q.q[q_index];
	struct k_thread *thread = (struct k_thread *)sys_dlist_peek_head(list);

	__ASSERT(thread, "no thread to run (prio: %d, queue index: %u)!\n",
		 prio, q_index);

	return thread;
}

/*
 * Check if there is a thread of higher prio than the current one. Should only
 * be called if we already know that the current thread is preemptible.
 */
int __must_switch_threads(void)
{
	K_DEBUG("current prio: %d, highest prio: %d\n",
		_current->prio, _get_highest_ready_prio());

	extern void _dump_ready_q(void);
	_dump_ready_q();

	return _is_prio_higher(_get_highest_ready_prio(), _current->prio);
}

/* application API: change a thread's priority. Not callable from ISR */
void k_thread_priority_set(struct tcs *thread, int prio)
{
	__ASSERT(!_is_in_isr(), "");

	int key = irq_lock();

	_thread_priority_set(thread, prio);
	_reschedule_threads(key);
}

/* application API: find out the priority of the current thread */
int k_current_priority_get(void)
{
	return k_thread_priority_get(_current);
}

/*
 * application API: the current thread yields control to threads of higher or
 * equal priorities. This is done by remove the thread from the ready queue,
 * putting it back at the end of its priority's list and invoking the
 * scheduler.
 */
void k_yield(void)
{
	__ASSERT(!_is_in_isr(), "");

	int key = irq_lock();

	_remove_thread_from_ready_q(_current);
	_add_thread_to_ready_q(_current);

	if (_current == _get_next_ready_thread()) {
		irq_unlock(key);
	} else {
		_Swap(key);
	}
}

/* application API: put the current thread to sleep */
void k_sleep(int32_t duration)
{
	__ASSERT(!_is_in_isr(), "");

	K_DEBUG("thread %p for %d ns\n", _current, duration);

	/* wait of 0 ns is treated as a 'yield' */
	if (duration == 0) {
		k_yield();
		return;
	}

	int key = irq_lock();

	_mark_thread_as_timing(_current);
	_remove_thread_from_ready_q(_current);
	_timeout_add(_current, NULL, _ms_to_ticks(duration));

	_Swap(key);
}

/* application API: wakeup a sleeping thread */
void k_wakeup(k_tid_t thread)
{
	int key = irq_lock();

	/* verify first if thread is not waiting on an object */
	if (thread->timeout.wait_q) {
		irq_unlock(key);
		return;
	}

	if (_timeout_abort(thread) < 0) {
		irq_unlock(key);
		return;
	}

	_ready_thread(thread);

	if (_is_in_isr()) {
		irq_unlock(key);
	} else {
		_reschedule_threads(key);
	}
}

/* application API: get current thread ID */
k_tid_t k_current_get(void)
{
	return _current;
}

/* debug aid */
void _dump_ready_q(void)
{
	K_DEBUG("bitmap: %x\n", _ready_q.prio_bmap[0]);
	for (int prio = 0; prio < K_NUM_PRIORITIES; prio++) {
		K_DEBUG("prio: %d, head: %p\n",
			prio - CONFIG_NUM_COOP_PRIORITIES,
			sys_dlist_peek_head(&_ready_q.q[prio]));
	}
}
