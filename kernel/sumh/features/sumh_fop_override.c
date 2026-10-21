#include "sumh_entrypoints.h"
#include "sumh_dirhijack.h"
#include "sumh_fop_bridge.h"
#include "sumh_fop_override.h"
#include "sumh_runtime.h"
#include "sumh_vfs_hooks.h"

#include <linux/hashtable.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/srcu.h>

#define SUMH_FOP_HASH_BITS 10
#define SUMH_FOP_TEMPLATE_HASH_BITS 6

struct sumh_fop_template {
	const struct file_operations *orig_fop;
	struct file_operations ingress_fop;
	struct file_operations shadow_fop;
	struct sumh_fop_bridge_entry bridge;
	struct hlist_node node;
	struct list_head retire_node;
};

struct sumh_fop_meta {
	struct inode *inode;
	/* A cached inode reference must not outlive its superblock. This active
	 * reference is independent of dirhijack clients and retained until the
	 * inode binding is retired; it does not pin a mount. */
	struct super_block *sb;
	struct sumh_fop_template *template;
	sumh_fop_iterate_client_fn iterate_client;
	void *iterate_client_data;
	struct hlist_node node;
	struct list_head retire_node;
};

static DEFINE_HASHTABLE(sumh_fop_table, SUMH_FOP_HASH_BITS);
static DEFINE_HASHTABLE(sumh_fop_templates, SUMH_FOP_TEMPLATE_HASH_BITS);
static DEFINE_SPINLOCK(sumh_fop_lock);
DEFINE_STATIC_SRCU(sumh_fop_client_srcu);
static bool sumh_fop_override_ready;
static bool sumh_fop_override_stopped;

SUMH_NOCFI static int sumh_shadow_iterate_shared(struct file *file,
						 struct dir_context *ctx);

static struct sumh_fop_meta *sumh_fop_lookup_rcu(struct inode *inode)
{
	struct sumh_fop_meta *m;

	hash_for_each_possible_rcu(sumh_fop_table, m, node,
				   (unsigned long)inode)
	{
		if (m->inode == inode)
			return m;
	}
	return NULL;
}

static void sumh_fop_put_orig(const struct file_operations *fop)
{
	if (fop && fop->owner && fop->owner != THIS_MODULE)
		module_put(fop->owner);
}

static void sumh_fop_meta_free(struct sumh_fop_meta *m)
{
	if (!m)
		return;
	/* iput may invoke filesystem callbacks or evict the inode. Keep the
	 * exact superblock alive until it finishes, including failed installs.
	 * Both releases run outside the FOP spinlock and s_umount. */
	if (m->inode)
		iput(m->inode);
	if (m->sb)
		deactivate_super(m->sb);
	kfree(m);
}

static void sumh_fop_template_free(struct sumh_fop_template *template)
{
	if (!template)
		return;
	sumh_fop_put_orig(template->orig_fop);
	kfree(template);
}

static struct sumh_fop_template *
sumh_fop_template_lookup_locked(const struct file_operations *orig)
{
	struct sumh_fop_template *template;

	hash_for_each_possible(sumh_fop_templates, template, node,
			       (unsigned long)orig)
	{
		if (template->orig_fop == orig)
			return template;
	}
	return NULL;
}

static struct sumh_fop_template *
sumh_fop_template_alloc(const struct file_operations *orig)
{
	struct sumh_fop_template *template;

	template = kzalloc(sizeof(*template), GFP_KERNEL);
	if (!template)
		return ERR_PTR(-ENOMEM);
	if (orig->owner && orig->owner != THIS_MODULE &&
	    !try_module_get(orig->owner)) {
		kfree(template);
		return ERR_PTR(-ENODEV);
	}
	template->orig_fop = orig;
	template->shadow_fop = *orig;
	template->shadow_fop.owner = THIS_MODULE;
	template->shadow_fop.iterate_shared = sumh_shadow_iterate_shared;
	template->ingress_fop = *orig;
	INIT_LIST_HEAD(&template->retire_node);
	return template;
}

static const struct file_operations *
sumh_fop_installed_table(const struct sumh_fop_meta *m)
{
	const struct sumh_fop_template *template = m->template;

	return template->bridge.registered ? &template->ingress_fop
					   : &template->shadow_fop;
}

