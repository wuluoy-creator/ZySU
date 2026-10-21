#include "sumh_sop_shadow.h"
#include "sumh_dirhijack.h"
#include "sumh_runtime.h"

#include <linux/fs.h>
#include <linux/hashtable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#define SUMH_SOP_HASH_BITS 8

struct sumh_sop_meta {
	struct super_block *sb;
	const struct super_operations *orig_sop;
	struct super_operations shadow_sop;
	atomic_t vnode_live;
	atomic_t callback_active;
	unsigned int dh_clients;
	bool accepting_vnodes;
	bool restoring;
	bool sb_active_held;
	bool module_pin_held;
	struct hlist_node node;
};

static DEFINE_HASHTABLE(sumh_sop_table, SUMH_SOP_HASH_BITS);
/* Leaf lock: never allocate, wait, take s_umount, or deactivate under it. */
static DEFINE_SPINLOCK(sumh_sop_lock);
/* Serializes install and the sleepable restore/drain/release sequence. */
static DEFINE_MUTEX(sumh_sop_manage_lock);
static atomic_t sumh_sop_active = ATOMIC_INIT(0);
static atomic_t sumh_sop_live = ATOMIC_INIT(0);
static atomic_t sumh_sop_vnodes = ATOMIC_INIT(0);
static DECLARE_WAIT_QUEUE_HEAD(sumh_sop_wait);
static bool sumh_sop_ready;
static bool sumh_sop_reap_work_enabled;

static void sumh_sop_reap_workfn(struct work_struct *work);
static DECLARE_WORK(sumh_sop_reap_work, sumh_sop_reap_workfn);

static struct sumh_sop_meta *sumh_sop_lookup_rcu(struct super_block *sb)
{
	struct sumh_sop_meta *m;

	hash_for_each_possible_rcu(sumh_sop_table, m, node, (unsigned long)sb)
	{
		if (m->sb == sb)
			return m;
	}
	return NULL;
}

static struct sumh_sop_meta *sumh_sop_lookup_locked(struct super_block *sb)
{
	struct sumh_sop_meta *m;

	hash_for_each_possible(sumh_sop_table, m, node, (unsigned long)sb)
	{
		if (m->sb == sb)
			return m;
	}
	return NULL;
}

/* The hash entry stays published until after pointer restore + double
 * Tasks-RCU, so every stale callback can still resolve its original vector. */
static struct sumh_sop_meta *
sumh_sop_callback_enter(struct super_block *sb,
			const struct super_operations **orig)
{
	struct sumh_sop_meta *m;

	*orig = NULL;
	atomic_inc(&sumh_sop_active);
	rcu_read_lock();
	m = sumh_sop_lookup_rcu(sb);
	if (m) {
		atomic_inc(&m->callback_active);
		*orig = m->orig_sop;
	}
	rcu_read_unlock();
	return m;
}

static void sumh_sop_callback_exit(struct sumh_sop_meta *m)
{
	if (m && atomic_dec_and_test(&m->callback_active))
		wake_up_all(&sumh_sop_wait);
	if (atomic_dec_and_test(&sumh_sop_active))
		wake_up_all(&sumh_sop_wait);
}

SUMH_NOCFI static void sumh_sop_destroy_inode(struct inode *inode)
{
	struct sumh_sop_meta *m;
	const struct super_operations *orig;

	m = sumh_sop_callback_enter(inode ? inode->i_sb : NULL, &orig);
	sumh_dh_reclaim_destroy_inode(inode, orig);
	sumh_sop_callback_exit(m);
}

SUMH_NOCFI static void sumh_sop_evict_inode(struct inode *inode)
{
	struct sumh_sop_meta *m;
	const struct super_operations *orig;

	m = sumh_sop_callback_enter(inode ? inode->i_sb : NULL, &orig);
	sumh_dh_reclaim_evict_inode(inode, orig);
	sumh_sop_callback_exit(m);
}

