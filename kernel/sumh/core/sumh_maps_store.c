#include "sumh_maps_store.h"

#include <linux/errno.h>
#include <linux/refcount.h>
#include <linux/slab.h>
#include <linux/sort.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/vmalloc.h>

#include "sumh_proc_memory.h"

struct sumh_maps_snapshot {
	refcount_t refs;
	unsigned int count;
	unsigned int temporary_count;
	unsigned int managed_count;
	size_t bytes;
	struct sumh_maps_rule_entry entries[];
};

/* Leaf lock: only pointer/refcount acquisition and publication. Allocation,
 * final put/free, sorting and memory-budget accounting happen outside it.
 */
static DEFINE_SPINLOCK(sumh_maps_snapshot_lock);
static struct sumh_maps_snapshot *sumh_maps_current;

struct sumh_maps_snapshot *sumh_maps_snapshot_get(void)
{
	struct sumh_maps_snapshot *snapshot;
	unsigned long flags;

	spin_lock_irqsave(&sumh_maps_snapshot_lock, flags);
	snapshot = sumh_maps_current;
	if (snapshot)
		refcount_inc(&snapshot->refs);
	spin_unlock_irqrestore(&sumh_maps_snapshot_lock, flags);
	return snapshot;
}

void sumh_maps_snapshot_put(struct sumh_maps_snapshot *snapshot)
{
	size_t bytes;

	if (!snapshot || !refcount_dec_and_test(&snapshot->refs))
		return;
	bytes = snapshot->bytes;
	kvfree(snapshot);
	sumh_proc_memory_release(SUMH_PROC_MEMORY_MAPS, bytes);
}

unsigned int sumh_maps_snapshot_count(const struct sumh_maps_snapshot *snapshot)
{
	return snapshot ? snapshot->count : 0;
}

const struct sumh_maps_rule_entry *
sumh_maps_snapshot_entry(const struct sumh_maps_snapshot *snapshot,
			 unsigned int index)
{
	return snapshot && index < snapshot->count ? &snapshot->entries[index]
						   : NULL;
}

static const struct sumh_maps_rule_entry *
sumh_maps_snapshot_find_key(const struct sumh_maps_snapshot *snapshot,
			    unsigned long ino, unsigned long dev)
{
	unsigned int first = 0, end;

	if (!snapshot)
		return NULL;
	end = snapshot->count;
	while (first < end) {
		unsigned int middle = first + (end - first) / 2;
		const struct sumh_maps_rule_entry *entry =
		    &snapshot->entries[middle];

		if (entry->target_ino < ino ||
		    (entry->target_ino == ino && entry->target_dev < dev))
			first = middle + 1;
		else
			end = middle;
	}
	if (first == snapshot->count ||
	    snapshot->entries[first].target_ino != ino ||
	    snapshot->entries[first].target_dev != dev)
		return NULL;
	/* Sorting puts temporary before managed for this exact key. */
	return &snapshot->entries[first];
}

const struct sumh_maps_rule_entry *
sumh_maps_snapshot_lookup(const struct sumh_maps_snapshot *snapshot,
			  unsigned long ino, unsigned long dev)
{
	const struct sumh_maps_rule_entry *entry;

	entry = sumh_maps_snapshot_find_key(snapshot, ino, dev);
	if (entry || !dev)
		return entry;
	return sumh_maps_snapshot_find_key(snapshot, ino, 0);
}

static int sumh_maps_entry_compare(const void *left, const void *right)
{
	const struct sumh_maps_rule_entry *a = left;
	const struct sumh_maps_rule_entry *b = right;

	if (a->target_ino != b->target_ino)
		return a->target_ino < b->target_ino ? -1 : 1;
	if (a->target_dev != b->target_dev)
		return a->target_dev < b->target_dev ? -1 : 1;
	if (a->managed != b->managed)
		return a->managed ? 1 : -1;
	return 0;
}