SUMH_NOCFI static int sumh_shadow_iterate_shared(struct file *file,
						 struct dir_context *ctx)
{
	struct sumh_filldir_wrapper *wrapper;
	struct sumh_fop_template *template;
	struct sumh_fop_meta *m;
	const struct file_operations *fop;
	const struct file_operations *orig;
	sumh_fop_iterate_client_fn client = NULL;
	void *client_data = NULL;
	loff_t entry_pos = ctx ? ctx->pos : 0;
	int srcu_idx;
	int ret;

	if (!file)
		return -EINVAL;
	fop = READ_ONCE(file->f_op);
	if (!fop || fop->iterate_shared != sumh_shadow_iterate_shared)
		return -ENOTDIR;
	template = container_of(fop, struct sumh_fop_template, shadow_fop);
	orig = template->orig_fop;

	if (!orig || !orig->iterate_shared)
		return -ENOTDIR;

	atomic64_inc(&sumh_hook_stats.iterate_fop_entries);
	srcu_idx = srcu_read_lock(&sumh_fop_client_srcu);
	rcu_read_lock();
	m = sumh_fop_lookup_rcu(file_inode(file));
	if (m && m->template == template) {
		/* Pairs with bind's release-store of the client pointer. */
		client = smp_load_acquire(&m->iterate_client);
		client_data = READ_ONCE(m->iterate_client_data);
	}
	rcu_read_unlock();
	if (sumh_dirhijack_injected_context(ctx)) {
		/* The native/merge phase is already drained. The client handles
		 * its remaining names and policies without another path lookup,
		 * merge scan or generic wrapper allocation. */
		if (client && client_data) {
			ret = client(file, ctx, orig, client_data);
			goto out;
		}
		/* CLEAR may leave an open fd at a retired client's cookie. */
		ctx->pos = 0;
	}
	wrapper = sumh_iterate_prepare_wrapper(file, ctx);
	if (IS_ERR(wrapper)) {
		ret = PTR_ERR(wrapper);
		/* A retired client cookie may have been reset above. Failed
		 * preparation must leave the caller's original cursor intact.
		 */
		ctx->pos = entry_pos;
		goto out;
	}
	if (!wrapper) {
		sumh_iterate_rewind_injected(ctx);
		ret = client && client_data
			  ? client(file, ctx, orig, client_data)
			  : orig->iterate_shared(file, ctx);
		goto out;
	}

	atomic64_inc(&sumh_hook_stats.iterate_fop_wrapped);
	wrapper->has_iterate_client = client && client_data;
	/* Compose the generic merge view with a specialized directory client.
	 * Emit before entering the native iterator, including for empty dirs.
	 */
	sumh_iterate_emit_injected(wrapper);
	ret = 0;
	while (!wrapper->stopped) {
		loff_t before = wrapper->wrap_ctx.pos;

		ret = client && client_data
			  ? client(file, &wrapper->wrap_ctx, orig, client_data)
			  : orig->iterate_shared(file, &wrapper->wrap_ctx);
		if (ret < 0 || wrapper->stopped || wrapper->emitted ||
		    wrapper->wrap_ctx.pos == before)
			break;
		/* A lower iterator may return a batch containing only entries
		 * removed by our filter. That is progress, not userspace EOF.
		 */
		cond_resched();
	}
	sumh_iterate_finish_wrapper(wrapper);
out:
	srcu_read_unlock(&sumh_fop_client_srcu, srcu_idx);
	return ret;
}