SUMH_NOCFI static int sumh_sop_drop_inode(struct inode *inode)
{
	struct sumh_sop_meta *m;
	const struct super_operations *orig;
	int ret;

	m = sumh_sop_callback_enter(inode ? inode->i_sb : NULL, &orig);
	ret = sumh_dh_reclaim_drop_inode(inode, orig);
	sumh_sop_callback_exit(m);
	return ret;
}

static void sumh_sop_drop_install_refs(struct super_block *sb, bool active_held,
				       bool module_held)
{
	/* A control fd plus the bootstrap lifecycle pin remains held while this
	 * helper runs, so module_put() cannot unmap the function returning
	 * here. */
	if (active_held)
		deactivate_super(sb);
	if (module_held)
		module_put(THIS_MODULE);
}

int sumh_sop_shadow_register_dh(struct super_block *sb)
{
	struct sumh_sop_meta *m, *spare;
	const struct super_operations *orig;
	bool active_held = false;
	bool module_held = false;
	int ret = 0;

	if (!sb || !READ_ONCE(sb->s_op))
		return -EINVAL;
	if (!READ_ONCE(sumh_sop_ready))
		return -ESHUTDOWN;

	spare = kzalloc(sizeof(*spare), GFP_KERNEL);
	if (!spare)
		return -ENOMEM;

	mutex_lock(&sumh_sop_manage_lock);
	spin_lock(&sumh_sop_lock);
	m = sumh_sop_lookup_locked(sb);
	if (m) {
		if (!READ_ONCE(sumh_sop_ready) || m->restoring ||
		    READ_ONCE(sb->s_op) != &m->shadow_sop) {
			ret = -ESHUTDOWN;
		} else if (m->dh_clients == UINT_MAX) {
			ret = -EOVERFLOW;
		} else {
			m->dh_clients++;
			m->accepting_vnodes = true;
		}
		spin_unlock(&sumh_sop_lock);
		goto out_unlock;
	}
	spin_unlock(&sumh_sop_lock);

	/* s_umount excludes a concurrent shutdown transition while the extra
	 * s_active reference is acquired. The caller still owns a live path. */
	down_read(&sb->s_umount);
	if (!READ_ONCE(sumh_sop_ready) || !(READ_ONCE(sb->s_flags) & SB_BORN) ||
	    !atomic_inc_not_zero(&sb->s_active)) {
		ret = -ESHUTDOWN;
		goto out_s_umount;
	}
	active_held = true;
	if (!try_module_get(THIS_MODULE)) {
		ret = -ESHUTDOWN;
		goto out_s_umount;
	}
	module_held = true;

	orig = READ_ONCE(sb->s_op);
	if (!orig) {
		ret = -EINVAL;
		goto out_s_umount;
	}
	spare->sb = sb;
	spare->orig_sop = orig;
	spare->shadow_sop = *orig;
	atomic_set(&spare->vnode_live, 0);
	atomic_set(&spare->callback_active, 0);
	spare->dh_clients = 1;
	spare->accepting_vnodes = true;
	spare->sb_active_held = true;
	spare->module_pin_held = true;

	if (!orig->destroy_inode && !orig->free_inode &&
	    !sumh_free_inode_nonrcu_ptr) {
		ret = -EOPNOTSUPP;
		goto out_s_umount;
	}
	/* VFS invokes ->destroy_inode synchronously and, when
	 * ->free_inode is also present, records that callback in the inode
	 * before queueing its own RCU callback.  Always use the synchronous
	 * stage for the sleepable vnode cleanup, while leaving a filesystem's
	 * original deferred container deallocator untouched.  For the
	 * generic-inode case, supplying free_inode_nonrcu makes VFS queue the
	 * same generic free it would have used without our newly-added destroy
	 * callback.  No module-owned callback is ever stored in
	 * inode->free_inode.
	 */
	spare->shadow_sop.destroy_inode = sumh_sop_destroy_inode;
	if (!orig->destroy_inode && !orig->free_inode)
		spare->shadow_sop.free_inode = sumh_free_inode_nonrcu_ptr;
	spare->shadow_sop.evict_inode = sumh_sop_evict_inode;
	spare->shadow_sop.drop_inode = sumh_sop_drop_inode;

	spin_lock(&sumh_sop_lock);
	if (!READ_ONCE(sumh_sop_ready) || READ_ONCE(sb->s_op) != orig ||
	    sumh_sop_lookup_locked(sb)) {
		ret = -EAGAIN;
	} else {
		hash_add_rcu(sumh_sop_table, &spare->node, (unsigned long)sb);
		atomic_inc(&sumh_sop_live);
		smp_wmb();
		smp_store_release(&sb->s_op, &spare->shadow_sop);
		spare = NULL;
		active_held = false;
		module_held = false;
	}
	spin_unlock(&sumh_sop_lock);

out_s_umount:
	up_read(&sb->s_umount);
	if (ret)
		sumh_sop_drop_install_refs(sb, active_held, module_held);
out_unlock:
	mutex_unlock(&sumh_sop_manage_lock);
	kfree(spare);
	if (!ret)
		sumh_log("sop_shadow: client registered on sb %p\n", sb);
	return ret;
}

