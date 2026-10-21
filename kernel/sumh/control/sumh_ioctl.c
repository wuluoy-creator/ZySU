#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kallsyms.h>
#include <linux/version.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0) &&                           \
    !defined(arch_ftrace_get_regs)
#define arch_ftrace_get_regs(fregs) (NULL)
#endif
#include <linux/kprobes.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/jhash.h>
#include <linux/kdev_t.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/fdtable.h>
#include <linux/namei.h>
#include <linux/path.h>
#include <linux/uaccess.h>
#include <linux/cred.h>
#include <linux/uidgid.h>
#include <linux/sched/task.h>
#include <linux/fs_struct.h>
#include <linux/dirent.h>
#include <linux/stat.h>
#include <linux/time.h>
#include <linux/anon_inodes.h>
#include <linux/fcntl.h>
#include <linux/percpu.h>
#include <linux/smp.h>
#include <linux/mount.h>
#include <linux/xattr.h>
#include <linux/seq_file.h>
#include <linux/random.h>
#include <uapi/linux/magic.h>
#include <asm/unistd.h>
#include "sumh_runtime.h"
#include "sumh_dirhijack.h"
#include "sumh_hide_rules.h"
#include "sumh_vnode.h"
#include "sumh_bootstrap.h"
#include "feature/sucompat_vfs.h"
#include "sumh_store.h"
#include "sumh_maps_store.h"
#include "sumh_entrypoints.h"
#include "sumh_path_policy.h"
#include "sumh_overlay.h"
#include "sumh_proc_read_hooks.h"
#include "sumh_proc_memory.h"
#include "sumh_vfs_hooks.h"
#include "sumh_vfs_view.h"
#include "sumh_fop_bridge.h"
#include "sumh_iop_override.h"
#include "sumh_fop_override.h"
#include "sumh_sop_shadow.h"
#include "sumh_fake_mountinfo.h"
#include "sumh_kernel_build.h"

/* Requested allocation bytes only; native allocator/VFS and proxy objects
 * are outside this first accounting slice. The snapshot is consistent, but
 * allocations can change immediately after releasing its leaf lock.
 */
static int sumh_proc_memory_report(char *buf, size_t size)
{
	struct sumh_proc_memory_stats memory;

	sumh_proc_memory_snapshot(&memory);
	return scnprintf(
	    buf, size,
	    "proc requested memory: scope=snapshot+stream+workspace+maps "
	    "proxy=excluded used=%zu limit=%lu peak=%zu snapshot=%zu "
	    "stream=%zu workspace=%zu maps=%zu denied=%llu "
	    "allocation_failed=%llu\n",
	    memory.used, SUMH_PROC_MEMORY_BUDGET, memory.peak,
	    memory.by_kind[SUMH_PROC_MEMORY_SNAPSHOT],
	    memory.by_kind[SUMH_PROC_MEMORY_STREAM],
	    memory.by_kind[SUMH_PROC_MEMORY_WORKSPACE],
	    memory.by_kind[SUMH_PROC_MEMORY_MAPS],
	    (unsigned long long)memory.denied,
	    (unsigned long long)memory.allocation_failed);
}

/* Mount filtering needs both the ingress hook and the snapshot renderer.
 * Keep capability queries and mutation guards on the same readiness test. */
static bool sumh_mount_hide_available(void)
{
	return sumh_proc_proxy_available() && sumh_fake_mi_active();
}

/* Settings and error history use the control mutation mutex. Lifecycle init
 * runs exclusively before publishing the provider. External hook readiness
 * has its own locks: report sampled availability, not global atomicity. */
static u64 sumh_runtime_generation;
static u64 sumh_runtime_instance;
static s32 sumh_runtime_errors[SUMH_RUNTIME_STATE_COUNT];
static bool sumh_runtime_live;

static void sumh_runtime_advance(void)
{
	if (!++sumh_runtime_generation)
		sumh_runtime_generation = 1;
}

void sumh_runtime_state_init(void)
{
	BUILD_BUG_ON(sizeof(struct sumh_runtime_feature_state) != 32);
	BUILD_BUG_ON(sizeof(struct sumh_runtime_state_arg) != 240);
	BUILD_BUG_ON(sizeof(struct sumh_runtime_set_arg) != 48);
	BUILD_BUG_ON(sizeof(struct sumh_kernel_build_cas_arg) != 168);
	BUILD_BUG_ON(sizeof(struct sumh_rule_inventory_entry) != 8272);
	BUILD_BUG_ON(sizeof(struct sumh_rule_inventory_arg) != 8352);
	if (!sumh_runtime_instance)
		get_random_bytes(&sumh_runtime_instance,
				 sizeof(sumh_runtime_instance));
	if (!++sumh_runtime_instance)
		sumh_runtime_instance = 1;
	sumh_runtime_generation = 1;
	memset(sumh_runtime_errors, 0, sizeof(sumh_runtime_errors));
	sumh_debug_enabled = false;
	sumh_stealth_enabled = false;
	sumh_feature_enabled_mask = 0;
	sumh_mount_hide_mode = SUMH_MOUNT_HIDE_MODE_NORMAL;
	sumh_runtime_live = true;
}

void sumh_runtime_state_shutdown(void)
{
	sumh_runtime_live = false;
	sumh_runtime_advance();
}

static void sumh_runtime_entry(struct sumh_runtime_state_arg *state, u32 id,
			       bool supported, bool configured, bool ready)
{
	struct sumh_runtime_feature_state *entry = &state->entries[id - 1];

	entry->id = id;
	entry->supported = supported;
	entry->configured = configured;
	entry->active = configured && supported && ready;
	entry->degraded = configured && !entry->active;
	entry->last_error = sumh_runtime_errors[id - 1];
}

static int sumh_runtime_state_get(void __user *arg)
{
	struct sumh_runtime_state_arg state;
	bool enabled, mount_available, mount_configured;
	int mask;

	if (copy_from_user(&state, arg, sizeof(state)))
		return -EFAULT;
	if (state.size != sizeof(state) ||
	    state.version != SUMH_RUNTIME_STATE_VERSION)
		return -EINVAL;
	state.size = state.version = 0;
	if (memchr_inv(&state, 0, sizeof(state)))
		return -EINVAL;
	if (!sumh_runtime_live)
		return -ESHUTDOWN;
	state.size = sizeof(state);
	state.version = SUMH_RUNTIME_STATE_VERSION;
	state.count = SUMH_RUNTIME_STATE_COUNT;
	state.flags =
	    SUMH_RUNTIME_CONTROL_SERIALIZED | SUMH_RUNTIME_READINESS_SAMPLED;
	state.generation = sumh_runtime_generation;
	state.instance_id = sumh_runtime_instance;
	state.mount_hide_mode = sumh_mount_hide_mode;
	enabled = smp_load_acquire(&sumh_enabled);
	mask = sumh_feature_enabled_mask;
	mount_available = sumh_mount_hide_available();
	mount_configured = !!(mask & SUMH_FEATURE_MOUNT_HIDE);
	sumh_runtime_entry(&state, SUMH_RUNTIME_DEBUG, true, sumh_debug_enabled,
			   true);
	sumh_runtime_entry(&state, SUMH_RUNTIME_STEALTH,
			   sumh_fop_override_available(), sumh_stealth_enabled,
			   enabled);
	sumh_runtime_entry(&state, SUMH_RUNTIME_MOUNT_HIDE, mount_available,
			   mount_configured, enabled);
	sumh_runtime_entry(&state, SUMH_RUNTIME_MAPS_SPOOF,
			   sumh_proc_proxy_available(),
			   !!(mask & SUMH_FEATURE_MAPS_SPOOF), enabled);
	sumh_runtime_entry(&state, SUMH_RUNTIME_STATFS_SPOOF,
			   sumh_statfs_view_available(),
			   !!(mask & SUMH_FEATURE_STATFS_SPOOF),
			   enabled && mount_available && mount_configured);
	sumh_runtime_entry(&state, SUMH_RUNTIME_KERNEL_BUILD,
			   sumh_kernel_build_available(),
			   sumh_kernel_build_enabled(), true);
	return copy_to_user(arg, &state, sizeof(state)) ? -EFAULT : 0;
}

static int sumh_inventory_path(char *out, u32 *length, const char *path)
{
	size_t len = strnlen(path, SUMH_RULE_INVENTORY_PATH_MAX);

	if (len == SUMH_RULE_INVENTORY_PATH_MAX)
		return -EOVERFLOW;
	*length = len;
	memcpy(out, path, len);
	return 0;
}

/* -1: lookahead found another record, 0: skip, 1: fill this page. The
 * ordinal belongs to category+instance+generation, never a mutation ID. */
static int sumh_inventory_position(struct sumh_rule_inventory_arg *page,
				   u64 *seen)
{
	if ((*seen)++ < page->cursor)
		return 0;
	if (page->count) {
		page->complete = 0;
		return -1;
	}
	page->count = 1;
	page->entry.id = *seen;
	page->entry.id_kind = SUMH_INVENTORY_ID_GENERATION_ORDINAL;
	return 1;
}

