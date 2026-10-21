#ifndef _SUMH_MAPS_STORE_H
#define _SUMH_MAPS_STORE_H

#include <linux/types.h>

#include "sumh_uapi.h"

/* Managed capacity is reserved independently of temporary rules. */
#define SUMH_TEMPORARY_MAPS_MAX_RULES 1024U
#define SUMH_MAPS_MAX_RULES                                                    \
	(SUMH_TEMPORARY_MAPS_MAX_RULES + SUMH_MANAGED_MAPS_MAX_RULES)

/* Immutable after publication. Both owners remain present for inventory. */
struct sumh_maps_rule_entry {
	bool managed;
	unsigned long target_ino;
	unsigned long target_dev;
	unsigned long spoofed_ino;
	unsigned long spoofed_dev;
	char spoofed_pathname[SUMH_MAX_LEN_PATHNAME];
};

struct sumh_maps_snapshot;

/* NULL is a valid, pinned empty collection. A stream must remember that it
 * acquired even when NULL was returned. Accessors require a retained pin;
 * all entries and returned pointers remain immutable until the final put.
 * put may free vmalloc storage and must run in sleepable context.
 */
struct sumh_maps_snapshot *sumh_maps_snapshot_get(void);
void sumh_maps_snapshot_put(struct sumh_maps_snapshot *snapshot);
unsigned int
sumh_maps_snapshot_count(const struct sumh_maps_snapshot *snapshot);
const struct sumh_maps_rule_entry *
sumh_maps_snapshot_entry(const struct sumh_maps_snapshot *snapshot,
			 unsigned int index);
const struct sumh_maps_rule_entry *
sumh_maps_snapshot_lookup(const struct sumh_maps_snapshot *snapshot,
			  unsigned long ino, unsigned long dev);

/* Writers must hold the control mutation/lifecycle boundary continuously
 * from preparation through publication. Shutdown clear uses the exclusive
 * lifecycle boundary. Preparation never modifies the current collection;
 * on failure *out is NULL. A successful NULL result is an empty collection.
 * Publish consumes the prepared reference and cannot fail. All these writer
 * operations, including discarding a prepared reference with put, are
 * sleepable. Snapshot acquisition/publication never frees while locked.
 */
int sumh_maps_prepare_add(const struct sumh_maps_rule *rule,
			  struct sumh_maps_snapshot **out);
/* Exact temporary key only: no wildcard fallback or managed deletion. */
int sumh_maps_prepare_delete(unsigned long ino, unsigned long dev,
			     struct sumh_maps_snapshot **out);
int sumh_maps_prepare_managed(const struct sumh_maps_rule *rules,
			      unsigned int count,
			      struct sumh_maps_snapshot **out);
void sumh_maps_publish(struct sumh_maps_snapshot *snapshot);
void sumh_maps_store_clear(void);

#endif