void sumh_sop_shadow_unregister_dh(struct super_block *sb)
{
	struct sumh_sop_meta *m;

	if (!sb)
		return;
	spin_lock(&sumh_sop_lock);
	m = sumh_sop_lookup_locked(sb);
	if (m) {
		if (WARN_ON_ONCE(!m->dh_clients)) {
			m->accepting_vnodes = false;
		} else if (!--m->dh_clients) {
			m->accepting_vnodes = false;
		}
	}
	spin_unlock(&sumh_sop_lock);
	wake_up_all(&sumh_sop_wait);
}

bool sumh_sop_vnode_get(struct super_block *sb)
{
	struct sumh_sop_meta *m;
	bool got = false;

	if (!sb)
		return false;
	spin_lock(&sumh_sop_lock);
	m = sumh_sop_lookup_locked(sb);
	if (m && m->accepting_vnodes && !m->restoring) {
		atomic_inc(&m->vnode_live);
		atomic_inc(&sumh_sop_vnodes);
		got = true;
	}
	spin_unlock(&sumh_sop_lock);
	return got;
}

void sumh_sop_vnode_put(struct super_block *sb)
{
	struct sumh_sop_meta *m;

	if (!sb)
		return;
	spin_lock(&sumh_sop_lock);
	m = sumh_sop_lookup_locked(sb);
	if (WARN_ON_ONCE(!m || atomic_read(&m->vnode_live) <= 0)) {
		spin_unlock(&sumh_sop_lock);
		return;
	}
	if (atomic_dec_and_test(&m->vnode_live) && !m->dh_clients &&
	    !m->restoring && sumh_sop_reap_work_enabled) {
		/* schedule_work() is atomic-safe; queueing under this lock
		 * pairs with exit's disable-before-cancel sequence.
		 */
		schedule_work(&sumh_sop_reap_work);
	}
	atomic_dec(&sumh_sop_vnodes);
	spin_unlock(&sumh_sop_lock);
	/* vnode_put() runs from the synchronous destroy_inode trampoline.
	 * Reaping takes s_umount, waits for callbacks, synchronizes RCU and may
	 * deactivate the superblock, so only request it here and do the work in
	 * process context.
	 */
	wake_up_all(&sumh_sop_wait);
}

static struct sumh_sop_meta *sumh_sop_take_reap_candidate(void)
{
	struct sumh_sop_meta *m;
	int bkt;

	spin_lock(&sumh_sop_lock);
	hash_for_each(sumh_sop_table, bkt, m, node)
	{
		if (!m->restoring && !m->dh_clients &&
		    !atomic_read(&m->vnode_live)) {
			m->accepting_vnodes = false;
			m->restoring = true;
			spin_unlock(&sumh_sop_lock);
			return m;
		}
	}
	spin_unlock(&sumh_sop_lock);
	return NULL;
}