/* Called under the lifecycle/control mutation boundary. All writes to these
 * definition tables (including recursive merge materialization) originate in
 * a serialized ioctl or exclusive shutdown. Managed-hide binding work changes
 * a separate table's binding state, intentionally absent from this ABI. */
static int sumh_inventory_collect(struct sumh_rule_inventory_arg *page)
{
	struct sumh_entry *mapping;
	struct sumh_hide_entry *hidden;
	struct sumh_merge_entry *merge;
	struct sumh_inject_entry *inject;
	const struct sumh_maps_rule_entry *maps;
	struct sumh_maps_snapshot *maps_snapshot;
	unsigned int maps_index;
	struct sumh_rule_inventory_entry *out = &page->entry;
	u64 seen = 0;
	int bucket, position, ret = 0;
	bool more;

	page->complete = 1;
	switch (page->category) {
	case 0:
		return 0;
	case SUMH_INVENTORY_MAPPING:
		hash_for_each(sumh_paths, bucket, mapping, node)
		{
			position = sumh_inventory_position(page, &seen);
			if (position < 0)
				break;
			if (!position)
				continue;
			out->type = mapping->type;
			ret = sumh_inventory_path(out->path1, &out->path1_len,
						  mapping->src);
			if (!ret)
				ret = sumh_inventory_path(out->path2,
							  &out->path2_len,
							  mapping->target);
			if (ret)
				return ret;
		}
		break;
	case SUMH_INVENTORY_LEGACY_HIDE:
		hash_for_each(sumh_hide_paths, bucket, hidden, node)
		{
			position = sumh_inventory_position(page, &seen);
			if (position < 0)
				break;
			if (!position)
				continue;
			out->flags = hidden->storage_managed
					 ? SUMH_INVENTORY_LEGACY_STORAGE_MANAGED
					 : 0;
			ret = sumh_inventory_path(out->path1, &out->path1_len,
						  hidden->path);
			if (ret)
				return ret;
		}
		break;
	case SUMH_INVENTORY_MERGE:
		hash_for_each(sumh_merge_dirs, bucket, merge, node)
		{
			position = sumh_inventory_position(page, &seen);
			if (position < 0)
				break;
			if (!position)
				continue;
			ret = sumh_inventory_path(out->path1, &out->path1_len,
						  merge->src);
			if (!ret)
				ret = sumh_inventory_path(
				    out->path2, &out->path2_len, merge->target);
			if (ret)
				return ret;
		}
		break;
	case SUMH_INVENTORY_MAPS_MANAGED:
	case SUMH_INVENTORY_MAPS_TEMPORARY:
		maps_snapshot = sumh_maps_snapshot_get();
		for (maps_index = 0;
		     maps_index < sumh_maps_snapshot_count(maps_snapshot);
		     maps_index++) {
			maps =
			    sumh_maps_snapshot_entry(maps_snapshot, maps_index);
			if (maps->managed !=
			    (page->category == SUMH_INVENTORY_MAPS_MANAGED))
				continue;
			position = sumh_inventory_position(page, &seen);
			if (position < 0)
				break;
			if (!position)
				continue;
			out->owner = maps->managed
					 ? SUMH_INVENTORY_OWNER_MANAGED_MAPS
					 : SUMH_INVENTORY_OWNER_UNATTRIBUTED;
			out->target_ino = maps->target_ino;
			out->target_dev = maps->target_dev;
			out->spoofed_ino = maps->spoofed_ino;
			out->spoofed_dev = maps->spoofed_dev;
			ret = sumh_inventory_path(out->path2, &out->path2_len,
						  maps->spoofed_pathname);
			if (ret)
				break;
		}
		sumh_maps_snapshot_put(maps_snapshot);
		if (ret)
			return ret;
		break;
	case SUMH_INVENTORY_INJECT:
		hash_for_each(sumh_inject_dirs, bucket, inject, node)
		{
			position = sumh_inventory_position(page, &seen);
			if (position < 0)
				break;
			if (!position)
				continue;
			ret = sumh_inventory_path(out->path1, &out->path1_len,
						  inject->dir);
			if (ret)
				return ret;
		}
		break;
	case SUMH_INVENTORY_USER_HIDE:
		ret = sumh_hide_rule_inventory(page->cursor, out, &more);
		if (ret < 0)
			return ret;
		page->count = ret;
		page->complete = !more;
		seen = page->cursor;
		break;
	default:
		return -EOPNOTSUPP;
	}
	if (page->cursor > seen)
		return -EINVAL;
	if (page->count)
		out->category = page->category;
	page->next_cursor = page->cursor + page->count;
	return 0;
}

static int sumh_rule_inventory_get(void __user *arg)
{
	struct sumh_rule_inventory_arg *page;
	u64 expected_instance, expected_generation, cursor;
	u32 category;
	int ret = -EINVAL;

	/* Two PATH_MAX arrays cannot live on a kernel stack. A page is fixed
	 * and bounded; no caller-selected allocation or whole-table snapshot.
	 */
	page = kvzalloc(sizeof(*page), GFP_KERNEL);
	if (!page)
		return -ENOMEM;
	if (copy_from_user(page, arg, sizeof(*page))) {
		ret = -EFAULT;
		goto out;
	}
	if (page->size != sizeof(*page) ||
	    page->version != SUMH_RULE_INVENTORY_VERSION)
		goto out;
	expected_instance = page->expected_instance_id;
	expected_generation = page->expected_generation;
	cursor = page->cursor;
	category = page->category;
	page->size = page->version = page->category = 0;
	page->expected_instance_id = page->expected_generation = page->cursor =
	    0;
	if (memchr_inv(page, 0, sizeof(*page)) ||
	    (!!expected_instance != !!expected_generation) ||
	    (cursor && (!expected_instance || !category)))
		goto out;
	if (category > SUMH_INVENTORY_USER_HIDE) {
		ret = -EOPNOTSUPP;
		goto out;
	}
	if (!sumh_runtime_live) {
		ret = -ESHUTDOWN;
		goto out;
	}
	if (expected_instance &&
	    (expected_instance != sumh_runtime_instance ||
	     expected_generation != sumh_runtime_generation)) {
		ret = -ESTALE;
		goto out;
	}
	page->size = sizeof(*page);
	page->version = SUMH_RULE_INVENTORY_VERSION;
	page->category = category;
	page->cursor = cursor;
	page->instance_id = sumh_runtime_instance;
	page->generation = sumh_runtime_generation;
	page->supported_categories = SUMH_INVENTORY_SUPPORTED_CATEGORIES;
	page->flags = SUMH_INVENTORY_DEFINITIONS_COHERENT;
	ret = sumh_inventory_collect(page);
	if (!ret && copy_to_user(arg, page, sizeof(*page)))
		ret = -EFAULT;
out:
	kvfree(page);
	return ret;
}

/* The outer mutation boundary records even copy_from_user failures. Read
 * errors never overwrite feature errors; failed attempts still advance the
 * control sequence. CLEAR_ALL intentionally leaves debug untouched. */
static void sumh_runtime_record_result(unsigned int cmd, long result)
{
	u32 id = 0;
	int i;

	switch (cmd) {
	case SUMH_IOC_SET_DEBUG:
		id = SUMH_RUNTIME_DEBUG;
		break;
	case SUMH_IOC_SET_STEALTH:
		id = SUMH_RUNTIME_STEALTH;
		break;
	case SUMH_IOC_SET_MOUNT_HIDE:
	case SUMH_IOC_SET_MOUNT_HIDE_MODE:
		id = SUMH_RUNTIME_MOUNT_HIDE;
		break;
	case SUMH_IOC_SET_MAPS_SPOOF:
		id = SUMH_RUNTIME_MAPS_SPOOF;
		break;
	case SUMH_IOC_SET_STATFS_SPOOF:
		id = SUMH_RUNTIME_STATFS_SPOOF;
		break;
	case SUMH_IOC_SET_KERNEL_BUILD:
		id = SUMH_RUNTIME_KERNEL_BUILD;
		break;
	case SUMH_IOC_CLEAR_ALL:
		for (i = SUMH_RUNTIME_STEALTH - 1; i < SUMH_RUNTIME_STATE_COUNT;
		     i++)
			sumh_runtime_errors[i] = result < 0 ? result : 0;
		break;
	case SUMH_IOC_USER_HIDE_UPSERT:
	case SUMH_IOC_USER_HIDE_DELETE:
	case SUMH_IOC_USER_HIDE_RETRY:
	case SUMH_IOC_USER_HIDE_CLEAR:
	case SUMH_IOC_SET_ENABLED:
	case SUMH_IOC_ADD_RULE:
	case SUMH_IOC_DEL_RULE:
	case SUMH_IOC_HIDE_RULE:
	case SUMH_IOC_REORDER_MNT_ID:
	case SUMH_IOC_HIDE_OVERLAY_XATTRS:
	case SUMH_IOC_ADD_MERGE_RULE:
	case SUMH_IOC_SET_MIRROR_PATH:
	case SUMH_IOC_ADD_MAPS_RULE:
	case SUMH_IOC_SET_MANAGED_MAPS:
	case SUMH_IOC_CLEAR_MAPS_RULES:
	case SUMH_IOC_ADD_SPOOF_KSTAT:
	case SUMH_IOC_UPDATE_SPOOF_KSTAT:
		break;
	default:
		return;
	}
	if (id)
		sumh_runtime_errors[id - 1] = result < 0 ? result : 0;
	sumh_runtime_advance();
}