SUMH_NOCFI int sumh_fop_install(struct inode *inode)
{
	struct sumh_fop_template *template, *spare;
	struct sumh_fop_meta *m, *existing;
	struct super_block *sb;
	const struct file_operations *orig;
	bool installed = false;
	int ret = 0;

	if (!READ_ONCE(sumh_fop_override_ready))
		return -EOPNOTSUPP;
	if (!inode || !inode->i_mapping || !S_ISDIR(inode->i_mode))
		return -EINVAL;
	if (!inode->i_sb)
		return -EINVAL;
	if (test_bit(AS_FLAGS_SUMH_FOP_INSTALLED, &inode->i_mapping->flags))
		return 0;

	orig = READ_ONCE(inode->i_fop);
	if (!orig || !orig->iterate_shared)
		return -EOPNOTSUPP;
	spare = sumh_fop_template_alloc(orig);
	if (IS_ERR(spare))
		return PTR_ERR(spare);

	m = kzalloc(sizeof(*m), GFP_KERNEL);
	if (!m) {
		sumh_fop_template_free(spare);
		return -ENOMEM;
	}

	INIT_LIST_HEAD(&m->retire_node);
	/* Callers retain a live path (or equivalent protection) throughout
	 * install. Acquire our own active reference before taking the cached
	 * inode reference, without reviving a superblock already shutting down.
	 * Never sleep on s_umount while holding sumh_fop_lock. */
	sb = inode->i_sb;
	down_read(&sb->s_umount);
	if (!(READ_ONCE(sb->s_flags) & SB_BORN) ||
	    !atomic_inc_not_zero(&sb->s_active)) {
		up_read(&sb->s_umount);
		ret = -ESHUTDOWN;
		goto out_free;
	}
	m->sb = sb;
	up_read(&sb->s_umount);
	ihold(inode);
	m->inode = inode;

	spin_lock(&sumh_fop_lock);
	if (!READ_ONCE(sumh_fop_override_ready)) {
		ret = -EOPNOTSUPP;
		goto out_unlock;
	}
	existing = sumh_fop_lookup_rcu(inode);
	if (existing) {
		ret = 0;
		goto out_unlock;
	}
	if (READ_ONCE(inode->i_fop) != orig) {
		ret = -EAGAIN;
		goto out_unlock;
	}

	template = sumh_fop_template_lookup_locked(orig);
	if (!template) {
		template = spare;
		hash_add_rcu(sumh_fop_templates, &template->node,
			     (unsigned long)orig);
		(void)sumh_fop_bridge_register(
		    &template->bridge, &template->ingress_fop,
		    &template->shadow_fop, template->orig_fop);
		spare = NULL;
	}
	m->template = template;
	hash_add_rcu(sumh_fop_table, &m->node, (unsigned long)inode);
	set_bit(AS_FLAGS_SUMH_FOP_INSTALLED, &inode->i_mapping->flags);
	smp_wmb();
	WRITE_ONCE(inode->i_fop, sumh_fop_installed_table(m));
	installed = true;
	m = NULL;
out_unlock:
	spin_unlock(&sumh_fop_lock);
out_free:
	sumh_fop_meta_free(m);
	sumh_fop_template_free(spare);

	if (installed)
		sumh_log("fop_override: installed on inode %p (orig=%p)\n",
			 inode, orig);
	return ret;
}

int sumh_fop_bind_iterate_client(struct inode *inode,
				 sumh_fop_iterate_client_fn client, void *data)
{
	struct sumh_fop_meta *m;
	int ret = 0;

	if (!inode || !client || !data)
		return -EINVAL;

	spin_lock(&sumh_fop_lock);
	if (!READ_ONCE(sumh_fop_override_ready)) {
		ret = -EOPNOTSUPP;
		goto out;
	}
	m = sumh_fop_lookup_rcu(inode);
	if (!m) {
		ret = -ENOENT;
		goto out;
	}
	if (READ_ONCE(m->iterate_client) &&
	    (READ_ONCE(m->iterate_client) != client ||
	     READ_ONCE(m->iterate_client_data) != data)) {
		ret = -EBUSY;
		goto out;
	}

	/* Publish context before the callback.  Readers acquire-load the
	 * callback inside SRCU, so a non-NULL callback always observes
	 * initialized data. */
	WRITE_ONCE(m->iterate_client_data, data);
	smp_store_release(&m->iterate_client, client);
out:
	spin_unlock(&sumh_fop_lock);
	return ret;
}

void sumh_fop_unbind_iterate_client(struct inode *inode, void *data)
{
	struct sumh_fop_meta *m;

	if (!inode)
		return;
	spin_lock(&sumh_fop_lock);
	m = sumh_fop_lookup_rcu(inode);
	if (m && (!data || READ_ONCE(m->iterate_client_data) == data))
		smp_store_release(&m->iterate_client, NULL);
	spin_unlock(&sumh_fop_lock);
}

void sumh_fop_synchronize_iterate_clients(void)
{
	synchronize_srcu(&sumh_fop_client_srcu);
}

static struct sumh_fop_meta *sumh_fop_uninstall_locked(struct inode *inode)
{
	struct sumh_fop_meta *m;

	m = sumh_fop_lookup_rcu(inode);
	if (!m)
		return NULL;

	/* No new callback may acquire client data after this store.  A caller
	 * that frees the data waits for sumh_fop_client_srcu below. */
	smp_store_release(&m->iterate_client, NULL);
	if (READ_ONCE(inode->i_fop) == sumh_fop_installed_table(m)) {
		/* Publish the original table before the active meta is
		 * reclaimed. */
		smp_store_release(&inode->i_fop, m->template->orig_fop);
	}

