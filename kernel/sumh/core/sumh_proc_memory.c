#include "sumh_proc_memory.h"

#include <linux/bug.h>
#include <linux/spinlock.h>

/* Leaf lock: no allocation, freeing, logging or native callbacks while held. */
static DEFINE_SPINLOCK(sumh_proc_memory_lock);
static struct sumh_proc_memory_stats sumh_proc_memory_state;

bool sumh_proc_memory_reserve(enum sumh_proc_memory_kind kind, size_t bytes)
{
	unsigned long flags;
	bool reserved = false;

	if ((unsigned int)kind >= SUMH_PROC_MEMORY_KINDS)
		return false;
	spin_lock_irqsave(&sumh_proc_memory_lock, flags);
	if (bytes > SUMH_PROC_MEMORY_BUDGET - sumh_proc_memory_state.used) {
		sumh_proc_memory_state.denied++;
	} else {
		sumh_proc_memory_state.used += bytes;
		sumh_proc_memory_state.by_kind[kind] += bytes;
		if (sumh_proc_memory_state.used > sumh_proc_memory_state.peak)
			sumh_proc_memory_state.peak =
			    sumh_proc_memory_state.used;
		reserved = true;
	}
	spin_unlock_irqrestore(&sumh_proc_memory_lock, flags);
	return reserved;
}

void sumh_proc_memory_release(enum sumh_proc_memory_kind kind, size_t bytes)
{
	unsigned long flags;
	bool invalid;

	if (WARN_ON_ONCE((unsigned int)kind >= SUMH_PROC_MEMORY_KINDS))
		return;
	spin_lock_irqsave(&sumh_proc_memory_lock, flags);
	invalid = bytes > sumh_proc_memory_state.by_kind[kind];
	if (!invalid) {
		sumh_proc_memory_state.by_kind[kind] -= bytes;
		sumh_proc_memory_state.used -= bytes;
	}
	spin_unlock_irqrestore(&sumh_proc_memory_lock, flags);
	WARN_ON_ONCE(invalid);
}

void sumh_proc_memory_alloc_failed(enum sumh_proc_memory_kind kind)
{
	unsigned long flags;

	if (WARN_ON_ONCE((unsigned int)kind >= SUMH_PROC_MEMORY_KINDS))
		return;
	spin_lock_irqsave(&sumh_proc_memory_lock, flags);
	sumh_proc_memory_state.allocation_failed++;
	spin_unlock_irqrestore(&sumh_proc_memory_lock, flags);
}

void sumh_proc_memory_snapshot(struct sumh_proc_memory_stats *out)
{
	unsigned long flags;

	spin_lock_irqsave(&sumh_proc_memory_lock, flags);
	*out = sumh_proc_memory_state;
	spin_unlock_irqrestore(&sumh_proc_memory_lock, flags);
}