/* Only this new setter provides compare-and-set semantics. Input and stale
 * checks precede error accounting, so stale clients cannot perturb the token
 * used by the current writer. Readiness failures of a matched canonical
 * request are observable attempts; disabled cleanup never needs readiness. */
static int sumh_kernel_build_conditional_set(void __user *arg)
{
	struct sumh_kernel_build_cas_arg set;
	struct sumh_kernel_build_arg build = {0};
	int ret;

	if (copy_from_user(&set, arg, sizeof(set)))
		return -EFAULT;
	if (set.size != sizeof(set) ||
	    set.version != SUMH_RUNTIME_STATE_VERSION || set.reserved ||
	    memchr_inv(set.reserved_tail, 0, sizeof(set.reserved_tail)))
		return -EINVAL;
	build.size = sizeof(build);
	build.enable = set.enable;
	memcpy(build.release, set.release, sizeof(build.release));
	memcpy(build.version, set.version_string, sizeof(build.version));
	ret = sumh_kernel_build_validate(&build);
	if (ret)
		return ret;
	if (!sumh_runtime_live)
		return -ESHUTDOWN;
	if (set.expected_generation != sumh_runtime_generation ||
	    set.expected_instance_id != sumh_runtime_instance)
		return -ESTALE;
	ret = sumh_kernel_build_set(&build);
	sumh_runtime_errors[SUMH_RUNTIME_KERNEL_BUILD - 1] = ret;
	sumh_runtime_advance();
	return ret;
}

static int sumh_runtime_feature_set(void __user *arg)
{
	struct sumh_runtime_set_arg set;
	int ret = 0;

	if (copy_from_user(&set, arg, sizeof(set)))
		return -EFAULT;
	if (set.size != sizeof(set) ||
	    set.version != SUMH_RUNTIME_STATE_VERSION || set.enable > 1 ||
	    set.mount_hide_mode != SUMH_MOUNT_HIDE_MODE_NORMAL ||
	    set.reserved[0] || set.reserved[1] || set.reserved[2])
		return -EINVAL;
	if (set.id < SUMH_RUNTIME_DEBUG || set.id > SUMH_RUNTIME_STATFS_SPOOF)
		return -EOPNOTSUPP;
	if (!sumh_runtime_live)
		return -ESHUTDOWN;
	if (set.expected_generation != sumh_runtime_generation ||
	    set.expected_instance_id != sumh_runtime_instance)
		return -ESTALE;

	switch (set.id) {
	case SUMH_RUNTIME_DEBUG:
		sumh_debug_enabled = !!set.enable;
		break;
	case SUMH_RUNTIME_STEALTH:
		if (set.enable && !sumh_fop_override_available()) {
			ret = -EOPNOTSUPP;
			break;
		}
		sumh_stealth_enabled = !!set.enable;
		break;
	case SUMH_RUNTIME_MOUNT_HIDE:
		if (set.enable && !sumh_mount_hide_available()) {
			ret = -EOPNOTSUPP;
			break;
		}
		if (set.enable)
			sumh_feature_enabled_mask |= SUMH_FEATURE_MOUNT_HIDE;
		else
			sumh_feature_enabled_mask &= ~SUMH_FEATURE_MOUNT_HIDE;
		sumh_mount_hide_mode = SUMH_MOUNT_HIDE_MODE_NORMAL;
		sumh_fake_mi_invalidate_all();
		break;
	case SUMH_RUNTIME_MAPS_SPOOF:
		if (set.enable && !sumh_proc_proxy_available()) {
			ret = -EOPNOTSUPP;
			break;
		}
		if (set.enable)
			sumh_feature_enabled_mask |= SUMH_FEATURE_MAPS_SPOOF;
		else
			sumh_feature_enabled_mask &= ~SUMH_FEATURE_MAPS_SPOOF;
		break;
	case SUMH_RUNTIME_STATFS_SPOOF:
		if (set.enable && !sumh_statfs_view_available()) {
			ret = -EOPNOTSUPP;
			break;
		}
		if (set.enable)
			sumh_feature_enabled_mask |= SUMH_FEATURE_STATFS_SPOOF;
		else
			sumh_feature_enabled_mask &= ~SUMH_FEATURE_STATFS_SPOOF;
		break;
	}
	sumh_runtime_errors[set.id - 1] = ret;
	sumh_runtime_advance();
	return ret;
}

/* Both legacy commands upsert the same store; a supplied path is the
 * authoritative identity, including after replacement or remount. */
static int sumh_kstat_set(void __user *arg)
{
	struct sumh_spoof_kstat __user *u = arg;
	struct sumh_spoof_kstat *k;
	struct sumh_spoof_kstat_entry *entry = NULL, *old = NULL, *cursor;
	struct path resolved;
	struct inode *inode = NULL;
	bool have_path, path_held = false;
	size_t len;
	int ret = 0;

	k = kmalloc(sizeof(*k), GFP_KERNEL);
	if (!k)
		return -ENOMEM;
	if (copy_from_user(k, u, sizeof(*k))) {
		kfree(k);
		return -EFAULT;
	}
	len = strnlen(k->target_pathname, sizeof(k->target_pathname));
	have_path = len != 0;
	if (len == sizeof(k->target_pathname) ||
	    (have_path && k->target_pathname[0] != '/') ||
	    (!have_path && !k->target_ino) || k->spoofed_size < 0 ||
	    k->spoofed_dev > 0xffffffffULL ||
	    k->spoofed_blksize > 0xffffffffULL || k->spoofed_atime_nsec < 0 ||
	    k->spoofed_atime_nsec >= 1000000000L || k->spoofed_mtime_nsec < 0 ||
	    k->spoofed_mtime_nsec >= 1000000000L || k->spoofed_ctime_nsec < 0 ||
	    k->spoofed_ctime_nsec >= 1000000000L) {
		ret = -EINVAL;
		goto out;
	}
	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry) {
		ret = -ENOMEM;
		goto out;
	}
	if (have_path) {
		ret = kern_path(k->target_pathname, LOOKUP_FOLLOW, &resolved);
		if (ret)
			goto out;
		path_held = true;
		inode = d_inode(resolved.dentry);
		if (!inode || !inode->i_sb || !inode->i_mapping ||
		    !inode->i_ino) {
			ret = -EINVAL;
			goto out;
		}
		k->target_ino = inode->i_ino;
		entry->target_dev = inode->i_sb->s_dev;
		entry->target_pathname =
		    kstrdup(k->target_pathname, GFP_KERNEL);
		if (!entry->target_pathname) {
			ret = -ENOMEM;
			goto out;
		}
		entry->path_hash =
		    full_name_hash(NULL, k->target_pathname, len);
	}
	entry->target_ino = k->target_ino;
	entry->spoofed_ino = k->spoofed_ino;
	entry->spoofed_dev = k->spoofed_dev;
	entry->spoofed_nlink = k->spoofed_nlink;
	entry->spoofed_size = k->spoofed_size;
	entry->spoofed_atime_sec = k->spoofed_atime_sec;
	entry->spoofed_atime_nsec = k->spoofed_atime_nsec;
	entry->spoofed_mtime_sec = k->spoofed_mtime_sec;
	entry->spoofed_mtime_nsec = k->spoofed_mtime_nsec;
	entry->spoofed_ctime_sec = k->spoofed_ctime_sec;
	entry->spoofed_ctime_nsec = k->spoofed_ctime_nsec;
	entry->spoofed_blksize = k->spoofed_blksize;
	entry->spoofed_blocks = k->spoofed_blocks;
	entry->is_static = k->is_static;

	mutex_lock(&sumh_config_mutex);
	if (have_path) {
		hlist_for_each_entry(cursor,
				     &sumh_spoof_kstat_path[hash_min(
					 entry->path_hash, SUMH_HASH_BITS)],
				     path_node)
		{
			if (cursor->path_hash == entry->path_hash &&
			    !strcmp(cursor->target_pathname,
				    entry->target_pathname)) {
				old = cursor;
				break;
			}
		}
	}
	hlist_for_each_entry(
	    cursor,
	    &sumh_spoof_kstat_ino[hash_min(entry->target_ino, SUMH_HASH_BITS)],
	    ino_node)
	{
		if (cursor->target_ino != entry->target_ino || cursor == old)
			continue;
		if (!have_path && !cursor->target_pathname) {
			old = cursor;
			break;
		}
		if (have_path && cursor->target_pathname &&
		    cursor->target_dev == entry->target_dev) {
			ret = -EEXIST;
			goto unlock;
		}
	}
	/* Install a transparent wrapper first. Failure must not clear a flag
	 * belonging to an existing rule or publish a partially installed rule.
	 */
	if (inode) {
		ret = sumh_iop_install(inode);
		if (ret)
			goto unlock;
	}
	k->err = 0;
	if (copy_to_user(u, k, sizeof(*k))) {
		ret = -EFAULT;
		goto unlock;
	}
	if (have_path) {
		if (old)
			hlist_replace_rcu(&old->path_node, &entry->path_node);
		else
			hlist_add_head_rcu(
			    &entry->path_node,
			    &sumh_spoof_kstat_path[hash_min(entry->path_hash,
							    SUMH_HASH_BITS)]);
	}
	if (old && old->target_ino == entry->target_ino) {
		hlist_replace_rcu(&old->ino_node, &entry->ino_node);
	} else {
		hlist_add_head_rcu(&entry->ino_node,
				   &sumh_spoof_kstat_ino[hash_min(
				       entry->target_ino, SUMH_HASH_BITS)]);
		if (old && old->target_ino)
			hlist_del_rcu(&old->ino_node);
	}
	if (old)
		call_rcu(&old->rcu, sumh_spoof_kstat_entry_free_rcu);
	else
		atomic_inc(&sumh_spoof_kstat_count);
	if (inode)
		set_bit(AS_FLAGS_SUMH_SPOOF_KSTAT, &inode->i_mapping->flags);
	entry = NULL;