static int sumh_maps_rule_validate(const struct sumh_maps_rule *rule)
{
	size_t len;

	if (!rule)
		return -EINVAL;
	len = strnlen(rule->spoofed_pathname, sizeof(rule->spoofed_pathname));
	if (rule->err || rule->target_dev > 0xffffffffULL ||
	    rule->spoofed_dev > 0xffffffffULL ||
	    len == sizeof(rule->spoofed_pathname) ||
	    memchr(rule->spoofed_pathname, '\n', len) ||
	    memchr(rule->spoofed_pathname, '\r', len))
		return -EINVAL;
	return 0;
}

static void sumh_maps_entry_from_rule(struct sumh_maps_rule_entry *entry,
				      const struct sumh_maps_rule *rule,
				      bool managed)
{
	memset(entry, 0, sizeof(*entry));
	entry->managed = managed;
	entry->target_ino = rule->target_ino;
	entry->target_dev = rule->target_dev;
	entry->spoofed_ino = rule->spoofed_ino;
	entry->spoofed_dev = rule->spoofed_dev;
	strscpy(entry->spoofed_pathname, rule->spoofed_pathname,
		sizeof(entry->spoofed_pathname));
}

static int sumh_maps_snapshot_alloc(unsigned int temporary_count,
				    unsigned int managed_count,
				    struct sumh_maps_snapshot **out)
{
	struct sumh_maps_snapshot *snapshot;
	unsigned int count;
	size_t bytes;

	*out = NULL;
	if (temporary_count > SUMH_TEMPORARY_MAPS_MAX_RULES ||
	    managed_count > SUMH_MANAGED_MAPS_MAX_RULES)
		return -ENOSPC;
	count = temporary_count + managed_count;
	if (!count)
		return 0;
	/* Both counts are bounded before the size calculation. Charge the
	 * whole immutable allocation, including generations pinned by FDs.
	 */
	bytes =
	    sizeof(*snapshot) + (size_t)count * sizeof(snapshot->entries[0]);
	if (!sumh_proc_memory_reserve(SUMH_PROC_MEMORY_MAPS, bytes))
		return -ENOMEM;
	snapshot = kvmalloc(bytes, GFP_KERNEL);
	if (!snapshot) {
		sumh_proc_memory_alloc_failed(SUMH_PROC_MEMORY_MAPS);
		sumh_proc_memory_release(SUMH_PROC_MEMORY_MAPS, bytes);
		return -ENOMEM;
	}
	refcount_set(&snapshot->refs, 1);
	snapshot->count = count;
	snapshot->temporary_count = temporary_count;
	snapshot->managed_count = managed_count;
	snapshot->bytes = bytes;
	*out = snapshot;
	return 0;
}

int sumh_maps_prepare_add(const struct sumh_maps_rule *rule,
			  struct sumh_maps_snapshot **out)
{
	struct sumh_maps_snapshot *previous, *prepared;
	const struct sumh_maps_rule_entry *existing;
	unsigned int temporary_count, managed_count, index;
	int ret;

	if (!out)
		return -EINVAL;
	*out = NULL;
	ret = sumh_maps_rule_validate(rule);
	if (ret)
		return ret;
	previous = sumh_maps_snapshot_get();
	existing = sumh_maps_snapshot_find_key(previous, rule->target_ino,
					       rule->target_dev);
	if (existing && existing->managed)
		existing = NULL;
	temporary_count = previous ? previous->temporary_count : 0;
	managed_count = previous ? previous->managed_count : 0;
	ret = sumh_maps_snapshot_alloc(temporary_count + !existing,
				       managed_count, &prepared);
	if (ret)
		goto out_current;
	if (previous)
		memcpy(prepared->entries, previous->entries,
		       previous->count * sizeof(previous->entries[0]));
	index = existing ? (unsigned int)(existing - previous->entries)
			 : sumh_maps_snapshot_count(previous);
	sumh_maps_entry_from_rule(&prepared->entries[index], rule, false);
	sort(prepared->entries, prepared->count, sizeof(prepared->entries[0]),
	     sumh_maps_entry_compare, NULL);
	*out = prepared;
out_current:
	sumh_maps_snapshot_put(previous);
	return ret;
}