	hash_del_rcu(&m->node);
	if (inode->i_mapping)
		clear_bit(AS_FLAGS_SUMH_FOP_INSTALLED,
			  &inode->i_mapping->flags);
	return m;
}

static void sumh_fop_retire_list(struct list_head *retiring)
{
	struct sumh_fop_meta *m, *next;

	if (list_empty(retiring))
		return;

	/* Dynamic fops templates are shared by original-fops identity and
	 * remain live until module exit.  A file opened across this restore can
	 * therefore finish fops_get()/the old-KMI bridge without retaining this
	 * per-inode client meta or creating one retired allocation per CLEAR
	 * generation.
	 */
	synchronize_rcu_tasks();
	/* Cover active-meta hash readers, then every callback which may have
	 * copied the client data before uninstall cleared it.
	 */
	synchronize_rcu();
	sumh_fop_synchronize_iterate_clients();

	list_for_each_entry_safe (m, next, retiring, retire_node) {
		list_del_init(&m->retire_node);
		WRITE_ONCE(m->iterate_client_data, NULL);
		sumh_fop_meta_free(m);
	}
}

bool sumh_fop_override_available(void)
{
	return READ_ONCE(sumh_fop_override_ready) &&
	       !READ_ONCE(sumh_fop_override_stopped);
}

int sumh_fop_override_init(void)
{
	hash_init(sumh_fop_table);
	hash_init(sumh_fop_templates);
	WRITE_ONCE(sumh_fop_override_stopped, false);
	WRITE_ONCE(sumh_fop_override_ready, true);
	pr_info("sumh: fop_override initialized\n");
	return 0;
}

static void sumh_fop_override_retire_all(bool stop_new)
{
	LIST_HEAD(retiring);
	struct sumh_fop_meta *m;
	struct hlist_node *tmp;
	bool reopen;
	int bkt;

	spin_lock(&sumh_fop_lock);
	if (stop_new)
		WRITE_ONCE(sumh_fop_override_stopped, true);
	reopen = READ_ONCE(sumh_fop_override_ready) &&
		 !READ_ONCE(sumh_fop_override_stopped);
	WRITE_ONCE(sumh_fop_override_ready, false);
	hash_for_each_safe(sumh_fop_table, bkt, tmp, m, node)
	{
		if (!stop_new && READ_ONCE(m->iterate_client))
			continue;
		m = sumh_fop_uninstall_locked(m->inode);
		if (m)
			list_add_tail(&m->retire_node, &retiring);
	}
	spin_unlock(&sumh_fop_lock);

	sumh_fop_retire_list(&retiring);

	/* Runtime CLEAR is reusable.  A concurrent/permanent stop wins and
	 * keeps the install gate closed. */
	if (reopen) {
		spin_lock(&sumh_fop_lock);
		if (!READ_ONCE(sumh_fop_override_stopped))
			WRITE_ONCE(sumh_fop_override_ready, true);
		spin_unlock(&sumh_fop_lock);
	}
}

void sumh_fop_override_clear(void)
{
	sumh_fop_override_retire_all(false);
}

void sumh_fop_override_stop_new(void)
{
	sumh_fop_override_retire_all(true);
}

void sumh_fop_override_exit(void)
{
	LIST_HEAD(retired_templates);
	struct sumh_fop_template *template, *next;
	struct hlist_node *tmp;
	int bkt;

	sumh_fop_override_stop_new();

	spin_lock(&sumh_fop_lock);
	WARN_ON_ONCE(!hash_empty(sumh_fop_table));
	hash_for_each_safe(sumh_fop_templates, bkt, tmp, template, node)
	{
		hash_del_rcu(&template->node);
		sumh_fop_bridge_unregister(&template->bridge);
		list_add_tail(&template->retire_node, &retired_templates);
	}
	spin_unlock(&sumh_fop_lock);

	/* PREPARE/module ownership guarantees no open shadow file can remain
	 * when module_exit runs. Finish bridge lookups before freeing the
	 * shared tables and releasing their one-per-original-fops owner
	 * references.
	 */
	synchronize_rcu();
	sumh_fop_synchronize_iterate_clients();
	list_for_each_entry_safe (template, next, &retired_templates,
				  retire_node) {
		list_del_init(&template->retire_node);
		sumh_fop_template_free(template);
	}
	pr_info("sumh: fop_override exited\n");
}