unlock:
	mutex_unlock(&sumh_config_mutex);
out:
	if (path_held)
		path_put(&resolved);
	if (entry) {
		kfree(entry->target_pathname);
		kfree(entry);
	}
	if (ret) {
		k->err = ret;
		if (copy_to_user(u, k, sizeof(*k)))
			ret = -EFAULT;
	}
	kfree(k);
	return ret;
}

/* The outer control mutation mutex serializes preparation through publish. */
static long sumh_maps_add_rule(void __user *user)
{
	struct sumh_maps_rule rule;
	struct sumh_maps_snapshot *prepared = NULL;
	int ret;

	if (copy_from_user(&rule, user, sizeof(rule)))
		return -EFAULT;
	ret = sumh_maps_prepare_add(&rule, &prepared);
	if (ret)
		return ret;
	/* Preserve the legacy reply before the no-fail publication: a bad
	 * output pointer must not install a rule or change either owner set. */
	if (copy_to_user(user, &rule, sizeof(rule))) {
		sumh_maps_snapshot_put(prepared);
		return -EFAULT;
	}
	sumh_maps_publish(prepared);
	return 0;
}

/* Called under the outer lifecycle/mutation boundary. Cleanup does not need
 * a live proc filter; rejected requests never advance the control token. */
static long sumh_maps_delete_temporary(void __user *user)
{
	struct sumh_maps_delete_arg arg;
	struct sumh_maps_snapshot *prepared = NULL;
	int ret;

	if (copy_from_user(&arg, user, sizeof(arg)))
		return -EFAULT;
	if (arg.size != sizeof(arg) ||
	    arg.version != SUMH_MAPS_DELETE_VERSION ||
	    !arg.expected_instance_id || !arg.expected_generation ||
	    arg.target_dev > 0xffffffffULL)
		return -EINVAL;
	if (arg.target_ino != (unsigned long)arg.target_ino)
		return -EOVERFLOW;
	if (!sumh_runtime_live)
		return -ESHUTDOWN;
	if (arg.expected_instance_id != sumh_runtime_instance ||
	    arg.expected_generation != sumh_runtime_generation)
		return -ESTALE;
	ret =
	    sumh_maps_prepare_delete((unsigned long)arg.target_ino,
				     (unsigned long)arg.target_dev, &prepared);
	if (ret)
		return ret;
	sumh_maps_publish(prepared);
	sumh_runtime_advance();
	return 0;
}

static long sumh_maps_replace_managed(void __user *user)
{
	struct sumh_managed_maps_arg arg;
	struct sumh_maps_rule *rules = NULL;
	struct sumh_maps_snapshot *prepared = NULL;
	size_t bytes = 0;
	int ret;

	if (copy_from_user(&arg, user, sizeof(arg)))
		return -EFAULT;
	if (arg.size != sizeof(arg) || arg.reserved[0] || arg.reserved[1] ||
	    arg.count > SUMH_MANAGED_MAPS_MAX_RULES ||
	    (arg.count && !arg.rules))
		return -EINVAL;
	/* Empty owned cleanup is allowed even without an available filter.
	 * Retaining temporary entries can still require a bounded allocation.
	 */
	if (arg.count && !sumh_proc_proxy_available())
		return -EOPNOTSUPP;
	if (arg.count) {
		bytes = arg.count * sizeof(*rules);
		if (!sumh_proc_memory_reserve(SUMH_PROC_MEMORY_WORKSPACE,
					      bytes))
			return -ENOMEM;
		rules = kvmalloc_array(arg.count, sizeof(*rules), GFP_KERNEL);
		if (!rules) {
			sumh_proc_memory_alloc_failed(
			    SUMH_PROC_MEMORY_WORKSPACE);
			ret = -ENOMEM;
			goto out;
		}
		if (copy_from_user(rules, u64_to_user_ptr(arg.rules), bytes)) {
			ret = -EFAULT;
			goto out;
		}
	}
	ret = sumh_maps_prepare_managed(rules, arg.count, &prepared);
	if (!ret)
		sumh_maps_publish(prepared);
out:
	kvfree(rules);
	if (bytes)
		sumh_proc_memory_release(SUMH_PROC_MEMORY_WORKSPACE, bytes);
	return ret;
}

static int SUMH_NOCFI sumh_resolve_rule_path(char **pathname,
					     struct inode **target_out,
					     struct inode **parent_out)
{
	struct path path;
	struct inode *target = NULL, *parent = NULL;
	char *buffer, *parent_name = NULL, *resolved = NULL, *text;
	const char *leaf = NULL;
	int ret;

