// SPDX-License-Identifier: MIT
/*
 * Copyright 2022 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 */

#ifndef _SIDE_RCU_H
#define _SIDE_RCU_H

#include <sched.h>
#include <stdint.h>
#include <pthread.h>
#include <stdbool.h>
#include <poll.h>
#include <rseq/rseq.h>
#include <linux/futex.h>
#include <sys/time.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <side/macros.h>

#define SIDE_CACHE_LINE_SIZE		256

struct side_rcu_percpu_count {
	uintptr_t begin;
	uintptr_t rseq_begin;
	uintptr_t end;
	uintptr_t rseq_end;
}  __attribute__((__aligned__(SIDE_CACHE_LINE_SIZE)));

struct side_rcu_cpu_gp_state {
	struct side_rcu_percpu_count count[2];
};

struct side_rcu_gp_state {
	struct side_rcu_cpu_gp_state *percpu_state;
	int nr_cpus;
	int32_t futex;
	unsigned int period;
	pthread_mutex_t gp_lock;
};

extern unsigned int side_rcu_rseq_membarrier_available __attribute__((visibility("hidden")));

static inline
int futex(int32_t *uaddr, int op, int32_t val,
	const struct timespec *timeout, int32_t *uaddr2, int32_t val3)
{
	return syscall(__NR_futex, uaddr, op, val, timeout, uaddr2, val3);
}

/*
 * Wake-up side_rcu_wait_grace_period. Called concurrently from many
 * threads.
 */
static inline
void side_rcu_wake_up_gp(struct side_rcu_gp_state *gp_state)
{
	if (side_unlikely(__atomic_load_n(&gp_state->futex, __ATOMIC_RELAXED) == -1)) {
		__atomic_store_n(&gp_state->futex, 0, __ATOMIC_RELAXED);
		/* TODO: handle futex return values. */
		(void) futex(&gp_state->futex, FUTEX_WAKE, 1, NULL, NULL, 0);
	}
}

static inline
unsigned int side_rcu_read_begin(struct side_rcu_gp_state *gp_state)
{
	unsigned int period = __atomic_load_n(&gp_state->period, __ATOMIC_RELAXED);
	struct side_rcu_cpu_gp_state *cpu_gp_state;
	int cpu;

	if (side_likely(side_rcu_rseq_membarrier_available)) {
		cpu = rseq_cpu_start();
		cpu_gp_state = &gp_state->percpu_state[cpu];
		if (side_likely(!rseq_addv((intptr_t *)&cpu_gp_state->count[period].rseq_begin, 1, cpu))) {
			/*
			 * This compiler barrier (A) is paired with membarrier() at (C),
			 * (D), (E). It effectively upgrades this compiler barrier to a
			 * SEQ_CST fence with respect to the paired barriers.
			 *
			 * This barrier (A) ensures that the contents of the read-side
			 * critical section does not leak before the "begin" counter
			 * increment. It pairs with memory barriers (D) and (E).
			 *
			 * This barrier (A) also ensures that the "begin" increment is
			 * before the "end" increment. It pairs with memory barrier (C).
			 * It is redundant with barrier (B) for that purpose.
			 */
			rseq_barrier();
			return period;
		}
	}
	/* Fallback to atomic increment and SEQ_CST. */
	cpu = sched_getcpu();
	if (side_unlikely(cpu < 0))
		cpu = 0;
	cpu_gp_state = &gp_state->percpu_state[cpu];
	(void) __atomic_add_fetch(&cpu_gp_state->count[period].begin, 1, __ATOMIC_SEQ_CST);
	return period;
}

static inline
void side_rcu_read_end(struct side_rcu_gp_state *gp_state, unsigned int period)
{
	struct side_rcu_cpu_gp_state *cpu_gp_state;
	int cpu;

	if (side_likely(side_rcu_rseq_membarrier_available)) {
		/*
		 * This compiler barrier (B) is paired with membarrier() at (C),
		 * (D), (E). It effectively upgrades this compiler barrier to a
		 * SEQ_CST fence with respect to the paired barriers.
		 *
		 * This barrier (B) ensures that the contents of the read-side
		 * critical section does not leak after the "end" counter
		 * increment. It pairs with memory barriers (D) and (E).
		 *
		 * This barrier (B) also ensures that the "begin" increment is
		 * before the "end" increment. It pairs with memory barrier (C).
		 * It is redundant with barrier (A) for that purpose.
		 */
		rseq_barrier();
		cpu = rseq_cpu_start();
		cpu_gp_state = &gp_state->percpu_state[cpu];
		if (side_likely(!rseq_addv((intptr_t *)&cpu_gp_state->count[period].rseq_end, 1, cpu))) {
			/*
			 * This barrier (F) is paired with membarrier()
			 * at (G). It orders increment of the begin/end
			 * counters before load/store to the futex.
			 */
			rseq_barrier();
			goto end;
		}
	}
	/* Fallback to atomic increment and SEQ_CST. */
	cpu = sched_getcpu();
	if (side_unlikely(cpu < 0))
		cpu = 0;
	cpu_gp_state = &gp_state->percpu_state[cpu];
	(void) __atomic_add_fetch(&cpu_gp_state->count[period].end, 1, __ATOMIC_SEQ_CST);
	/*
	 * This barrier (F) is paired with SEQ_CST barrier or
	 * membarrier() at (G). It orders increment of the begin/end
	 * counters before load/store to the futex.
	 */
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
end:
	side_rcu_wake_up_gp(gp_state);
}

#define side_rcu_dereference(p) \
	__extension__ \
	({ \
		__typeof__(p) _____side_v = __atomic_load_n(&(p), __ATOMIC_CONSUME); \
		(_____side_v); \
	})

#define side_rcu_assign_pointer(p, v)	__atomic_store_n(&(p), v, __ATOMIC_RELEASE); \

void side_rcu_wait_grace_period(struct side_rcu_gp_state *gp_state) __attribute__((visibility("hidden")));
void side_rcu_gp_init(struct side_rcu_gp_state *rcu_gp) __attribute__((visibility("hidden")));
void side_rcu_gp_exit(struct side_rcu_gp_state *rcu_gp) __attribute__((visibility("hidden")));

#endif /* _SIDE_RCU_H */