int sumh_maps_prepare_delete(unsigned long ino, unsigned long dev,
			     struct sumh_maps_snapshot **out)
{
	struct sumh_maps_snapshot *previous, *prepared;
	const struct sumh_maps_rule_entry *existing;
	unsigned int index;
	int ret;

	if (!out)
		return -EINVAL;
	*out = NULL;
	if (dev > 0xffffffffULL)
		return -EINVAL;
	previous = sumh_maps_snapshot_get();
	/* A display lookup may fall back to dev=0; deletion must not. Sorting
	 * places the temporary entry first when both owners share this key. */
	existing = sumh_maps_snapshot_find_key(previous, ino, dev);
	if (!existing || existing->managed) {
		ret = -ENOENT;
		goto out_current;
	}
	ret = sumh_maps_snapshot_alloc(previous->temporary_count - 1,
				       previous->managed_count, &prepared);
	if (ret)
		goto out_current;
	if (prepared) {
		index = (unsigned int)(existing - previous->entries);
		memcpy(prepared->entries, previous->entries,
		       index * sizeof(previous->entries[0]));
		memcpy(prepared->entries + index, previous->entries + index + 1,
		       (previous->count - index - 1) *
			   sizeof(previous->entries[0]));
	}
	/* Removing one entry preserves the order and every other owner. A
	 * NULL prepared collection is the successful last-rule deletion. */
	*out = prepared;
out_current:
	sumh_maps_snapshot_put(previous);
	return ret;
}

int sumh_maps_prepare_managed(const struct sumh_maps_rule *rules,
			      unsigned int count,
			      struct sumh_maps_snapshot **out)
{
	struct sumh_maps_snapshot *previous, *prepared;
	unsigned int i, j, next = 0;
	int ret;

	if (!out)
		return -EINVAL;
	*out = NULL;
	if (count > SUMH_MANAGED_MAPS_MAX_RULES || (count && !rules))
		return -EINVAL;
	for (i = 0; i < count; i++) {
		if (!rules[i].target_ino)
			return -EINVAL;
		ret = sumh_maps_rule_validate(&rules[i]);
		if (ret)
			return ret;
		for (j = 0; j < i; j++) {
			if (rules[j].target_ino == rules[i].target_ino &&
			    rules[j].target_dev == rules[i].target_dev)
				return -EINVAL;
		}
	}
	previous = sumh_maps_snapshot_get();
	ret = sumh_maps_snapshot_alloc(previous ? previous->temporary_count : 0,
				       count, &prepared);
	if (ret)
		goto out_current;
	if (previous) {
		for (i = 0; i < previous->count; i++) {
			if (!previous->entries[i].managed)
				prepared->entries[next++] =
				    previous->entries[i];
		}
	}
	for (i = 0; i < count; i++)
		sumh_maps_entry_from_rule(&prepared->entries[next++], &rules[i],
					  true);
	if (prepared)
		sort(prepared->entries, prepared->count,
		     sizeof(prepared->entries[0]), sumh_maps_entry_compare,
		     NULL);
	*out = prepared;
out_current:
	sumh_maps_snapshot_put(previous);
	return ret;
}

void sumh_maps_publish(struct sumh_maps_snapshot *snapshot)
{
	struct sumh_maps_snapshot *old;
	unsigned long flags;

	spin_lock_irqsave(&sumh_maps_snapshot_lock, flags);
	old = sumh_maps_current;
	sumh_maps_current = snapshot;
	spin_unlock_irqrestore(&sumh_maps_snapshot_lock, flags);
	sumh_maps_snapshot_put(old);
}

void sumh_maps_store_clear(void)
{
	sumh_maps_publish(NULL);
}