	if (!pathname || !*pathname || (*pathname)[0] != '/')
		return -EINVAL;
	buffer = kmalloc(PATH_MAX, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;
	ret = kern_path(*pathname, LOOKUP_FOLLOW, &path);
	if (ret == -ENOENT) {
		char *slash;

		parent_name = kstrdup(*pathname, GFP_KERNEL);
		if (!parent_name) {
			ret = -ENOMEM;
			goto out;
		}
		slash = strrchr(parent_name, '/');
		leaf = *pathname + (slash - parent_name) + 1;
		if (!*leaf) {
			ret = -EINVAL;
			goto out;
		}
		if (slash == parent_name)
			slash[1] = '\0';
		else
			*slash = '\0';
		ret = kern_path(parent_name, LOOKUP_FOLLOW | LOOKUP_DIRECTORY,
				&path);
		if (ret == -ENOENT) {
			/* Keep the stored key deletable after its parent
			 * disappears. */
			ret = 0;
			goto out;
		}
	}
	if (ret)
		goto out;
	text = d_path(&path, buffer, PATH_MAX);
	if (IS_ERR(text)) {
		ret = PTR_ERR(text);
		goto out_path;
	}
	if (text[0] != '/') {
		ret = -EINVAL;
		goto out_path;
	}
	if (leaf) {
		size_t prefix = strcmp(text, "/") ? strlen(text) : 0;
		size_t length = strlen(leaf);

		if (prefix + length + 2 > PATH_MAX) {
			ret = -ENAMETOOLONG;
			goto out_path;
		}
		resolved = kmalloc(prefix + length + 2, GFP_KERNEL);
		if (resolved) {
			memcpy(resolved, text, prefix);
			resolved[prefix] = '/';
			memcpy(resolved + prefix + 1, leaf, length + 1);
		}
		parent = d_inode(path.dentry);
	} else {
		resolved = kstrdup(text, GFP_KERNEL);
		target = d_inode(path.dentry);
		parent = d_inode(path.dentry->d_parent);
	}
	if (!resolved) {
		ret = -ENOMEM;
		goto out_path;
	}
	if (target_out && target) {
		ihold(target);
		*target_out = target;
	}
	if (parent_out && parent) {
		ihold(parent);
		*parent_out = parent;
	}
	kfree(*pathname);
	*pathname = resolved;
out_path:
	path_put(&path);
out:
	kfree(parent_name);
	kfree(buffer);
	return ret;
}

/* Normalize only lexical components. DEL must select a stored key before
 * consulting today's namespace; canonical fallback is a separate step. */
static int sumh_normalize_rule_key(char *path)
{
	size_t read = 1, write = 1;

	if (!path || path[0] != '/')
		return -EINVAL;
	while (path[read]) {
		size_t begin, len;

		while (path[read] == '/')
			read++;
		begin = read;
		while (path[read] && path[read] != '/')
			read++;
		len = read - begin;
		if (!len || (len == 1 && path[begin] == '.'))
			continue;
		if (len == 2 && path[begin] == '.' && path[begin + 1] == '.') {
			while (write > 1 && path[write - 1] != '/')
				write--;
			if (write > 1)
				write--;
			continue;
		}
		if (write > 1)
			path[write++] = '/';
		memmove(path + write, path + begin, len);
		write += len;
	}
	path[write] = '\0';
	return 0;
}

static SUMH_NOCFI int sumh_dispatch_cmd(unsigned int cmd, void __user *arg)
{
	struct sumh_syscall_arg req;
	struct sumh_entry *entry;
	struct sumh_hide_entry *hide_entry;
	char *src = NULL, *target = NULL;
	u32 hash;
	int ret = 0;

	if (cmd == SUMH_IOC_CLEAR_ALL) {
		struct sumh_dh_retire retired;

		sumh_dh_retire_init(&retired, true);
		sumh_hide_rules_clear();
		sumh_kernel_build_clear();
		mutex_lock(&sumh_config_mutex);
		sumh_cleanup_locked(&retired);
		mutex_unlock(&sumh_config_mutex);
		sumh_dh_retire_finish(&retired);
		sumh_dirhijack_clear();
		sumh_fop_override_clear();
		sumh_sop_shadow_reap();
		sumh_fake_mi_invalidate_all();
		sumh_store_drain();
		sumh_dh_drain_releases();
		sumh_sop_shadow_reap();
		return 0;
	}

	if (cmd == SUMH_IOC_GET_VERSION) {
		int ver = SUMH_PROTOCOL_VERSION;
		if (copy_to_user(arg, &ver, sizeof(ver)))
			return -EFAULT;
		return 0;
	}

	if (cmd == SUMH_IOC_GET_ENABLED) {
		int enabled = smp_load_acquire(&sumh_enabled);

		return copy_to_user(arg, &enabled, sizeof(enabled)) ? -EFAULT
								    : 0;
	}

	if (cmd == SUMH_IOC_SET_KERNEL_BUILD) {
		struct sumh_kernel_build_arg build;

		if (copy_from_user(&build, arg, sizeof(build)))
			return -EFAULT;
		return sumh_kernel_build_set(&build);
	}

	if (cmd == SUMH_IOC_GET_KERNEL_BUILD ||
	    cmd == SUMH_IOC_GET_ORIGINAL_KERNEL_BUILD) {
		struct sumh_kernel_build_arg build;

		ret = cmd == SUMH_IOC_GET_ORIGINAL_KERNEL_BUILD
			  ? sumh_kernel_build_get_original(&build)
			  : sumh_kernel_build_get(&build);
		if (ret)
			return ret;
		return copy_to_user(arg, &build, sizeof(build)) ? -EFAULT : 0;
	}

	if (cmd == SUMH_IOC_SET_DEBUG) {
		int val;
		if (copy_from_user(&val, arg, sizeof(val)))
			return -EFAULT;
		sumh_debug_enabled = !!val;
		sumh_log("debug mode %s\n",
			 sumh_debug_enabled ? "enabled" : "disabled");
		return 0;
	}

	if (cmd == SUMH_IOC_SET_STEALTH) {
		int val;
		if (copy_from_user(&val, arg, sizeof(val)))
			return -EFAULT;
		if (val && !sumh_fop_override_available())
			return -EOPNOTSUPP;
		sumh_stealth_enabled = !!val;
		sumh_log("stealth mode %s\n",
			 sumh_stealth_enabled ? "enabled" : "disabled");
		return 0;
	}

	if (cmd == SUMH_IOC_SET_ENABLED) {
		int val;
		if (copy_from_user(&val, arg, sizeof(val)))
			return -EFAULT;
		if (!val)
			return -EPERM;
		if (val != 1)
			return -EINVAL;
		/* Retain the ioctl for old clients, but SUMH is always
		 * enabled once its bootstrap completed. */
		return sumh_is_ready() ? 0 : -EOPNOTSUPP;
	}

	if (cmd == SUMH_IOC_REORDER_MNT_ID) {
		/* struct mnt_namespace/mount not exposed to LKM; only KPM
		 * (built-in) supports this */
		return -EOPNOTSUPP;
	}

	if (cmd == SUMH_IOC_LIST_RULES) {
		struct sumh_inject_entry *inject_entry;
		struct sumh_syscall_list_arg list_arg;
		struct sumh_xattr_sb_entry *sb_entry;
		struct sumh_merge_entry *merge_entry;
		char *kbuf;
		size_t buf_size, written = 0;
		int bkt;

		if (copy_from_user(&list_arg, arg, sizeof(list_arg)))
			return -EFAULT;

		buf_size = list_arg.size;
		if (buf_size > 64 * 1024)
			buf_size = 64 * 1024;

		/* Rule dumps need no physically contiguous backing. Allow
		 * vmalloc fallback when a 64 KiB allocation would fail from
		 * fragmentation.
		 */
		kbuf = kvzalloc(buf_size, GFP_KERNEL);
		if (!kbuf)
			return -ENOMEM;

		rcu_read_lock();
		written +=
		    scnprintf(kbuf + written, buf_size - written,
			      "SUMH Protocol: %d\n", SUMH_PROTOCOL_VERSION);
		written +=
		    scnprintf(kbuf + written, buf_size - written,
			      "SUMH Enabled: %d\n", sumh_enabled ? 1 : 0);
		hash_for_each_rcu(sumh_paths, bkt, entry, node)
		{
			if (written >= buf_size)
				break;
			written += scnprintf(kbuf + written, buf_size - written,
					     "add %s %s %d\n", entry->src,
					     entry->target, entry->type);
		}
		hash_for_each_rcu(sumh_hide_paths, bkt, hide_entry, node)
		{
			if (written >= buf_size)
				break;
			written += scnprintf(kbuf + written, buf_size - written,
					     "hide %s\n", hide_entry->path);
		}
		hash_for_each_rcu(sumh_inject_dirs, bkt, inject_entry, node)
		{
			if (written >= buf_size)
				break;
			written += scnprintf(kbuf + written, buf_size - written,
					     "inject %s\n", inject_entry->dir);
		}
		hash_for_each_rcu(sumh_merge_dirs, bkt, merge_entry, node)
		{
			if (written >= buf_size)
				break;
			written += scnprintf(kbuf + written, buf_size - written,
					     "merge %s %s\n", merge_entry->src,
					     merge_entry->target);
		}
		hash_for_each_rcu(sumh_xattr_sbs, bkt, sb_entry, node)
		{
			if (written >= buf_size)
				break;
			written +=
			    scnprintf(kbuf + written, buf_size - written,
				      "hide_xattr_sb %p\n", sb_entry->sb);
		}
		/* Feature rules: mount_hide, maps_spoof, statfs_spoof, stealth
		 */
		if (sumh_feature_enabled_mask & SUMH_FEATURE_MOUNT_HIDE) {
			if (written < buf_size)
				written += scnprintf(
				    kbuf + written, buf_size - written,
				    "mount_hide enabled mode=%s\n",
				    READ_ONCE(sumh_mount_hide_mode) ==
					    SUMH_MOUNT_HIDE_MODE_AGGRESSIVE
					? "aggressive"
					: "normal");
		}
		if (sumh_feature_enabled_mask & SUMH_FEATURE_MAPS_SPOOF) {
			if (written < buf_size)
				written += scnprintf(kbuf + written,
						     buf_size - written,
						     "maps_spoof enabled\n");
		}
		if (sumh_feature_enabled_mask & SUMH_FEATURE_STATFS_SPOOF) {
			if (written < buf_size)
				written += scnprintf(kbuf + written,
						     buf_size - written,
						     "statfs_spoof enabled\n");
		}
		if (sumh_stealth_enabled) {
			if (written < buf_size)
				written += scnprintf(kbuf + written,
						     buf_size - written,
						     "stealth enabled\n");
		}
		if (sumh_kernel_build_enabled() && written < buf_size)
			written += scnprintf(kbuf + written, buf_size - written,
					     "kernel_build_spoof enabled\n");
		rcu_read_unlock();

		if (copy_to_user(list_arg.buf, kbuf, written)) {
			kvfree(kbuf);
			return -EFAULT;
		}
		list_arg.size = written;
		if (copy_to_user(arg, &list_arg, sizeof(list_arg))) {
			kvfree(kbuf);
			return -EFAULT;
		}
		kvfree(kbuf);
		return 0;
	}

	if (cmd == SUMH_IOC_SET_MIRROR_PATH) {
		/* ABI slot 14 is intentionally retained, but pure virtual mode
		 * no longer owns or consumes a mirror/workdir path. */
		return -EOPNOTSUPP;
	}

	if (cmd == SUMH_IOC_ADD_SPOOF_KSTAT ||
	    cmd == SUMH_IOC_UPDATE_SPOOF_KSTAT)
		return sumh_kstat_set(arg);

	if (cmd == SUMH_IOC_ADD_MAPS_RULE) {
		return sumh_maps_add_rule(arg);
	}
	if (cmd == SUMH_IOC_SET_MANAGED_MAPS)
		return sumh_maps_replace_managed(arg);

	if (cmd == SUMH_IOC_CLEAR_MAPS_RULES) {
		sumh_maps_store_clear();
		return 0;
	}

	if (cmd == SUMH_IOC_SET_MOUNT_HIDE) {
		struct sumh_mount_hide_arg a;
		if (copy_from_user(&a, arg, sizeof(a)))
			return -EFAULT;
		if (a.enable && !sumh_mount_hide_available())
			return -EOPNOTSUPP;
		if (a.enable)
			sumh_feature_enabled_mask |= SUMH_FEATURE_MOUNT_HIDE;
		else
			sumh_feature_enabled_mask &= ~SUMH_FEATURE_MOUNT_HIDE;
		sumh_fake_mi_invalidate_all();
		sumh_log("mount hide %s\n", a.enable ? "enabled" : "disabled");
		/* path_pattern reserved for future custom hide rules */
		return 0;
	}

	if (cmd == SUMH_IOC_SET_MOUNT_HIDE_MODE) {
		int mode;

		if (copy_from_user(&mode, arg, sizeof(mode)))
			return -EFAULT;
		if (mode != SUMH_MOUNT_HIDE_MODE_NORMAL &&
		    mode != SUMH_MOUNT_HIDE_MODE_AGGRESSIVE)
			return -EINVAL;
		/* A namespace is one kernel object across readlink/stat/fstat.
		 * The retired text-only projection could not preserve that
		 * identity.
		 */
		if (mode == SUMH_MOUNT_HIDE_MODE_AGGRESSIVE)
			return -EOPNOTSUPP;
		WRITE_ONCE(sumh_mount_hide_mode, mode);
		sumh_fake_mi_invalidate_all();
		sumh_log("mount hide mode: %s\n",
			 mode == SUMH_MOUNT_HIDE_MODE_AGGRESSIVE ? "aggressive"
								 : "normal");
		return 0;
	}

	if (cmd == SUMH_IOC_SET_MAPS_SPOOF) {
		struct sumh_maps_spoof_arg a;
		if (copy_from_user(&a, arg, sizeof(a)))
			return -EFAULT;
		if (a.enable && !sumh_proc_proxy_available())
			return -EOPNOTSUPP;
		if (a.enable)
			sumh_feature_enabled_mask |= SUMH_FEATURE_MAPS_SPOOF;
		else
			sumh_feature_enabled_mask &= ~SUMH_FEATURE_MAPS_SPOOF;
		/* reserved for future inline rule */
		return 0;
	}

	if (cmd == SUMH_IOC_SET_STATFS_SPOOF) {
		struct sumh_statfs_spoof_arg a;
		if (copy_from_user(&a, arg, sizeof(a)))
			return -EFAULT;
		if (a.enable && !sumh_statfs_view_available())
			return -EOPNOTSUPP;
		if (a.enable)
			sumh_feature_enabled_mask |= SUMH_FEATURE_STATFS_SPOOF;
		else
			sumh_feature_enabled_mask &= ~SUMH_FEATURE_STATFS_SPOOF;
		/* path/spoof_f_type reserved for future custom mappings */
		return 0;
	}

	if (cmd == SUMH_IOC_GET_FEATURES) {
		int features = SUMH_FEATURE_RUNTIME_STATE |
			       SUMH_FEATURE_RUNTIME_SET |
			       SUMH_FEATURE_KERNEL_BUILD_CAS |
			       SUMH_FEATURE_RULE_INVENTORY |
			       SUMH_FEATURE_TEMPORARY_MAPS_DELETE;
		if (sumh_hide_rules_available())
			features |= SUMH_FEATURE_MANAGED_HIDE;
		if (sumh_kernel_build_available())
			features |= SUMH_FEATURE_KERNEL_BUILD_SPOOF;
		features |= SUMH_FEATURE_KSTAT_SPOOF;
		features |= SUMH_FEATURE_MERGE_DIR;
		if (sumh_overlay_xattr_available())
			features |= SUMH_FEATURE_OVERLAY_XATTR_HIDE;
		if (sumh_mount_hide_available())
			features |= SUMH_FEATURE_MOUNT_HIDE |
				    SUMH_FEATURE_FAKE_MOUNTINFO;
		if (sumh_proc_proxy_available())
			features |=
			    SUMH_FEATURE_MAPS_SPOOF | SUMH_FEATURE_MANAGED_MAPS;
		if (sumh_statfs_view_available())
			features |= SUMH_FEATURE_STATFS_SPOOF;
		if (copy_to_user(arg, &features, sizeof(features)))
			return -EFAULT;
		return 0;
	}

	if (cmd == SUMH_IOC_GET_HOOKS) {
		struct sumh_syscall_list_arg list_arg;
		char *kbuf;
		size_t buf_size, written = 0;
		int n;

		if (copy_from_user(&list_arg, arg, sizeof(list_arg)))
			return -EFAULT;

		buf_size = list_arg.size;
		if (buf_size > 4096)
			buf_size = 4096;

		kbuf = kzalloc(buf_size, GFP_KERNEL);
		if (!kbuf)
			return -ENOMEM;

		written += scnprintf(kbuf + written, buf_size - written,
				     "control: KernelSU fd\n");

		/* Path redirect: served entirely through the VFS lookup/vnode
		 * layer; no syscall dispatcher or sys_enter tracepoint remains.
		 */
		n = scnprintf(kbuf + written, buf_size - written,
			      "path: none\n");
		written += n;
		{
			dev_t vnode_dev = sumh_vnode_device();

			n = scnprintf(
			    kbuf + written, buf_size - written,
			    "vnode: live=%u allocated=%llu dev=%u:%u\n",
			    sumh_vnode_live(),
			    (unsigned long long)sumh_vnode_allocated(),
			    MAJOR(vnode_dev), MINOR(vnode_dev));
			written += n;
		}
		n = scnprintf(
		    kbuf + written, buf_size - written, "overlay xattrs: %s\n",
		    sumh_overlay_xattr_available() ? "VFS get/list filter"
						   : "none");
		written += n;

		n = scnprintf(kbuf + written, buf_size - written,
			      "vfs: getattr=iop readdir=fop xattrs=%s\n",
			      sumh_overlay_xattr_available() ? "VFS-filter"
							     : "none");
		written += n;

		n = scnprintf(
		    kbuf + written, buf_size - written,
		    "mountinfo/mounts: %s\n",
		    sumh_mount_hide_available()
			? "fd-install fop proxy (app and isolated readers)"
			: "none");
		written += n;
		n = scnprintf(
		    kbuf + written, buf_size - written, "fake mountinfo: %s\n",
		    sumh_mount_hide_available()
			? "per-open namespace snapshot, native mount IDs"
			: "none");
		written += n;
		n = scnprintf(kbuf + written, buf_size - written,
			      "mount namespace links: native\n");
		written += n;

		/* maps spoof */
		if (sumh_proc_proxy_available())
			n = scnprintf(kbuf + written, buf_size - written,
				      "maps: fd-install fop proxy\n");
		else
			n = scnprintf(kbuf + written, buf_size - written,
				      "maps: none\n");
		written += n;
		if (sumh_statfs_view_available())
			n = scnprintf(kbuf + written, buf_size - written,
				      "statfs/fstatfs: path-aware VFS wrapper "
				      "(entries=%llu spoofs=%llu)\n",
				      (unsigned long long)atomic64_read(
					  &sumh_hook_stats.statfs_entries),
				      (unsigned long long)atomic64_read(
					  &sumh_hook_stats.statfs_spoofs));
		else
			n = scnprintf(kbuf + written, buf_size - written,
				      "statfs: none\n");
		written += n;
		written +=
		    sumh_proc_memory_report(kbuf + written, buf_size - written);

		list_arg.size = written;
		if (copy_to_user(arg, &list_arg, sizeof(list_arg))) {
			kfree(kbuf);
			return -EFAULT;
		}
		if (written && copy_to_user(list_arg.buf, kbuf, written)) {
			kfree(kbuf);
			return -EFAULT;
		}
		kfree(kbuf);
		return 0;
	}

	/* Commands that use sumh_syscall_arg */
	if (copy_from_user(&req, arg, sizeof(req)))
		return -EFAULT;

	if (req.src) {
		src = strndup_user(req.src, PAGE_SIZE);
		if (IS_ERR(src))
			return PTR_ERR(src);
	}
	if (req.target) {
		target = strndup_user(req.target, PAGE_SIZE);
		if (IS_ERR(target)) {
			kfree(src);
			return PTR_ERR(target);
		}
	}
	if (src && ksu_sucompat_vfs_reserved_path(src)) {
		kfree(src);
		kfree(target);
		return -EPERM;
	}

	switch (cmd) {
	case SUMH_IOC_ADD_MERGE_RULE: {
		struct sumh_merge_entry *me = NULL, *existing;
		char *mat_src = NULL, *mat_tgt = NULL;
		struct path target_path;

		if (!src || !target) {
			ret = -EINVAL;
			break;
		}
		ret = sumh_capture_merge_target(target, &target_path);
		if (ret)
			break;
		/* Preserve target validation and materialization-copy error
		 * order. */
		mat_src = kstrdup(src, GFP_KERNEL);
		mat_tgt = kstrdup(target, GFP_KERNEL);
		if (!mat_src || !mat_tgt) {
			ret = -ENOMEM;
			goto merge_done;
		}
		hash = full_name_hash(NULL, src, strlen(src));
		/* A duplicate must not consult a newly inaccessible visible
		 * path or prepare new FOP infrastructure. Recheck before final
		 * publication. */
		mutex_lock(&sumh_config_mutex);
		hlist_for_each_entry(
		    existing, &sumh_merge_dirs[hash_min(hash, SUMH_HASH_BITS)],
		    node)
		{
			if (!strcmp(existing->src, src) &&
			    !strcmp(existing->target, target)) {
				ret = -EEXIST;
				break;
			}
		}
		mutex_unlock(&sumh_config_mutex);
		if (ret)
			goto merge_done;
		me = kzalloc(sizeof(*me), GFP_KERNEL);
		if (!me) {
			ret = -ENOMEM;
			goto merge_done;
		}
		ret = sumh_inject_prepare_merge(src, &me->resolved_src,
						me->inject_claims);
		if (ret)
			goto merge_done;
		mutex_lock(&sumh_config_mutex);
		hlist_for_each_entry(
		    existing, &sumh_merge_dirs[hash_min(hash, SUMH_HASH_BITS)],
		    node)
		{
			if (!strcmp(existing->src, src) &&
			    !strcmp(existing->target, target)) {
				ret = -EEXIST;
				break;
			}
		}
		if (!ret)
			ret = sumh_inject_attach_locked(me->inject_claims, 2);
		if (!ret) {
			me->src = src;
			me->target = target;
			me->target_path = target_path;
			path_get(&me->target_path);
			hlist_add_head_rcu(
			    &me->node,
			    &sumh_merge_dirs[hash_min(hash, SUMH_HASH_BITS)]);
			src = NULL;
			target = NULL;
		}
		mutex_unlock(&sumh_config_mutex);
		if (ret)
			goto merge_done;
		sumh_inject_finish_prepare(me->inject_claims[0]);
		sumh_inject_finish_prepare(me->inject_claims[1]);
		sumh_log("add merge rule: src=%s, target=%s\n", me->src,
			 me->target);
		me = NULL; /* Published prefix owns claims even on a child
			      failure. */
		ret = sumh_materialize_merge(mat_src, mat_tgt, &target_path, 0);
	merge_done:
		if (me) {
			sumh_inject_destroy(me->inject_claims[0]);
			sumh_inject_destroy(me->inject_claims[1]);
			kfree(me->resolved_src);
			kfree(me);
		}
		kfree(mat_src);
		kfree(mat_tgt);
		path_put(&target_path);
		break;
	}

	case SUMH_IOC_ADD_RULE: {
		char *parent_dir = NULL, *slash;
		struct sumh_entry *new_entry = NULL;
		struct path native_parent = {}, captured_source = {};
		struct sumh_dh_spec spec = {.kind = SUMH_DH_OWNER_MAPPING};

		if (!src || !target) {
			ret = -EINVAL;
			break;
		}
		ret = sumh_resolve_rule_path(&src, NULL, NULL);
		if (ret)
			break;
		parent_dir = kstrdup(src, GFP_KERNEL);
		if (!parent_dir) {
			ret = -ENOMEM;
			break;
		}
		slash = strrchr(parent_dir, '/');
		if (slash == parent_dir)
			slash[1] = '\0';
		else
			*slash = '\0';
		new_entry = kzalloc(sizeof(*new_entry), GFP_KERNEL);
		if (!new_entry) {
			ret = -ENOMEM;
			goto add_rule_done;
		}
		new_entry->src = kstrdup(src, GFP_KERNEL);
		new_entry->target = kstrdup(target, GFP_KERNEL);
		new_entry->type = req.type;
		new_entry->src_hash = full_name_hash(NULL, src, strlen(src));
		if (!new_entry->src || !new_entry->target) {
			ret = -ENOMEM;
			goto add_rule_done;
		}
		ret = sumh_entry_capture_source(new_entry, target);
		if (ret)
			goto add_rule_done;
		if (new_entry->source_nofollow_path_valid) {
			spec.source = &new_entry->source_nofollow_path;
			spec.v_ino = new_entry->nofollow_visible_ino;
			spec.flags = SUMH_VNODE_F_LNK;
		} else {
			spec.source = &new_entry->source_path;
			spec.v_ino = new_entry->visible_ino;
			if (S_ISDIR(new_entry->source_mode))
				spec.flags = SUMH_VNODE_F_DIR;
			else if (!S_ISREG(new_entry->source_mode))
				spec.flags = SUMH_VNODE_F_SPECIAL;
		}
		ret = sumh_dh_prepare_path(src, &spec, &new_entry->dh_claim,
					   &native_parent);
		if (ret)
			goto add_rule_done;
		ret = sumh_inject_prepare_observed(
		    parent_dir, native_parent.dentry ? &native_parent : NULL,
		    SUMH_INJECT_MISSING_NONE, &new_entry->inject_claim);
		if (ret)
			goto add_rule_done;
		captured_source = new_entry->source_path;
		path_get(&captured_source);
		ret = sumh_store_upsert(new_entry, NULL);
		if (ret)
			goto add_rule_done;
		sumh_inject_finish_prepare(new_entry->inject_claim);
		new_entry = NULL;
		/* Legacy source spoof marking remains separate from ownership.
		 */
		(void)sumh_iop_mark_spoof(d_inode(captured_source.dentry));
	add_rule_done:
		if (native_parent.dentry)
			path_put(&native_parent);
		if (captured_source.dentry)
			path_put(&captured_source);
		if (new_entry) {
			sumh_dh_abort(new_entry->dh_claim);
			sumh_inject_destroy(new_entry->inject_claim);
			sumh_entry_release_source(new_entry);
			kfree(new_entry->src);
			kfree(new_entry->target);
			kfree(new_entry->source_canonical);
			kfree(new_entry);
		}
		kfree(parent_dir);
		break;
	}

	case SUMH_IOC_HIDE_RULE: {
		struct sumh_hide_entry *new_hide = NULL, *previous = NULL;
		struct inode *target_inode = NULL, *parent_inode = NULL;
		struct sumh_dh_spec spec = {.kind = SUMH_DH_OWNER_LEGACY_HIDE};
		struct sumh_dh_change change;
		struct sumh_dh_retire retired;

		sumh_dh_retire_init(&retired, false);
		ret =
		    sumh_resolve_rule_path(&src, &target_inode, &parent_inode);
		if (ret)
			break;
		new_hide = kzalloc(sizeof(*new_hide), GFP_KERNEL);
		if (!new_hide) {
			ret = -ENOMEM;
			goto hide_done;
		}
		new_hide->path = kstrdup(src, GFP_KERNEL);
		if (!new_hide->path) {
			ret = -ENOMEM;
			goto hide_done;
		}
		new_hide->storage_managed =
		    sumh_hide_storage_parent(parent_inode);
		ret =
		    sumh_dh_prepare_path(src, &spec, &new_hide->dh_claim, NULL);
		if (ret)
			goto hide_done;
		hash = full_name_hash(NULL, src, strlen(src));
		new_hide->path_hash = hash;
		mutex_lock(&sumh_config_mutex);
		hlist_for_each_entry(
		    hide_entry,
		    &sumh_hide_paths[hash_min(hash, SUMH_HASH_BITS)], node)
		{
			if (hide_entry->path_hash == hash &&
			    !strcmp(hide_entry->path, src)) {
				previous = hide_entry;
				break;
			}
		}
		if (!previous && atomic_read(&sumh_hide_count) == INT_MAX) {
			ret = -EOVERFLOW;
			goto hide_unlock;
		}
		ret = sumh_dh_change_begin(new_hide->dh_claim,
					   previous ? previous->dh_claim : NULL,
					   &change);
		if (ret)
			goto hide_unlock;
		sumh_dh_change_commit(&change, &retired);
		hlist_add_head_rcu(
		    &new_hide->node,
		    &sumh_hide_paths[hash_min(hash, SUMH_HASH_BITS)]);
		if (previous) {
			hlist_del_rcu(&previous->node);
			call_rcu(&previous->rcu, sumh_hide_entry_free_rcu);
		} else {
			set_bit(jhash(src, strlen(src), 0) &
				    (SUMH_BLOOM_SIZE - 1),
				sumh_hide_bloom);
			set_bit(jhash(src, strlen(src), 1) &
				    (SUMH_BLOOM_SIZE - 1),
				sumh_hide_bloom);
			atomic_inc(&sumh_hide_count);
		}
		new_hide = NULL;
		sumh_dh_change_end();
	hide_unlock:
		mutex_unlock(&sumh_config_mutex);
		sumh_dh_retire_finish(&retired);
		if (!ret) {
			if (target_inode)
				sumh_mark_inode_hidden(target_inode);
			if (parent_inode && parent_inode->i_mapping)
				set_bit(AS_FLAGS_SUMH_DIR_HAS_HIDDEN,
					&parent_inode->i_mapping->flags);
		}
	hide_done:
		if (new_hide) {
			sumh_dh_abort(new_hide->dh_claim);
			kfree(new_hide->path);
			kfree(new_hide);
		}
		if (target_inode)
			iput(target_inode);
		if (parent_inode)
			iput(parent_inode);
		break;
	}

	case SUMH_IOC_HIDE_OVERLAY_XATTRS:
		ret = src ? sumh_overlay_xattr_mark(src) : -EINVAL;
		break;

	case SUMH_IOC_DEL_RULE: {
		struct sumh_entry *selected = NULL;
		struct sumh_hide_entry *selected_hide = NULL;
		struct sumh_dh_retire retired;
		char *original = NULL;
		bool normalized = false, fallback = false;

		if (!src || src[0] != '/') {
			ret = -EINVAL;
			break;
		}
		sumh_dh_retire_init(&retired, false);
	del_select:
		hash = full_name_hash(NULL, src, strlen(src));
		mutex_lock(&sumh_config_mutex);
		hlist_for_each_entry(
		    entry, &sumh_paths[hash_min(hash, SUMH_HASH_BITS)], node)
		{
			if (entry->src_hash == hash &&
			    !strcmp(entry->src, src)) {
				selected = entry;
				break;
			}
		}
		if (!selected) {
			hlist_for_each_entry(
			    hide_entry,
			    &sumh_hide_paths[hash_min(hash, SUMH_HASH_BITS)],
			    node)
			{
				if (hide_entry->path_hash == hash &&
				    !strcmp(hide_entry->path, src)) {
					selected_hide = hide_entry;
					break;
				}
			}
		}
		if (!selected && !selected_hide &&
		    sumh_inject_key_present_locked(src)) {
			ret = -EBUSY;
			goto del_unlock;
		}
		if (!selected && !selected_hide && !normalized) {
			mutex_unlock(&sumh_config_mutex);
			/* Old missing-parent and materialized rows may retain
			 * lexical spelling. Prefer that exact stored key before
			 * normalization. */
			original = kstrdup(src, GFP_KERNEL);
			if (!original) {
				ret = -ENOMEM;
				break;
			}
			ret = sumh_normalize_rule_key(src);
			normalized = true;
			goto del_select;
		}
		if (!selected && !selected_hide && !fallback) {
			mutex_unlock(&sumh_config_mutex);
			/* Only this fallback resolves CURRENT aliases. A
			 * selected exact row never depends on native lookup for
			 * ownership withdrawal. Keep the original input:
			 * lexical '..' can cross a symlink. */
			kfree(src);
			src = original;
			original = NULL;
			ret = sumh_resolve_rule_path(&src, NULL, NULL);
			if (ret)
				break;
			fallback = true;
			goto del_select;
		}
		if (selected) {
			hlist_del_rcu(&selected->node);
			sumh_dh_detach(selected->dh_claim, &retired);
			sumh_inject_detach_locked(selected->inject_claim);
			atomic_dec(&sumh_rule_count);
			call_rcu(&selected->rcu, sumh_entry_free_rcu);
		} else if (selected_hide) {
			hlist_del_rcu(&selected_hide->node);
			sumh_dh_detach(selected_hide->dh_claim, &retired);
			atomic_dec(&sumh_hide_count);
			call_rcu(&selected_hide->rcu, sumh_hide_entry_free_rcu);
		}
	del_unlock:
		mutex_unlock(&sumh_config_mutex);
		sumh_dh_retire_finish(&retired);
		if (!ret)
			sumh_clear_inode_flags_for_path(src,
							AS_FLAGS_SUMH_HIDE);
		kfree(original);
		break;
	}

	default:
		ret = -EINVAL;
		break;
	}

	kfree(src);
	kfree(target);
	return ret;
}

/* ======================================================================
 * Part 16: Ioctl Handler
 * ====================================================================== */

/*
 * Rule updates have side effects outside sumh_config_mutex (inode/dentry
 * overrides and injected-directory state). Serialize the control plane so a
 * concurrent CLEAR_ALL cannot return while an older update is still applying
 * those side effects or re-enable the module afterwards.
 */
long sumh_handle_ioctl(unsigned int cmd, void __user *arg)
{
	long ret = sumh_control_begin();

	if (ret)
		return ret;

	mutex_lock(&sumh_mutation_mutex);
	/* A pure snapshot must not trigger hide initialization, su rebinding,
	 * per-command tracing writes or generation/error updates. */
	if (cmd == SUMH_IOC_GET_RUNTIME_STATE ||
	    cmd == SUMH_IOC_GET_RULE_INVENTORY) {
		ret = cmd == SUMH_IOC_GET_RUNTIME_STATE
			  ? sumh_runtime_state_get(arg)
			  : sumh_rule_inventory_get(arg);
		mutex_unlock(&sumh_mutation_mutex);
		sumh_control_end();
		return ret;
	}
	atomic_long_set(&sumh_ioctl_tgid, (long)task_tgid_vnr(current));
	switch (cmd) {
	case SUMH_IOC_SET_KERNEL_BUILD_CAS:
		ret = sumh_kernel_build_conditional_set(arg);
		break;
	case SUMH_IOC_SET_RUNTIME_FEATURE:
		ret = sumh_runtime_feature_set(arg);
		break;
	case SUMH_IOC_DELETE_TEMPORARY_MAPS:
		ret = sumh_maps_delete_temporary(arg);
		break;
	case SUMH_IOC_USER_HIDE_UPSERT:
	case SUMH_IOC_USER_HIDE_DELETE:
	case SUMH_IOC_USER_HIDE_QUERY:
	case SUMH_IOC_USER_HIDE_RETRY:
	case SUMH_IOC_USER_HIDE_CLEAR:
		ret = sumh_hide_rules_ioctl(cmd, arg);
		break;
	case SUMH_IOC_GET_VERSION:
	case SUMH_IOC_GET_ENABLED:
	case SUMH_IOC_SET_KERNEL_BUILD:
	case SUMH_IOC_GET_KERNEL_BUILD:
	case SUMH_IOC_GET_ORIGINAL_KERNEL_BUILD:
	case SUMH_IOC_SET_ENABLED:
	case SUMH_IOC_ADD_RULE:
	case SUMH_IOC_DEL_RULE:
	case SUMH_IOC_HIDE_RULE:
	case SUMH_IOC_CLEAR_ALL:
	case SUMH_IOC_LIST_RULES:
	case SUMH_IOC_SET_DEBUG:
	case SUMH_IOC_REORDER_MNT_ID:
	case SUMH_IOC_SET_STEALTH:
	case SUMH_IOC_HIDE_OVERLAY_XATTRS:
	case SUMH_IOC_ADD_MERGE_RULE:
	case SUMH_IOC_SET_MIRROR_PATH:
	case SUMH_IOC_GET_HOOKS:
	case SUMH_IOC_ADD_MAPS_RULE:
	case SUMH_IOC_SET_MANAGED_MAPS:
	case SUMH_IOC_CLEAR_MAPS_RULES:
	case SUMH_IOC_GET_FEATURES:
	case SUMH_IOC_SET_MOUNT_HIDE:
	case SUMH_IOC_SET_MOUNT_HIDE_MODE:
	case SUMH_IOC_SET_MAPS_SPOOF:
	case SUMH_IOC_SET_STATFS_SPOOF:
	case SUMH_IOC_ADD_SPOOF_KSTAT:
	case SUMH_IOC_UPDATE_SPOOF_KSTAT:
		ret = sumh_dispatch_cmd(cmd, arg);
		break;
	default:
		ret = -EINVAL;
		break;
	}
	sumh_runtime_record_result(cmd, ret);
	atomic_long_set(&sumh_ioctl_tgid, 0);
	if (ret >= 0 && ksu_sucompat_vfs_enabled() &&
	    (cmd == SUMH_IOC_SET_ENABLED || cmd == SUMH_IOC_CLEAR_ALL ||
	     cmd == SUMH_IOC_ADD_RULE || cmd == SUMH_IOC_ADD_MERGE_RULE ||
	     cmd == SUMH_IOC_DEL_RULE)) {
		int refresh = ksu_sucompat_vfs_refresh();
		if (refresh)
			pr_warn("sumh: failed to rebind internal su node: %d\n",
				refresh);
	}
	mutex_unlock(&sumh_mutation_mutex);
	sumh_control_end();
	return ret;
}