static void sumh_sop_reap_meta(struct sumh_sop_meta *m)
{
	struct super_block *sb = m->sb;
	bool active_held;
	bool module_held;

	/* The retained s_active reference means shutdown cannot be in progress.
	 * Take s_umount read-side anyway so the publication contract is
	 * explicit. */
	down_read(&sb->s_umount);
	spin_lock(&sumh_sop_lock);
	if (READ_ONCE(sb->s_op) == &m->shadow_sop)
		smp_store_release(&sb->s_op, m->orig_sop);
	else
		WARN_ON_ONCE(READ_ONCE(sb->s_op) != m->orig_sop);
	spin_unlock(&sumh_sop_lock);
	up_read(&sb->s_umount);

	/* A caller may have loaded shadow_sop just before the restore. */
	synchronize_rcu_tasks();
	wait_event(sumh_sop_wait, !atomic_read(&m->callback_active));
	/* Cover callback epilogues after their active count reached zero. */
	synchronize_rcu_tasks();

	spin_lock(&sumh_sop_lock);
	WARN_ON_ONCE(m->dh_clients || atomic_read(&m->vnode_live));
	hash_del_rcu(&m->node);
	atomic_dec(&sumh_sop_live);
	active_held = m->sb_active_held;
	module_held = m->module_pin_held;
	m->sb_active_held = false;
	m->module_pin_held = false;
	spin_unlock(&sumh_sop_lock);
	synchronize_rcu();

	sumh_log("sop_shadow: retired sb %p\n", sb);
	kfree(m);
	/* May run the original generic_shutdown_super() if userspace detached
	 * the last mount while SUMH held the extra active reference. */
	sumh_sop_drop_install_refs(sb, active_held, module_held);
	wake_up_all(&sumh_sop_wait);
}

void sumh_sop_shadow_reap(void)
{
	struct sumh_sop_meta *m;

	mutex_lock(&sumh_sop_manage_lock);
	while ((m = sumh_sop_take_reap_candidate()) != NULL)
		sumh_sop_reap_meta(m);
	mutex_unlock(&sumh_sop_manage_lock);
}

static void sumh_sop_reap_workfn(struct work_struct *work)
{
	(void)work;
	sumh_sop_shadow_reap();
}

int sumh_sop_shadow_init(void)
{
	hash_init(sumh_sop_table);
	atomic_set(&sumh_sop_active, 0);
	atomic_set(&sumh_sop_live, 0);
	atomic_set(&sumh_sop_vnodes, 0);
	WRITE_ONCE(sumh_sop_reap_work_enabled, true);
	WRITE_ONCE(sumh_sop_ready, true);
	pr_info("sumh: sop_shadow initialized\n");
	return 0;
}

void sumh_sop_shadow_stop_new(void)
{
	struct sumh_sop_meta *m;
	int bkt;

	WRITE_ONCE(sumh_sop_ready, false);
	spin_lock(&sumh_sop_lock);
	hash_for_each(sumh_sop_table, bkt, m, node) m->accepting_vnodes = false;
	spin_unlock(&sumh_sop_lock);
	sumh_sop_shadow_reap();
	/* Pair with a late final vnode_put() that may have queued the sleepable
	 * reap while stop_new() was scanning the table.
	 */
	flush_work(&sumh_sop_reap_work);
}

void sumh_sop_shadow_exit(void)
{
	sumh_sop_shadow_stop_new();
	/* No live owner may remain at module exit (each one pins THIS_MODULE).
	 * Disable new requests under the same lock used by vnode_put(), then
	 * drain the reusable static work item before module text can disappear.
	 */
	spin_lock(&sumh_sop_lock);
	WRITE_ONCE(sumh_sop_reap_work_enabled, false);
	spin_unlock(&sumh_sop_lock);
	cancel_work_sync(&sumh_sop_reap_work);
	sumh_sop_shadow_reap();
	WARN_ON_ONCE(atomic_read(&sumh_sop_live));
	WARN_ON_ONCE(atomic_read(&sumh_sop_active));
	WARN_ON_ONCE(atomic_read(&sumh_sop_vnodes));
	synchronize_rcu();
	pr_info("sumh: sop_shadow exited\n");
}
