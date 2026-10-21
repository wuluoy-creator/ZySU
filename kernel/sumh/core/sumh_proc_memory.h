#ifndef _SUMH_PROC_MEMORY_H
#define _SUMH_PROC_MEMORY_H

#include <linux/types.h>

/* Requested SUMH allocation bytes, not allocator overhead or native VFS data.
 * Proxy objects are excluded until their ingress can deny admission safely.
 */
#define SUMH_PROC_MEMORY_BUDGET (16UL * 1024 * 1024)

enum sumh_proc_memory_kind {
	SUMH_PROC_MEMORY_SNAPSHOT,
	SUMH_PROC_MEMORY_STREAM,
	SUMH_PROC_MEMORY_WORKSPACE,
	SUMH_PROC_MEMORY_MAPS,
	SUMH_PROC_MEMORY_KINDS,
};

struct sumh_proc_memory_stats {
	size_t used;
	size_t peak;
	size_t by_kind[SUMH_PROC_MEMORY_KINDS];
	u64 denied;
	u64 allocation_failed;
};

/* Reserve before allocating; free before releasing. A replacement reserves
 * its full size while the old allocation remains charged. No lifecycle reset
 * is permitted while any opened proc file can still own an allocation.
 */
bool sumh_proc_memory_reserve(enum sumh_proc_memory_kind kind, size_t bytes);
void sumh_proc_memory_release(enum sumh_proc_memory_kind kind, size_t bytes);
void sumh_proc_memory_alloc_failed(enum sumh_proc_memory_kind kind);
void sumh_proc_memory_snapshot(struct sumh_proc_memory_stats *out);

#endif
