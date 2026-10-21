#include <linux/dcache.h>
#include <linux/fs.h>
#include <linux/kprobes.h>
#include <uapi/linux/magic.h>
#include <linux/hashtable.h>
#include <linux/list.h>
#include <linux/llist.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/namei.h>
#include <linux/rcupdate.h>
#include <linux/refcount.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/srcu.h>
#include <linux/string.h>
#include <linux/version.h>
#include <linux/workqueue.h>

#include "sumh_base.h"
#include "sumh_dirhijack.h"
#include "sumh_fop_override.h"
#include "sumh_overlay.h"
#include "sumh_path_policy.h"
#include "sumh_runtime.h"
#include "sumh_sop_shadow.h"
#include "sumh_vfs_hooks.h"
#include "sumh_vnode.h"
#include "sumh_unicode_guard.h"
#include "feature/sucompat_vfs.h"

/*
 * readdir cookie tag.  Virtual children are emitted after the real entries at
 * positions carrying a signature in the high bits.  NOTE: like nomount, this
 * leaves a getdents d_off distinguishable from native cookies — the one
 * residual view-consistency tell of injecting into a real directory; there is
 * no collision-free way to mint native-looking cookies in the host fs's space.
 */
#define SUMH_DH_POS_SIG 0x6B6DULL /* "km" */
static inline bool sumh_dh_virtual_pos(loff_t pos)
{
	return (pos & 0xFFFFFFFF00000000ULL) == (SUMH_DH_POS_SIG << 48);
}
static inline loff_t sumh_dh_pack_pos(u32 id)
{
	return (loff_t)((SUMH_DH_POS_SIG << 48) | id);
}
static inline u32 sumh_dh_unpack_pos(loff_t pos)
{
	return (u32)(pos & 0xFFFFFFFFULL);
}

bool sumh_dirhijack_injected_context(const struct dir_context *ctx)
{
	return ctx && sumh_dh_virtual_pos(ctx->pos);
}

struct sumh_dh_iop {
	struct inode_operations fake_iop; /* must stay first */
	const struct inode_operations *orig_iop;
	struct sumh_dh_dir *dir;
};

struct sumh_dh_dir {
	refcount_t refs;
	struct inode *dir_inode; /* igrab'd real parent directory */
	struct hlist_head children;
	struct sumh_dh_iop *iop_meta;
	spinlock_t lock;
	bool iterate_bound; /* client of the shared fop_override owner */
	bool sop_client; /* exactly one s_op client per installed dir */
	bool retiring; /* blocks late d_op installs during CLEAR */
	struct list_head list;
};

/*
 * A per-dentry clone is required: real filesystems may already provide any of
 * the other dentry callbacks, and replacing their whole vector with one static
 * d_revalidate would silently discard those semantics.  @dentry is dget-held
 * until stale-result work or DEL/CLEAR has restored the vector, dropped the
 * dentry and drained SRCU.
 */
struct sumh_dh_dop_meta {
	struct dentry_operations shadow_dop;
	const struct dentry_operations *orig_dop;
	struct dentry *dentry;
	struct sumh_dh_dir *dir;
	unsigned long state;
	unsigned int orig_op_flags;
	u32 name_hash;
	u16 name_len;
	bool synthetic_negative;
	bool projected;
	struct sumh_dh_payload
	    *payload; /* pins immutable projection identity */
	struct hlist_node node;
	struct list_head retire_node;
	char name[];
};

/* SUMH_DH_OWNERS_TYPES_BEGIN */
struct sumh_dh_payload {
	refcount_t refs;
	struct path source;
	unsigned long v_ino;
	u8 flags;
	bool hide, lookup_only;
	char *vpath;
};

/* Stable physical slot; it never borrows a caller's path spelling. */
struct sumh_dh_child {
	refcount_t refs;
	struct sumh_dh_dir *dir;
	struct dentry *cache_parent;
	struct hlist_node node;
	struct list_head owners, prepared_claims;
	struct list_head retire_node, invalidate_node;
	struct sumh_dh_payload __rcu *winner;
	spinlock_t lock;
	unsigned int prepared, attached, hide_users;
	u32 name_hash;
	u16 name_len;
	bool indexed;
	char name[];
};

enum sumh_dh_claim_state {
	DH_PREPARED,
	DH_INVALIDATED,
	DH_ATTACHED,
	DH_DETACHED,
};
struct sumh_dh_claim {
	enum sumh_dh_owner_kind kind;
	enum sumh_dh_claim_state state;
	struct sumh_dh_child *slot;
	struct dentry *observed_parent;
	struct sumh_dh_payload *payload;
	struct list_head owner_node;
	struct rcu_head rcu;
	struct llist_node free_node;
};

/* Borrowed only inside RCU. Lookup pins the payload before leaving RCU.
 */
struct sumh_dh_view {
	struct sumh_dh_payload *payload;
	struct path source;
	unsigned long v_ino;
	u8 flags;
	unsigned int hide_users;
	bool legacy, hide, lookup_only;
	const char *vpath;
};
/* SUMH_DH_OWNERS_TYPES_END */

/* A captured observer decision. A NULL payload is valid for managed hide. */
struct sumh_dh_projection {
	struct sumh_dh_payload *payload;
	bool hidden;
};

static LIST_HEAD(sumh_dh_dirs);
#define SUMH_DH_DOP_HASH_BITS 8
static DEFINE_HASHTABLE(sumh_dh_dops, SUMH_DH_DOP_HASH_BITS);
static DEFINE_MUTEX(sumh_dh_lock);
DEFINE_STATIC_SRCU(sumh_dh_srcu);
static bool sumh_dh_ready;
static struct kprobe sumh_dh_fuse_probe;
static bool sumh_dh_fuse_registered;

#define SUMH_DH_DOP_AUTO_RETIRE 0

static void sumh_dh_reap_dops_workfn(struct work_struct *work);
static DECLARE_WORK(sumh_dh_reap_dops_work, sumh_dh_reap_dops_workfn);
static void sumh_dh_release_workfn(struct work_struct *work);
static DECLARE_WORK(sumh_dh_release_work, sumh_dh_release_workfn);
static LLIST_HEAD(sumh_dh_released_claims);

static struct dentry *sumh_dh_lookup(struct inode *dir, struct dentry *dentry,
				     unsigned int flags);
static int sumh_dh_atomic_open(struct inode *dir, struct dentry *dentry,
			       struct file *file, unsigned int flags,
			       umode_t mode);
static int sumh_dh_iterate(struct file *file, struct dir_context *ctx,
			   const struct file_operations *orig, void *data);
static int sumh_dh_set_dentry_ops(struct sumh_dh_dir *dir,
				  struct dentry *dentry,
				  const struct sumh_dh_projection *projection);
static bool
sumh_dh_validate_projection(struct sumh_dh_dir *dir, struct dentry *dentry,
			    const struct sumh_dh_projection *projection);
static void sumh_dh_drop_projection(struct dentry *dentry);
static void sumh_dh_payload_put(struct sumh_dh_payload *payload);
static void sumh_dh_dir_put(struct sumh_dh_dir *dir);

bool sumh_dirhijack_enabled(void)
{
	return READ_ONCE(sumh_dh_ready);
}

bool sumh_dirhijack_fuse_guard_available(void)
{
	return READ_ONCE(sumh_dh_ready) && READ_ONCE(sumh_dh_fuse_registered);
}

/* ---- meta accessors ---------------------------------------------------- */

static struct sumh_dh_iop *sumh_dh_iop_meta(const struct inode_operations *iop)
{
	if (iop && iop->lookup == sumh_dh_lookup)
		return container_of(iop, struct sumh_dh_iop, fake_iop);
	return NULL;
}

static struct sumh_dh_iop *sumh_dh_iop_of(const struct inode *inode)
{
	return inode ? sumh_dh_iop_meta(smp_load_acquire(&inode->i_op)) : NULL;
}

static struct sumh_dh_child *
sumh_dh_find_slot(struct sumh_dh_dir *dir, const char *name, u16 len, u32 hash)
{
	struct sumh_dh_child *c;

	hlist_for_each_entry_rcu(c, &dir->children, node)
	{
		if (c->name_hash == hash && c->name_len == len &&
		    !memcmp(c->name, name, len))
			return c;
	}
	return NULL;
}

static struct sumh_dh_child *
sumh_dh_find_child(struct sumh_dh_dir *dir, const char *name, u16 len, u32 hash)
{
	struct sumh_dh_child *c = sumh_dh_find_slot(dir, name, len, hash);

	return c && READ_ONCE(c->attached) ? c : NULL;
}

/* Caller owns RCU; no borrowed pointer in @view may outlive it. The lock is
 * below dentry->d_lock (FUSE guard), so writers release it before d_op work. */
static bool sumh_dh_read_view(struct sumh_dh_child *c,
			      struct sumh_dh_view *view)
{
	struct sumh_dh_payload *payload;
	bool active;

	memset(view, 0, sizeof(*view));
	if (!c)
		return false;
	spin_lock(&c->lock);
	active = c->attached != 0;
	view->hide_users = c->hide_users;
	payload = rcu_dereference_protected(c->winner, 1);
	view->payload = payload;
	if (payload) {
		view->source = payload->source;
		view->v_ino = payload->v_ino;
		view->flags = payload->flags;
		view->hide = payload->hide;
		view->lookup_only = payload->lookup_only;
		view->vpath = payload->vpath;
		view->legacy = true;
	}
	spin_unlock(&c->lock);
	return active;
}

const struct inode_operations *
sumh_dirhijack_original_iops(const struct inode *inode)
{
	const struct inode_operations *orig;
	int idx = srcu_read_lock(&sumh_dh_srcu);
	struct sumh_dh_iop *meta = sumh_dh_iop_of(inode);
	orig = meta ? meta->orig_iop : inode->i_op;
	srcu_read_unlock(&sumh_dh_srcu, idx);
	return orig;
}

/* True if the current task should see this dir's injected children. */
static bool sumh_dh_current_sees(void)
{
	if (!sumh_dirhijack_enabled())
		return false;
	return sumh_policy_current_is_view_target();
}

static bool sumh_dh_current_hides(const struct sumh_dh_view *child,
				  const struct inode *parent)
{
	return (child->hide || READ_ONCE(child->hide_users)) &&
	       sumh_dirhijack_enabled() &&
	       sumh_policy_current_is_hide_target(parent);
}

static bool sumh_dh_current_targets(const struct sumh_dh_view *child,
				    const struct inode *parent)
{
	if (child->flags & SUMH_VNODE_F_SU)
		return ksu_sucompat_vfs_current_ino(child->v_ino);
	if (sumh_dh_current_hides(child, parent))
		return true;
	return child->legacy && !child->hide && sumh_dh_current_sees();
}

static bool sumh_dh_child_emitted(const struct sumh_dh_view *child,
				  const struct inode *parent)
{
	if (!child->legacy || child->hide || child->lookup_only ||
	    sumh_dh_current_hides(child, parent))
		return false;
	return (child->flags & SUMH_VNODE_F_SU)
		   ? (ksu_sucompat_vfs_current_ino(child->v_ino) &&
		      ksu_sucompat_vfs_visible())
		   : sumh_dh_current_sees();
}

static bool sumh_dh_hidden_name(struct inode *parent, const struct qstr *name)
{
	struct sumh_dh_iop *m = sumh_dh_iop_of(parent);
	struct sumh_dh_child *child;
	struct sumh_dh_view view;
	bool hidden = false;

	if (!m || !m->dir || !name || READ_ONCE(m->dir->retiring))
		return false;
	rcu_read_lock();
	child =
	    sumh_dh_find_child(m->dir, name->name, (u16)name->len,
			       full_name_hash(parent, name->name, name->len));
	if (sumh_dh_read_view(child, &view))
		hidden = sumh_dh_current_hides(&view, parent);
	rcu_read_unlock();
	return hidden;
}

static bool sumh_dh_capture_projection(struct sumh_dh_dir *dn,
				       struct inode *dir, const char *name,
				       u16 len,
				       struct sumh_dh_projection *projection)
{
	struct sumh_dh_child *child;
	struct sumh_dh_view view;
	bool selected = false;

	memset(projection, 0, sizeof(*projection));
	if (!dn || READ_ONCE(dn->retiring))
		return false;
	rcu_read_lock();
	child =
	    sumh_dh_find_child(dn, name, len, full_name_hash(dir, name, len));
	if (sumh_dh_read_view(child, &view) &&
	    sumh_dh_current_targets(&view, dir)) {
		projection->payload = view.payload;
		if (view.payload)
			refcount_inc(&view.payload->refs);
		projection->hidden = sumh_dh_current_hides(&view, dir) ||
				     ((view.flags & SUMH_VNODE_F_SU) &&
				      !ksu_sucompat_vfs_visible());
		selected = true;
	}
	rcu_read_unlock();
	return selected;
}

/* Called under RCU or DH; source identity is held by the projection/meta. */
static bool
sumh_dh_projection_matches(struct sumh_dh_child *child, struct inode *dir,
			   const struct sumh_dh_projection *projection)
{
	struct sumh_dh_view view;
	bool hidden;

	if (!sumh_dh_read_view(child, &view) ||
	    view.payload != projection->payload ||
	    !sumh_dh_current_targets(&view, dir))
		return false;
	hidden =
	    sumh_dh_current_hides(&view, dir) ||
	    ((view.flags & SUMH_VNODE_F_SU) && !ksu_sucompat_vfs_visible());
	return hidden == projection->hidden;
}

static int sumh_dh_requested_name(struct dentry *dentry, char *name)
{
	unsigned int len;

	spin_lock(&dentry->d_lock);
	len = dentry->d_name.len;
	if (len <= NAME_MAX) {
		memcpy(name, dentry->d_name.name, len);
		name[len] = '\0';
	}
	spin_unlock(&dentry->d_lock);
	return len > NAME_MAX ? -ENAMETOOLONG : (int)len;
}

/* Native lookup may return a moved alias and may publish after a commit.
 * Recheck the requested slot after the native call, including PREPARED.
 * This governs completion; it does not gate a filesystem's internal
 * hash-to-return interval or READDIRPLUS cache ingress. */
static void sumh_dh_native_complete(struct sumh_dh_dir *dn, const char *name,
				    u16 len, struct dentry *result)
{
	bool governed, retiring, same_name;

	if (!dn || !result || IS_ERR(result))
		return;
	/* Observe withdrawal and slot eligibility together. If a governed
	 * slot retires after unlocking, set_dentry_ops rechecks and drops. */
	mutex_lock(&sumh_dh_lock);
	retiring = READ_ONCE(dn->retiring);
	governed =
	    !retiring &&
	    sumh_dh_find_slot(dn, name, len,
			      full_name_hash(dn->dir_inode, name, len)) != NULL;
	mutex_unlock(&sumh_dh_lock);
	if (retiring) {
		sumh_dh_drop_projection(result);
		return;
	}
	if (!governed)
		return;
	spin_lock(&result->d_lock);
	same_name = d_inode(result->d_parent) == dn->dir_inode &&
		    result->d_name.len == len &&
		    !memcmp(result->d_name.name, name, len);
	spin_unlock(&result->d_lock);
	if (!same_name || sumh_dh_set_dentry_ops(dn, result, NULL))
		sumh_dh_drop_projection(result);
}

static struct dentry *
sumh_dh_materialize(struct inode *dir, struct sumh_dh_dir *dn,
		    struct dentry *dentry,
		    const struct sumh_dh_projection *projection);

static int SUMH_NOCFI sumh_dh_atomic_open_inner(struct inode *dir,
						struct dentry *dentry,
						struct file *file,
						unsigned int flags,
						umode_t mode)
{
	const struct inode_operations *iop = smp_load_acquire(&dir->i_op);
	struct sumh_dh_iop *m = sumh_dh_iop_meta(iop);
	struct sumh_dh_dir *dn = m ? m->dir : NULL;
	const struct inode_operations *orig = m ? m->orig_iop : iop;
	struct sumh_dh_projection projection;
	struct dentry *result;
	char name[NAME_MAX + 1];
	int len, ret;

	len = sumh_dh_requested_name(dentry, name);
	if (len < 0)
		return len;
	if (sumh_dh_capture_projection(dn, dir, name, len, &projection)) {
		if (projection.hidden) {
			sumh_dh_payload_put(projection.payload);
			return -ENOENT;
		}
		/* atomic_open also receives already hashed negative dentries.
		 * Both d_add and d_splice_alias require an unhashed input. */
		d_drop(dentry);
		result = sumh_dh_materialize(dir, dn, dentry, &projection);
		sumh_dh_payload_put(projection.payload);
		/* Consumes an alias reference; NULL means input was filled.
		 * VFS owns lookup_done and all ordinary open/create checks. */
		return finish_no_open(file, result);
	}
	if (!orig || !orig->atomic_open ||
	    orig->atomic_open == sumh_dh_atomic_open)
		return -EOPNOTSUPP;
	ret = orig->atomic_open(dir, dentry, file, flags, mode);
	if (ret)
		return ret;
	/* A failed/malformed native callback can leave DENTRY_NOT_SET.
	 * Never dereference it. VFS retains its own diagnostic/error policy. */
	result = file->f_path.dentry;
	if (!IS_ERR(result))
		sumh_dh_native_complete(dn, name, len, result ?: dentry);
	return ret;
}

static int sumh_dh_atomic_open(struct inode *dir, struct dentry *dentry,
			       struct file *file, unsigned int flags,
			       umode_t mode)
{
	int idx = srcu_read_lock(&sumh_dh_srcu);
	int ret = sumh_dh_atomic_open_inner(dir, dentry, file, flags, mode);

	srcu_read_unlock(&sumh_dh_srcu, idx);
	return ret;
}

/* READDIRPLUS can publish real dentries without calling the parent lookup. */
static int sumh_dh_fuse_revalidate_pre(struct kprobe *probe,
				       struct pt_regs *regs)
{
#ifdef CONFIG_ARM64
	struct dentry *dentry;
	struct inode *parent;
	const struct qstr *name;
	bool hidden;
	bool unicode_blocked;
	int idx;

	(void)probe;
	if (!sumh_dirhijack_enabled())
		return 0;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 13, 0)
	parent = (void *)regs->regs[0];
	name = (void *)regs->regs[1];
	dentry = (void *)regs->regs[2];
#else
	dentry = (void *)regs->regs[0];
#endif
	if (!dentry)
		return 0;
	idx = srcu_read_lock(&sumh_dh_srcu);
	spin_lock(&dentry->d_lock);
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 13, 0)
	parent = d_inode(dentry->d_parent);
	name = &dentry->d_name;
#endif
	/* Security decisions are independent of VIEW/SPOOF and UID hide
	 * exemptions. Run before the normal SUMH module visibility policy.
	 */
	unicode_blocked = sumh_unicode_guard_reject_component(name);
	hidden = !unicode_blocked && sumh_dh_hidden_name(parent, name);
	spin_unlock(&dentry->d_lock);
	srcu_read_unlock(&sumh_dh_srcu, idx);
	if (!hidden && !unicode_blocked)
		return 0;
	regs->regs[0] = (unsigned long)(unicode_blocked ? -EACCES : -ENOENT);
	instruction_pointer_set(regs, regs->regs[30]);
	return 1;
#else
	return 0;
#endif
}

/* ---- hijacked lookup (SRCU-wrapped) ------------------------------------ */

static struct dentry *
sumh_dh_materialize(struct inode *dir, struct sumh_dh_dir *dn,
		    struct dentry *dentry,
		    const struct sumh_dh_projection *projection)
{
	struct sumh_dh_payload *payload = projection->payload;
	struct inode *vi = NULL;
	struct dentry *res = NULL;
	int ret;

	if (!projection->hidden) {
		if (!payload) {
			ret = -EAGAIN;
			goto failed_input;
		}
		if ((payload->flags & SUMH_VNODE_F_VIRTUAL_DIR) &&
		    payload->vpath)
			vi = sumh_vnode_new_virtual(dir->i_sb, payload->vpath,
						    payload->v_ino, dir);
		else
			vi = sumh_vnode_new(
			    dir->i_sb,
			    payload->source.dentry ? &payload->source : NULL,
			    payload->v_ino, S_IFREG | 0644, payload->flags);
		if (!vi) {
			ret = -ENOMEM;
			goto failed_input;
		}
	}
	ret = sumh_dh_set_dentry_ops(dn, dentry, projection);
	if (ret) {
		if (vi)
			iput(vi);
		goto failed_input;
	}
	if (projection->hidden)
		d_add(dentry, NULL);
	else
		res = d_splice_alias(vi, dentry);
	if (IS_ERR(res)) {
		sumh_dh_drop_projection(dentry);
		return res;
	}
	if (res) {
		/* The unused input must not keep a shadow/lifetime pin until
		 * some later DEL. Retirement runs after this SRCU callback. */
		sumh_dh_drop_projection(dentry);
		ret = sumh_dh_set_dentry_ops(dn, res, projection);
		if (ret)
			goto stale;
	}
	/* DEL can restore the first d_op before d_add/splice publishes.
	 * Recheck without allocation after the actual publication. */
	if (!sumh_dh_validate_projection(dn, res ?: dentry, projection)) {
		ret = -EAGAIN;
		goto stale;
	}
	return res;
stale:
	sumh_dh_drop_projection(res ?: dentry);
	if (res)
		dput(res);
	return ERR_PTR(ret == -ENOENT || ret == -ESHUTDOWN ? -EAGAIN : ret);
failed_input:
	/* An observer can change after a native negative revalidates but before
	 * atomic_open selects this projection. Its old immutable metadata must
	 * retire even when allocation/installation fails before publication. */
	sumh_dh_drop_projection(dentry);
	return ERR_PTR(ret == -ENOENT || ret == -ESHUTDOWN ? -EAGAIN : ret);
}

static struct dentry *SUMH_NOCFI sumh_dh_lookup_inner(struct inode *dir,
						      struct dentry *dentry,
						      unsigned int flags)
{
	const struct inode_operations *iop = smp_load_acquire(&dir->i_op);
	struct sumh_dh_iop *m = sumh_dh_iop_meta(iop);
	struct sumh_dh_dir *dn = m ? READ_ONCE(m->dir) : NULL;
	const struct inode_operations *orig = m ? m->orig_iop : iop;
	struct sumh_dh_projection projection;
	struct dentry *res;
	char name[NAME_MAX + 1];
	int len;

	len = sumh_dh_requested_name(dentry, name);
	if (len < 0)
		return ERR_PTR(len);
	if (sumh_dh_capture_projection(dn, dir, name, len, &projection)) {
		res = sumh_dh_materialize(dir, dn, dentry, &projection);
		sumh_dh_payload_put(projection.payload);
		return res;
	}
	/* A callback loaded before restoration may enter after i_op is
	 * native again. Use that current native lookup rather than minting
	 * an invented negative or recursively entering the shadow. */
	if (!orig || !orig->lookup || orig->lookup == sumh_dh_lookup)
		return ERR_PTR(-EOPNOTSUPP);
	res = orig->lookup(dir, dentry, flags);
	if (!IS_ERR(res))
		sumh_dh_native_complete(dn, name, len, res ?: dentry);
	return res;
}

static struct dentry *sumh_dh_lookup(struct inode *dir, struct dentry *dentry,
				     unsigned int flags)
{
	struct dentry *res;
	int idx = srcu_read_lock(&sumh_dh_srcu);

	res = sumh_dh_lookup_inner(dir, dentry, flags);
	srcu_read_unlock(&sumh_dh_srcu, idx);
	return res;
}

/* ---- hijacked iterate (readdir injection, SRCU-wrapped) ---------------- */

struct sumh_dh_proxy {
	struct dir_context ctx;
	struct dir_context *orig;
	struct sumh_dh_dir *dir;
	int emitted;
	bool stopped;
};

static bool SUMH_NOCFI sumh_dh_proxy_actor(struct dir_context *ctx,
					   const char *name, int namelen,
					   loff_t offset, u64 ino,
					   unsigned int d_type)
{
	struct sumh_dh_proxy *p = container_of(ctx, struct sumh_dh_proxy, ctx);
	bool ret;
	bool injected = false;

	if (p->dir) {
		u32 hash = full_name_hash(p->dir->dir_inode, name, namelen);
		struct sumh_dh_child *c;
		struct sumh_dh_view view;

		rcu_read_lock();
		c = sumh_dh_find_child(p->dir, name, (u16)namelen, hash);
		/* Suppress the real entry only for names dirhijack itself emits
		 * (inject / hide).  A lookup_only child leaves readdir to the
		 * overlay filldir, which already emits and dedups the name, so
		 * it must pass through here untouched. */
		injected =
		    sumh_dh_read_view(c, &view) &&
		    (sumh_dh_current_hides(&view, p->dir->dir_inode) ||
		     (!view.lookup_only &&
		      sumh_dh_current_targets(&view, p->dir->dir_inode)));
		rcu_read_unlock();
	}
	if (injected)
		return true;
	p->orig->pos = p->ctx.pos;
	ret = p->orig->actor(p->orig, name, namelen, offset, ino, d_type);
	p->ctx.pos = p->orig->pos;
	if (ret)
		p->emitted++;
	else
		p->stopped = true;
	return ret;
}

#define SUMH_DH_EMIT_BATCH 8

struct sumh_dh_emit_entry {
	unsigned long ino;
	u16 name_len;
	u8 type;
	char name[NAME_MAX + 1];
};

static int sumh_dh_emit_children(struct dir_context *ctx,
				 struct sumh_dh_dir *dir)
{
	struct sumh_dh_emit_entry *entries;
	struct sumh_dh_child *c;
	u32 want = sumh_dh_unpack_pos(ctx->pos);
	u32 idx = 0;
	unsigned int count = 0, i;

	entries =
	    kmalloc_array(SUMH_DH_EMIT_BATCH, sizeof(*entries), GFP_KERNEL);
	if (!entries)
		return -ENOMEM;
	if (!sumh_dh_virtual_pos(ctx->pos))
		ctx->pos = sumh_dh_pack_pos(0);
	/* The actor may fault on userspace memory; retain only copied names. */
	rcu_read_lock();
	hlist_for_each_entry_rcu(c, &dir->children, node)
	{
		u32 cur;
		struct sumh_dh_emit_entry *entry;
		struct sumh_dh_view view;
		if (!sumh_dh_read_view(c, &view) ||
		    !sumh_dh_child_emitted(&view, dir->dir_inode))
			continue; /* suppress-only, or readdir owned by the
				   * overlay filldir: occupies no readdir slot
				   */
		cur = idx++;
		if (cur < want)
			continue;
		entry = &entries[count++];
		entry->ino = view.v_ino;
		entry->name_len = c->name_len;
		entry->type = (view.flags & SUMH_VNODE_F_DIR)	? DT_DIR
			      : (view.flags & SUMH_VNODE_F_LNK) ? DT_LNK
								: DT_REG;
		memcpy(entry->name, c->name, c->name_len + 1);
		if (count == SUMH_DH_EMIT_BATCH)
			break;
	}
	rcu_read_unlock();
	for (i = 0; i < count; i++) {
		struct sumh_dh_emit_entry *entry = &entries[i];

		ctx->pos = sumh_dh_pack_pos(want + i);
		if (!sumh_emit_projected_dirent(ctx, entry->name,
						entry->name_len, entry->ino,
						entry->type))
			break;
		ctx->pos = sumh_dh_pack_pos(want + i + 1);
	}
	kfree(entries);
	return 0;
}

static int SUMH_NOCFI sumh_dh_iterate_inner(struct file *file,
					    struct dir_context *ctx,
					    const struct file_operations *orig,
					    struct sumh_dh_dir *dn)
{
	struct sumh_dh_proxy proxy = {.ctx.actor = sumh_dh_proxy_actor};
	int ret;

	if (!orig || !orig->iterate_shared)
		return -ENOTDIR;
	if (!dn || (!sumh_dh_current_sees() &&
		    !sumh_policy_current_is_hide_target(dn->dir_inode) &&
		    !ksu_sucompat_vfs_enabled())) {
		if (sumh_dh_virtual_pos(ctx->pos))
			ctx->pos = 0;
		return orig->iterate_shared(file, ctx);
	}

	if (sumh_dh_virtual_pos(ctx->pos))
		return sumh_dh_emit_children(ctx, dn);

	proxy.ctx.pos = ctx->pos;
	proxy.orig = ctx;
	proxy.dir = dn;
	for (;;) {
		loff_t before = proxy.ctx.pos;

		proxy.emitted = 0;
		proxy.stopped = false;
		ret = orig->iterate_shared(file, &proxy.ctx);
		ctx->pos = proxy.ctx.pos;
		if (ret < 0 || proxy.stopped || proxy.emitted)
			return ret;
		if (proxy.ctx.pos == before)
			break;
		/* Hidden-only batches must not signal EOF to the caller. */
		cond_resched();
	}
	if (!sumh_dh_current_sees() && !ksu_sucompat_vfs_enabled())
		return ret;

	ctx->pos = sumh_dh_pack_pos(0);
	return sumh_dh_emit_children(ctx, dn);
}

static int sumh_dh_iterate(struct file *file, struct dir_context *ctx,
			   const struct file_operations *orig, void *data)
{
	struct sumh_dh_dir *dir = data;
	int ret;
	int idx = srcu_read_lock(&sumh_dh_srcu);

	ret = sumh_dh_iterate_inner(file, ctx, orig, dir);
	srcu_read_unlock(&sumh_dh_srcu, idx);
	return ret;
}

/* ---- per-observer d_revalidate (SRCU-wrapped) -------------------------- */

#define SUMH_DH_DOP_FLAGS                                                      \
	(DCACHE_OP_HASH | DCACHE_OP_COMPARE | DCACHE_OP_REVALIDATE |           \
	 DCACHE_OP_WEAK_REVALIDATE | DCACHE_OP_DELETE | DCACHE_OP_PRUNE |      \
	 DCACHE_OP_REAL)

static struct sumh_dh_dop_meta *
sumh_dh_dop_lookup_rcu(const struct dentry *dentry)
{
	struct sumh_dh_dop_meta *m;

	hash_for_each_possible_rcu(sumh_dh_dops, m, node, (unsigned long)dentry)
	{
		if (m->dentry == dentry)
			return m;
	}
	return NULL;
}

static struct sumh_dh_dop_meta *
sumh_dh_dop_lookup_locked(const struct dentry *dentry)
{
	struct sumh_dh_dop_meta *m;

	hash_for_each_possible(sumh_dh_dops, m, node, (unsigned long)dentry)
	{
		if (m->dentry == dentry)
			return m;
	}
	return NULL;
}

/*
 * A stale per-observer dentry is about to be invalidated by the VFS.  The
 * meta's dget must not keep that now-unhashed object (and a possible vnode)
 * alive until DEL/CLEAR.  d_revalidate can run in LOOKUP_RCU, so it may only
 * mark the preallocated meta and queue sleepable retirement here.
 */
static void sumh_dh_queue_dop_retire(struct sumh_dh_dop_meta *m)
{
	if (!test_and_set_bit(SUMH_DH_DOP_AUTO_RETIRE, &m->state))
		schedule_work(&sumh_dh_reap_dops_work);
}

static int SUMH_NOCFI sumh_dh_revalidate_inner(
    struct sumh_dh_dop_meta *dm, struct inode *dir, const struct qstr *name,
    struct dentry *dentry, unsigned int flags, bool *chain_orig)
{
	struct sumh_dh_dir *dn = dm ? READ_ONCE(dm->dir) : NULL;
	struct inode *inode;
	bool is_virtual;
	bool governed = false;
	bool child_hide = false;
	bool synthetic_negative;
	bool sees;

	*chain_orig = false;
	inode = READ_ONCE(dentry->d_inode);
	is_virtual = inode && sumh_vnode_is_ours(inode);
	synthetic_negative = !inode && READ_ONCE(dm->synthetic_negative);
	if (!dn || READ_ONCE(dn->retiring))
		return 0;
	sees = false;

	/* A rename can leave the held dentry carrying our shadow outside the
	 * directory where it was installed.  It is no longer governed there. */
	if (dir && dir == dn->dir_inode) {
		struct sumh_dh_child *c;
		struct sumh_dh_view view;

		rcu_read_lock();
		c = sumh_dh_find_child(
		    dn, name->name, (u16)name->len,
		    full_name_hash(dir, name->name, name->len));
		if (dm->projected) {
			struct sumh_dh_projection projection = {
			    .payload = dm->payload,
			    .hidden = dm->synthetic_negative,
			};

			if (!sumh_dh_projection_matches(c, dir, &projection)) {
				rcu_read_unlock();
				return 0;
			}
		}
		if (sumh_dh_read_view(c, &view)) {
			governed = true;
			child_hide =
			    view.hide || sumh_dh_current_hides(&view, dir);
			sees = sumh_dh_current_targets(&view, dir);
			if (view.flags & SUMH_VNODE_F_SU) {
				bool enabled =
				    ksu_sucompat_vfs_current_ino(view.v_ino);
				bool visible = ksu_sucompat_vfs_visible();
				unsigned long ino = view.v_ino;
				rcu_read_unlock();
				if (enabled)
					return visible
						   ? (inode &&
						      inode->i_ino == ino &&
						      ksu_sucompat_vfs_is_inode(
							  inode))
						   : (!inode &&
						      synthetic_negative);
				if (is_virtual || synthetic_negative)
					return 0;
				*chain_orig = true;
				return 1;
			}
		}
		rcu_read_unlock();
	}

	(void)flags;

	/*
	 * Decide, per current observer, whether the cached dentry still matches
	 * what a fresh lookup would yield; return 0 to force re-resolution.
	 * Only 0/1 is returned and nothing sleeps, so this is rcu-walk safe.
	 *
	 * Not governed: no rule touches this name.  Keep a real/negative
	 * dentry; invalidate a stale virtual left by a since-deleted rule so
	 * the name drops back to its real entry.
	 *
	 * Inject rule: a seeing observer must resolve to the virtual inode, so
	 * a cached real/negative is stale; a non-seeing observer must resolve
	 * to the real entry, so a cached virtual is stale.
	 *
	 * Hide rule: a seeing observer must resolve to a negative (hidden), so
	 * a cached real or virtual dentry is stale; a non-seeing observer (e.g.
	 * root) must resolve to the real entry, so a negative cached by a
	 * seeing observer -- and any stray virtual -- is stale.  This is the
	 * §5.2 cross-observer mirror: a correctly hidden negative must not leak
	 * to an observer the rule does not target, which still has to see the
	 * file.
	 */
	if (!governed) {
		if (is_virtual || synthetic_negative)
			return 0;
		*chain_orig = true;
		return 1;
	}
	if (!child_hide) { /* inject */
		if (sees)
			return is_virtual ? 1 : 0;
		if (is_virtual || synthetic_negative)
			return 0;
		*chain_orig = true;
		return 1;
	}
	/* hide */
	if (sees) {
		if (inode)
			return 0;
		if (!synthetic_negative)
			*chain_orig = true;
		return 1; /* valid negative */
	}
	if (inode && !is_virtual) {
		*chain_orig = true;
		return 1; /* positive-real */
	}
	if (!inode && !synthetic_negative) {
		*chain_orig = true;
		return 1; /* negative-real */
	}
	return 0;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 13, 0)
static int SUMH_NOCFI sumh_dh_revalidate(struct inode *dir,
					 const struct qstr *name,
					 struct dentry *dentry,
					 unsigned int flags)
{
	struct sumh_dh_dop_meta *dm;
	bool chain_orig;
	int ret;
	/* srcu_read_lock() is non-sleeping and the inner never sleeps, so this
	 * is safe in rcu-walk; it also pins the meta against a concurrent
	 * clear(). */
	int idx = srcu_read_lock(&sumh_dh_srcu);

	rcu_read_lock();
	dm = sumh_dh_dop_lookup_rcu(dentry);
	rcu_read_unlock();
	if (!dm) {
		ret = 0;
		goto out;
	}
	ret =
	    sumh_dh_revalidate_inner(dm, dir, name, dentry, flags, &chain_orig);
	if (ret > 0 && chain_orig && dm->orig_dop && dm->orig_dop->d_revalidate)
		ret = dm->orig_dop->d_revalidate(dir, name, dentry, flags);
	if (ret == 0)
		sumh_dh_queue_dop_retire(dm);
out:
	srcu_read_unlock(&sumh_dh_srcu, idx);
	return ret;
}
#else
static int SUMH_NOCFI sumh_dh_revalidate(struct dentry *dentry,
					 unsigned int flags)
{
	struct sumh_dh_dop_meta *dm;
	struct inode *dir = d_inode(READ_ONCE(dentry->d_parent));
	const struct qstr *name = &dentry->d_name;
	bool chain_orig;
	int ret;
	int idx = srcu_read_lock(&sumh_dh_srcu);

	rcu_read_lock();
	dm = sumh_dh_dop_lookup_rcu(dentry);
	rcu_read_unlock();
	if (!dm) {
		ret = 0;
		goto out;
	}
	ret =
	    sumh_dh_revalidate_inner(dm, dir, name, dentry, flags, &chain_orig);
	if (ret > 0 && chain_orig && dm->orig_dop && dm->orig_dop->d_revalidate)
		/* Preserve the original callback's LOOKUP_RCU result verbatim,
		 * including -ECHILD when it needs a ref-walk retry. */
		ret = dm->orig_dop->d_revalidate(dentry, flags);
	if (ret == 0)
		sumh_dh_queue_dop_retire(dm);
out:
	srcu_read_unlock(&sumh_dh_srcu, idx);
	return ret;
}
#endif

static int sumh_dh_set_dentry_ops(struct sumh_dh_dir *dir,
				  struct dentry *dentry,
				  const struct sumh_dh_projection *projection)
{
	struct sumh_dh_dop_meta *m, *existing;
	struct sumh_dh_child *slot;
	const struct dentry_operations *orig;
	char name[NAME_MAX + 1];
	u32 hash;
	u16 len;
	int ret = 0;

	if (!dir || !dir->dir_inode || !dentry)
		return -EINVAL;
	spin_lock(&dentry->d_lock);
	if (dentry->d_name.len > NAME_MAX ||
	    d_inode(dentry->d_parent) != dir->dir_inode) {
		spin_unlock(&dentry->d_lock);
		return -EAGAIN;
	}
	len = (u16)dentry->d_name.len;
	memcpy(name, dentry->d_name.name, len);
	name[len] = '\0';
	spin_unlock(&dentry->d_lock);
	hash = full_name_hash(dir->dir_inode, name, len);

	m = kzalloc(sizeof(*m) + len + 1, GFP_KERNEL);
	if (!m)
		return -ENOMEM;
	m->dentry = dget(dentry);
	m->dir = dir;
	m->name_hash = hash;
	m->name_len = len;
	m->projected = projection != NULL;
	m->synthetic_negative = projection && projection->hidden;
	m->payload = projection ? projection->payload : NULL;
	if (m->payload)
		refcount_inc(&m->payload->refs);
	INIT_LIST_HEAD(&m->retire_node);
	memcpy(m->name, name, len);
	m->name[len] = '\0';

	mutex_lock(&sumh_dh_lock);
	if (READ_ONCE(dir->retiring)) {
		ret = -ESHUTDOWN;
		goto out_unlock;
	}
	/* PREPARED stays transparent but governs native cache completions.
	 * Synthetic metadata must still belong to the captured payload, not
	 * merely to any winner that happens to occupy the same slot now. */
	slot = sumh_dh_find_slot(dir, m->name, len, hash);
	if (!slot) {
		ret = -ENOENT;
		goto out_unlock;
	}
	if (projection &&
	    !sumh_dh_projection_matches(slot, dir->dir_inode, projection)) {
		ret = -EAGAIN;
		goto out_unlock;
	}

	existing = sumh_dh_dop_lookup_locked(dentry);
	if (existing) {
		spin_lock(&dentry->d_lock);
		if (d_inode(dentry->d_parent) != dir->dir_inode ||
		    dentry->d_name.len != len ||
		    memcmp(dentry->d_name.name, name, len))
			ret = -EAGAIN;
		else if (test_bit(SUMH_DH_DOP_AUTO_RETIRE, &existing->state) ||
			 dentry->d_op != &existing->shadow_dop ||
			 existing->dir != dir || existing->name_hash != hash ||
			 existing->name_len != len ||
			 memcmp(existing->name, name, len) ||
			 existing->projected != m->projected ||
			 existing->synthetic_negative !=
			     m->synthetic_negative ||
			 existing->payload != m->payload)
			ret = -EBUSY;
		spin_unlock(&dentry->d_lock);
		goto out_unlock;
	}

	if (refcount_read(&dir->refs) >= INT_MAX - 4) {
		ret = -EOVERFLOW;
		goto out_unlock;
	}
	spin_lock(&dentry->d_lock);
	if (d_inode(dentry->d_parent) != dir->dir_inode ||
	    dentry->d_name.len != len ||
	    memcmp(dentry->d_name.name, m->name, len)) {
		spin_unlock(&dentry->d_lock);
		ret = -EAGAIN;
		goto out_unlock;
	}
	refcount_inc(&dir->refs);
	orig = READ_ONCE(dentry->d_op);
	m->orig_dop = orig;
	if (orig)
		m->shadow_dop = *orig;
	m->shadow_dop.d_revalidate = sumh_dh_revalidate;
	m->orig_op_flags = READ_ONCE(dentry->d_flags) & SUMH_DH_DOP_FLAGS;
	hash_add_rcu(sumh_dh_dops, &m->node, (unsigned long)dentry);
	/* Publish the initialized/hash-visible meta before pathwalk can observe
	 * DCACHE_OP_REVALIDATE and enter the shadow callback. */
	smp_wmb();
	WRITE_ONCE(dentry->d_op, &m->shadow_dop);
	WRITE_ONCE(dentry->d_flags,
		   READ_ONCE(dentry->d_flags) | DCACHE_OP_REVALIDATE);
	spin_unlock(&dentry->d_lock);
	mutex_unlock(&sumh_dh_lock);
	return 0;

out_unlock:
	mutex_unlock(&sumh_dh_lock);
	dput(m->dentry);
	sumh_dh_payload_put(m->payload);
	kfree(m);
	return ret;
}

/* No allocation and no grace wait inside a lookup callback. A concurrent
 * retirement before publication may have restored d_op; after publication
 * it must either own and drop this exact dentry or fail this check. */
static bool
sumh_dh_validate_projection(struct sumh_dh_dir *dir, struct dentry *dentry,
			    const struct sumh_dh_projection *projection)
{
	struct sumh_dh_dop_meta *m;
	struct sumh_dh_child *slot;
	bool valid = false;

	mutex_lock(&sumh_dh_lock);
	if (READ_ONCE(dir->retiring))
		goto out;
	m = sumh_dh_dop_lookup_locked(dentry);
	if (!m || m->dir != dir || !m->projected ||
	    m->payload != projection->payload ||
	    m->synthetic_negative != projection->hidden ||
	    test_bit(SUMH_DH_DOP_AUTO_RETIRE, &m->state))
		goto out;
	spin_lock(&dentry->d_lock);
	if (dentry->d_op != &m->shadow_dop ||
	    d_inode(dentry->d_parent) != dir->dir_inode ||
	    dentry->d_name.len != m->name_len ||
	    memcmp(dentry->d_name.name, m->name, m->name_len))
		goto unlock_dentry;
	slot = sumh_dh_find_slot(dir, m->name, m->name_len, m->name_hash);
	valid = sumh_dh_projection_matches(slot, dir->dir_inode, projection);
unlock_dentry:
	spin_unlock(&dentry->d_lock);
out:
	mutex_unlock(&sumh_dh_lock);
	return valid;
}

static void sumh_dh_drop_projection(struct dentry *dentry)
{
	struct sumh_dh_dop_meta *m;

	mutex_lock(&sumh_dh_lock);
	m = sumh_dh_dop_lookup_locked(dentry);
	if (m)
		sumh_dh_queue_dop_retire(m);
	d_drop(dentry);
	mutex_unlock(&sumh_dh_lock);
}

/* Caller holds sumh_dh_lock.  The dget in @m remains live across the SRCU
 * drain; this prevents d_release/free while an old shadow callback can run. */
static void sumh_dh_retire_dop_locked(struct sumh_dh_dop_meta *m,
				      struct list_head *retired)
{
	struct dentry *dentry = m->dentry;

	spin_lock(&dentry->d_lock);
	if (dentry->d_op == &m->shadow_dop) {
		unsigned int flags = READ_ONCE(dentry->d_flags);

		WRITE_ONCE(dentry->d_flags,
			   (flags & ~SUMH_DH_DOP_FLAGS) | m->orig_op_flags);
		/* The shadow cloned every original callback, so temporarily
		 * publishing fewer operation flags while it remains installed
		 * is safe.  Publishing orig_dop first could expose an old
		 * REVALIDATE flag beside an original vector with a NULL
		 * d_revalidate. */
		smp_wmb();
		WRITE_ONCE(dentry->d_op, m->orig_dop);
	}
	spin_unlock(&dentry->d_lock);

	hash_del_rcu(&m->node);
	list_add_tail(&m->retire_node, retired);
	/* Restore first, then unhash.  External refs can keep the dentry alive,
	 * but no subsequent cache lookup may enter SUMH through this object.
	 */
	d_drop(dentry);
}

/* synchronize_srcu(sumh_dh_srcu) must have completed after retirement. */
static void sumh_dh_free_retired_dops(struct list_head *retired)
{
	struct sumh_dh_dop_meta *m, *tmp;

	list_for_each_entry_safe (m, tmp, retired, retire_node) {
		list_del(&m->retire_node);
		dput(m->dentry);
		sumh_dh_payload_put(m->payload);
		sumh_dh_dir_put(m->dir);
		kfree(m);
	}
}

/*
 * Retire every dentry that a revalidation declared stale.  The mutex
 * serializes ownership with DEL/CLEAR: whichever side removes a meta from the
 * hash owns its eventual dput/free, while the other side can no longer find
 * it. Each meta owns a directory reference through its child dput, so a
 * concurrent last-child DEL cannot free the directory before this worker.
 */
static void sumh_dh_reap_dops_workfn(struct work_struct *work)
{
	struct sumh_dh_dop_meta *m;
	struct hlist_node *tmp;
	LIST_HEAD(retired_dops);
	int bkt;

	(void)work;
	mutex_lock(&sumh_dh_lock);
	hash_for_each_safe(sumh_dh_dops, bkt, tmp, m, node)
	{
		if (test_bit(SUMH_DH_DOP_AUTO_RETIRE, &m->state))
			sumh_dh_retire_dop_locked(m, &retired_dops);
	}
	mutex_unlock(&sumh_dh_lock);

	if (list_empty(&retired_dops))
		return;

	/* Close a stale d_op load, drain callbacks that already published their
	 * meta through SRCU, then cover both the wrapper epilogue and hash
	 * readers before dropping the lifetime pin.
	 */
	synchronize_rcu_tasks();
	synchronize_srcu(&sumh_dh_srcu);
	synchronize_rcu_tasks();
	synchronize_rcu();
	sumh_dh_free_retired_dops(&retired_dops);

	/* The dput above may release the final vnode after the directory's last
	 * rule was concurrently deleted.  Give the shared s_op owner a chance
	 * to retire now that both its client and vnode counts can be zero.
	 */
	sumh_sop_shadow_reap();
}

/* ---- virtual-inode reclaim leaf functions ------------------------------
 * Called by the unified super_operations owner (sumh_sop_shadow) from its
 * destroy/evict/drop_inode trampolines, with @orig (the sb's original s_op)
 * already resolved under RCU.  The trampoline owns the active-counter drain, so
 * these bodies may early-return freely.  Behavior is identical to the former
 * SRCU-wrapped s_op callbacks; only the meta lookup + drain moved to the owner.
 */
void SUMH_NOCFI sumh_dh_reclaim_destroy_inode(
    struct inode *inode, const struct super_operations *orig)
{
	if (sumh_vnode_is_ours(inode))
		sumh_vnode_free_info(inode);
	if (orig && orig->destroy_inode)
		orig->destroy_inode(inode);
}

void SUMH_NOCFI sumh_dh_reclaim_evict_inode(struct inode *inode,
					    const struct super_operations *orig)
{
	if (sumh_vnode_is_ours(inode)) {
		truncate_inode_pages_final(&inode->i_data);
		clear_inode(inode);
		return;
	}
	if (orig && orig->evict_inode) {
		orig->evict_inode(inode);
	} else {
		truncate_inode_pages_final(&inode->i_data);
		clear_inode(inode);
	}
}

int SUMH_NOCFI sumh_dh_reclaim_drop_inode(struct inode *inode,
					  const struct super_operations *orig)
{
	if (sumh_vnode_is_ours(inode))
		return !inode->i_nlink || inode_unhashed(inode);
	if (orig && orig->drop_inode)
		return orig->drop_inode(inode);
	return !inode->i_nlink || inode_unhashed(inode);
}

/* ---- install ----------------------------------------------------------- */

/* Install (or, on a dir already carrying our lookup shadow, upgrade to add) the
 * iterate_shared shadow so readdir runs through sumh_dh_iterate.  Only inject
 * and hide children need it; lookup_only (merge) children leave readdir to the
 * overlay filldir, so their dir is installed iop-only and never gets this
 * shadow — lookup (i_op) and iterate (i_fop) are independent inode fields, so
 * the two layers never contend when dirhijack only owns lookup. */
static int sumh_dh_install_iterate(struct inode *inode, struct sumh_dh_dir *dir)
{
	int ret;

	if (dir->iterate_bound)
		return 0;
	ret = sumh_fop_install(inode);
	if (ret)
		return ret;
	ret = sumh_fop_bind_iterate_client(inode, sumh_dh_iterate, dir);
	if (ret)
		return ret;
	dir->iterate_bound = true;
	return 0;
}

static struct sumh_dh_dir *sumh_dh_install_dir(struct inode *inode,
					       bool need_iterate,
					       bool sop_registered, int *errp)
{
	struct sumh_dh_iop *iop_meta = sumh_dh_iop_of(inode);
	struct sumh_dh_iop *im;
	struct sumh_dh_dir *dir;
	const struct inode_operations *orig_iop;
	int ret = 0;

	if (iop_meta) {
		if (need_iterate)
			ret = sumh_dh_install_iterate(inode, iop_meta->dir);
		if (errp)
			*errp = ret;
		if (ret)
			return NULL;
		return iop_meta->dir;
	}
	orig_iop = inode->i_op;
	if (!orig_iop || !orig_iop->lookup)
		return NULL;

	dir = kzalloc(sizeof(*dir), GFP_KERNEL);
	if (!dir)
		return NULL;
	dir->dir_inode = igrab(inode);
	if (!dir->dir_inode) {
		kfree(dir);
		return NULL;
	}
	refcount_set(&dir->refs, 1);
	INIT_HLIST_HEAD(&dir->children);
	spin_lock_init(&dir->lock);
	INIT_LIST_HEAD(&dir->list);
	dir->sop_client = sop_registered;

	im = kzalloc(sizeof(*im), GFP_KERNEL);
	if (!im) {
		iput(dir->dir_inode);
		kfree(dir);
		return NULL;
	}
	im->fake_iop = *orig_iop;
	im->orig_iop = orig_iop;
	im->dir = dir;
	im->fake_iop.lookup = sumh_dh_lookup;
	if (orig_iop->atomic_open)
		im->fake_iop.atomic_open = sumh_dh_atomic_open;
	dir->iop_meta = im;

	if (need_iterate) {
		ret = sumh_dh_install_iterate(inode, dir);
		if (ret) {
			kfree(im);
			iput(dir->dir_inode);
			kfree(dir);
			if (errp)
				*errp = ret;
			return NULL;
		}
	}
	list_add_tail(&dir->list, &sumh_dh_dirs);
	smp_store_release(&inode->i_op, &im->fake_iop);
	if (errp)
		*errp = 0;
	return dir;
}

/* Withdraw an empty directory under sumh_dh_lock. The sleepable callback
 * drain and registry-reference release happen in sumh_dh_retire_finish(). */
static void sumh_dh_detach_dir_locked(struct sumh_dh_dir *dir)
{
	struct inode *inode;

	if (!dir)
		return;
	inode = dir->dir_inode;
	WRITE_ONCE(dir->retiring, true);
	if (inode && dir->iop_meta &&
	    READ_ONCE(inode->i_op) == &dir->iop_meta->fake_iop)
		smp_store_release(&inode->i_op, dir->iop_meta->orig_iop);
	if (inode && dir->iterate_bound) {
		sumh_fop_unbind_iterate_client(inode, dir);
		dir->iterate_bound = false;
	}
	if (!list_empty(&dir->list))
		list_del_init(&dir->list);
}

static int sumh_dh_split_parent(const char *visible_path, char **parent_out,
				const char **child_out)
{
	char *parent;
	char *slash;

	if (!visible_path || visible_path[0] != '/')
		return -EINVAL;
	parent = kstrdup(visible_path, GFP_KERNEL);
	if (!parent)
		return -ENOMEM;
	slash = strrchr(parent, '/');
	if (!slash || !slash[1]) {
		kfree(parent);
		return -EINVAL;
	}
	*child_out = visible_path + (slash - parent) + 1;
	if (slash == parent)
		parent[1] = '\0';
	else
		*slash = '\0';
	*parent_out = parent;
	return 0;
}

static void sumh_dh_shrink_dead_sbs(struct list_head *dead_dirs)
{
	struct sumh_dh_dir *dir;

	list_for_each_entry (dir, dead_dirs, list) {
		struct sumh_dh_dir *prior;
		struct super_block *sb;
		bool seen = false;

		if (!dir->dir_inode)
			continue;
		sb = dir->dir_inode->i_sb;
		list_for_each_entry (prior, dead_dirs, list) {
			if (prior == dir)
				break;
			if (prior->dir_inode && prior->dir_inode->i_sb == sb) {
				seen = true;
				break;
			}
		}
		if (!seen)
			shrink_dcache_sb(sb);
	}
}

/* SUMH_DH_OWNERS_BEGIN */
void sumh_dh_retire_init(struct sumh_dh_retire *retired, bool bulk)
{
	memset(retired, 0, sizeof(*retired));
	INIT_LIST_HEAD(&retired->slots);
	INIT_LIST_HEAD(&retired->dirs);
	INIT_LIST_HEAD(&retired->dops);
	INIT_LIST_HEAD(&retired->invalidations);
	retired->bulk = bulk;
}

/* These final puts run only in sleepable contexts outside config/DH. Registry
 * references enforce grace/callback completion independently of owner refs. */
static void sumh_dh_dir_put(struct sumh_dh_dir *dir)
{
	struct super_block *sb;

	if (!refcount_dec_and_test(&dir->refs))
		return;
	WARN_ON_ONCE(!dir->retiring || !hlist_empty(&dir->children));
	sb = dir->dir_inode->i_sb;
	iput(dir->dir_inode);
	if (dir->sop_client)
		sumh_sop_shadow_unregister_dh(sb);
	kfree(dir->iop_meta);
	kfree(dir);
	/* Ordinary DEL/abort may have no vnode and no later CLEAR to reap it.
	 * The bootstrap pin protects this helper and its worker epilogue. */
	sumh_sop_shadow_reap();
}

static void sumh_dh_slot_put(struct sumh_dh_child *slot)
{
	if (!refcount_dec_and_test(&slot->refs))
		return;
	WARN_ON_ONCE(slot->indexed || slot->attached || slot->prepared);
	dput(slot->cache_parent);
	sumh_dh_dir_put(slot->dir);
	kfree(slot);
}

static void sumh_dh_payload_put(struct sumh_dh_payload *payload)
{
	if (!payload || !refcount_dec_and_test(&payload->refs))
		return;
	if (payload->source.dentry)
		path_put(&payload->source);
	kfree(payload->vpath);
	kfree(payload);
}

static void sumh_dh_claim_free(struct sumh_dh_claim *claim)
{
	sumh_dh_payload_put(claim->payload);
	dput(claim->observed_parent);
	sumh_dh_slot_put(claim->slot);
	kfree(claim);
}

static void sumh_dh_claim_free_rcu(struct rcu_head *rcu)
{
	struct sumh_dh_claim *claim =
	    container_of(rcu, struct sumh_dh_claim, rcu);

	llist_add(&claim->free_node, &sumh_dh_released_claims);
	schedule_work(&sumh_dh_release_work);
}

static void sumh_dh_release_workfn(struct work_struct *work)
{
	struct llist_node *head, *node, *next;

	(void)work;
	while ((head = llist_del_all(&sumh_dh_released_claims)) != NULL) {
		llist_for_each_safe(node, next, head)
		{
			struct sumh_dh_claim *claim =
			    container_of(node, struct sumh_dh_claim, free_node);
			sumh_dh_claim_free(claim);
			cond_resched();
		}
	}
}

void sumh_dh_claim_put(struct sumh_dh_claim *claim)
{
	if (!claim || WARN_ON_ONCE(claim->state != DH_DETACHED))
		return;
	/* Payload readers are RCU-only. No logical mutation belongs here. */
	call_rcu(&claim->rcu, sumh_dh_claim_free_rcu);
}

void sumh_dh_drain_releases(void)
{
	/* Run AFTER store_drain: store work may enqueue this second RCU stage.
	 * Runtime CLEAR drains its withdrawn cohort; SU may submit later work.
	 */
	rcu_barrier();
	flush_work(&sumh_dh_release_work);
}

/* Caller drops the returned lookup reference outside any parent inode lock. */
static struct dentry *sumh_dh_drop_cached(struct dentry *parent,
					  const char *name)
{
	struct qstr qname = QSTR_INIT(name, strlen(name));
	struct dentry *cached;

	qname.hash = full_name_hash(parent, name, qname.len);
	cached = d_lookup(parent, &qname);
	if (cached)
		d_drop(cached);
	return cached;
}

static void sumh_dh_invalidate_parent(struct dentry *parent, const char *name)
{
	dput(sumh_dh_drop_cached(parent, name));
}

/* DH held, slot spinlock NOT held: FUSE takes dentry lock before slot lock. */
static void sumh_dh_retire_slot_dops(struct sumh_dh_child *slot,
				     struct sumh_dh_retire *retired)
{
	struct sumh_dh_dop_meta *dm;
	struct hlist_node *tmp;
	int bkt;

	hash_for_each_safe(sumh_dh_dops, bkt, tmp, dm, node)
	{
		if (dm->dir == slot->dir && dm->name_hash == slot->name_hash &&
		    dm->name_len == slot->name_len &&
		    !memcmp(dm->name, slot->name, slot->name_len))
			sumh_dh_retire_dop_locked(dm, &retired->dops);
	}
}

static void sumh_dh_record_invalidation(struct sumh_dh_claim *claim,
					struct sumh_dh_retire *retired)
{
	struct sumh_dh_child *slot = claim->slot;

	if (retired->bulk) {
		/* Bulk callers are mutation-serialized. One reserved reference
		 * of headroom covers this list even if PREPARED keeps the slot
		 * indexed. */
		if (list_empty(&slot->invalidate_node)) {
			refcount_inc(&slot->refs);
			list_add_tail(&slot->invalidate_node,
				      &retired->invalidations);
		}
	} else {
		struct sumh_dh_invalidation *item;

		if (WARN_ON_ONCE(retired->count >= ARRAY_SIZE(retired->single)))
			return;
		item = &retired->single[retired->count++];
		item->parent = dget(claim->observed_parent);
		memcpy(item->name, slot->name, slot->name_len + 1);
	}
	sumh_dh_retire_slot_dops(slot, retired);
}

static void sumh_dh_retire_empty_slot(struct sumh_dh_child *slot,
				      struct sumh_dh_retire *retired)
{
	struct sumh_dh_dir *dir = slot->dir;
	struct sumh_dh_dop_meta *dm;
	struct hlist_node *tmp;
	int bkt;

	if (!slot->indexed || slot->attached || slot->prepared)
		return;
	hlist_del_rcu(&slot->node);
	slot->indexed = false;
	list_add_tail(&slot->retire_node, &retired->slots);
	if (!hlist_empty(&dir->children))
		return;
	sumh_dh_detach_dir_locked(dir);
	list_add_tail(&dir->list, &retired->dirs);
	hash_for_each_safe(sumh_dh_dops, bkt, tmp, dm, node) if (dm->dir == dir)
	    sumh_dh_retire_dop_locked(dm, &retired->dops);
}

/* The owner list is individually ordered even for identical topology values. */
static void sumh_dh_select_winner(struct sumh_dh_child *slot)
{
	struct sumh_dh_claim *claim;
	struct sumh_dh_payload *winner = NULL;

	list_for_each_entry (claim, &slot->owners, owner_node) {
		if (claim->payload) {
			winner = claim->payload;
			break;
		}
	}
	rcu_assign_pointer(slot->winner, winner);
}

static void sumh_dh_detach_locked(struct sumh_dh_claim *claim,
				  struct sumh_dh_retire *retired)
{
	struct sumh_dh_child *slot;

	if (!claim || claim->state == DH_DETACHED)
		return;
	if (WARN_ON_ONCE(claim->state != DH_ATTACHED))
		return;
	slot = claim->slot;
	spin_lock(&slot->lock);
	list_del_init(&claim->owner_node);
	slot->attached--;
	if (claim->kind == SUMH_DH_OWNER_MANAGED_HIDE)
		slot->hide_users--;
	claim->state = DH_DETACHED;
	sumh_dh_select_winner(slot);
	spin_unlock(&slot->lock);
	sumh_dh_record_invalidation(claim, retired);
	sumh_dh_retire_empty_slot(slot, retired);
}

void sumh_dh_detach(struct sumh_dh_claim *claim, struct sumh_dh_retire *retired)
{
	mutex_lock(&sumh_dh_lock);
	sumh_dh_detach_locked(claim, retired);
	mutex_unlock(&sumh_dh_lock);
}

int sumh_dh_change_begin(struct sumh_dh_claim *add,
			 struct sumh_dh_claim *remove,
			 struct sumh_dh_change *change)
{
	struct sumh_dh_child *slot;
	struct sumh_dh_claim *owner;
	int ret = 0;

	mutex_lock(&sumh_dh_lock);
	if (!add)
		goto ready;
	if (!READ_ONCE(sumh_dh_ready)) {
		ret = -ESHUTDOWN;
		goto fail;
	}
	if (add->state == DH_INVALIDATED) {
		ret = -EAGAIN;
		goto fail;
	}
	slot = add->slot;
	if (add->state != DH_PREPARED || !slot->indexed || !slot->prepared ||
	    slot->dir->retiring) {
		ret = -EINVAL;
		goto fail;
	}
	list_for_each_entry (owner, &slot->owners, owner_node) {
		if (owner == remove)
			continue;
		if (owner->kind == SUMH_DH_OWNER_SU ||
		    add->kind == SUMH_DH_OWNER_SU) {
			ret = add->kind == SUMH_DH_OWNER_SU ? -EEXIST : -EPERM;
			goto fail;
		}
	}
	if (slot->attached == UINT_MAX ||
	    (add->kind == SUMH_DH_OWNER_MANAGED_HIDE &&
	     slot->hide_users == UINT_MAX)) {
		ret = -EOVERFLOW;
		goto fail;
	}
ready:
	if (remove && remove->state != DH_ATTACHED &&
	    remove->state != DH_DETACHED) {
		ret = -EINVAL;
		goto fail;
	}
	change->add = add;
	change->remove = remove;
	return 0;
fail:
	mutex_unlock(&sumh_dh_lock);
	return ret;
}

void sumh_dh_change_commit(struct sumh_dh_change *change,
			   struct sumh_dh_retire *retired)
{
	struct sumh_dh_claim *claim = change->add;

	if (claim) {
		struct sumh_dh_child *slot = claim->slot;

		spin_lock(&slot->lock);
		list_del_init(&claim->owner_node);
		slot->prepared--;
		list_add(&claim->owner_node, &slot->owners);
		slot->attached++;
		if (claim->kind == SUMH_DH_OWNER_MANAGED_HIDE)
			slot->hide_users++;
		claim->state = DH_ATTACHED;
		sumh_dh_select_winner(slot);
		spin_unlock(&slot->lock);
		sumh_dh_record_invalidation(claim, retired);
	}
	sumh_dh_detach_locked(change->remove, retired);
}

void sumh_dh_change_end(void)
{
	mutex_unlock(&sumh_dh_lock);
}

void sumh_dh_retire_finish(struct sumh_dh_retire *retired)
{
	struct sumh_dh_child *slot, *stmp;
	struct sumh_dh_dir *dir, *dtmp;
	unsigned int i;
	bool callbacks =
	    !list_empty(&retired->dirs) || !list_empty(&retired->dops);

	if (callbacks) {
		synchronize_rcu_tasks();
		synchronize_srcu(&sumh_dh_srcu);
		sumh_fop_synchronize_iterate_clients();
		synchronize_rcu_tasks();
	}
	if (callbacks || !list_empty(&retired->slots))
		synchronize_rcu();
	sumh_dh_free_retired_dops(&retired->dops);
	for (i = 0; i < retired->count; i++) {
		sumh_dh_invalidate_parent(retired->single[i].parent,
					  retired->single[i].name);
		dput(retired->single[i].parent);
	}
	retired->count = 0;
	list_for_each_entry_safe (slot, stmp, &retired->invalidations,
				  invalidate_node) {
		/* Only the serialized bulk producer uses this node. */
		list_del_init(&slot->invalidate_node);
		sumh_dh_invalidate_parent(slot->cache_parent, slot->name);
		sumh_dh_slot_put(slot);
	}
	sumh_dh_shrink_dead_sbs(&retired->dirs);
	list_for_each_entry_safe (slot, stmp, &retired->slots, retire_node) {
		list_del_init(&slot->retire_node);
		sumh_dh_slot_put(slot);
	}
	list_for_each_entry_safe (dir, dtmp, &retired->dirs, list) {
		list_del_init(&dir->list);
		sumh_dh_dir_put(dir);
	}
}

void sumh_dh_abort(struct sumh_dh_claim *claim)
{
	struct sumh_dh_retire retired;

	if (!claim)
		return;
	sumh_dh_retire_init(&retired, false);
	mutex_lock(&sumh_dh_lock);
	if (WARN_ON_ONCE(claim->state != DH_PREPARED &&
			 claim->state != DH_INVALIDATED)) {
		mutex_unlock(&sumh_dh_lock);
		return;
	}
	if (claim->state == DH_PREPARED) {
		list_del_init(&claim->owner_node);
		claim->slot->prepared--;
		claim->state = DH_INVALIDATED;
		sumh_dh_retire_empty_slot(claim->slot, &retired);
	}
	mutex_unlock(&sumh_dh_lock);
	sumh_dh_retire_finish(&retired);
	/* Unpublished payload was never a winner and needs no grace of its own.
	 */
	sumh_dh_claim_free(claim);
}

static int sumh_dh_prepare_observed(const struct path *parent, const char *name,
				    const struct sumh_dh_spec *spec,
				    const char *vpath,
				    struct sumh_dh_claim **out)
{
	struct inode *inode =
	    parent && parent->dentry ? d_inode(parent->dentry) : NULL;
	struct sumh_dh_child *slot, *spare = NULL;
	struct sumh_dh_claim *claim = NULL;
	struct sumh_dh_dir *dir = NULL;
	struct dentry *cached = NULL;
	struct sumh_dh_retire retired;
	bool registered = false;
	size_t len = name ? strlen(name) : 0;
	int ret = -ENOMEM;

	*out = NULL;
	if (!inode || !S_ISDIR(inode->i_mode) || !len || len > NAME_MAX ||
	    strchr(name, '/'))
		return -EINVAL;
	if (sumh_vnode_is_ours(inode))
		return -EOPNOTSUPP;
	if ((spec->kind == SUMH_DH_OWNER_LEGACY_HIDE ||
	     spec->kind == SUMH_DH_OWNER_MANAGED_HIDE) &&
	    inode->i_sb->s_magic == FUSE_SUPER_MAGIC &&
	    !READ_ONCE(sumh_dh_fuse_registered))
		return -EOPNOTSUPP;
	claim = kzalloc(sizeof(*claim), GFP_KERNEL);
	spare = kzalloc(sizeof(*spare) + len + 1, GFP_KERNEL);
	if (!claim || !spare)
		goto fail;
	if (spec->kind != SUMH_DH_OWNER_MANAGED_HIDE) {
		claim->payload = kzalloc(sizeof(*claim->payload), GFP_KERNEL);
		if (!claim->payload)
			goto fail;
		refcount_set(&claim->payload->refs, 1);
		if (vpath) {
			claim->payload->vpath = kstrdup(vpath, GFP_KERNEL);
			if (!claim->payload->vpath)
				goto fail;
		}
		if (spec->source) {
			claim->payload->source = *spec->source;
			path_get(&claim->payload->source);
		}
		claim->payload->v_ino = spec->v_ino;
		claim->payload->flags = spec->flags;
		claim->payload->hide = spec->kind == SUMH_DH_OWNER_LEGACY_HIDE;
		claim->payload->lookup_only =
		    spec->kind == SUMH_DH_OWNER_SHADOW;
	}
	claim->kind = spec->kind;
	claim->state = DH_PREPARED;
	claim->observed_parent = dget(parent->dentry);
	INIT_LIST_HEAD(&claim->owner_node);
	sumh_dh_retire_init(&retired, false);
	/* Normal native lookup/atomic_open runs with this parent locked.
	 * Drain callbacks started before shadow publication. Callers prepare
	 * outside config; callbacks already take inode -> DH, never reverse. */
	inode_lock(inode);
	mutex_lock(&sumh_dh_lock);
	if (!READ_ONCE(sumh_dh_ready)) {
		ret = -ESHUTDOWN;
		goto unlock;
	}
	if (!sumh_dh_iop_of(inode)) {
		ret = sumh_sop_shadow_register_dh(inode->i_sb);
		if (ret)
			goto unlock;
		registered = true;
	}
	dir = sumh_dh_install_dir(inode, spec->kind != SUMH_DH_OWNER_SHADOW,
				  registered, &ret);
	if (!dir) {
		ret = ret ?: -ENOMEM;
		if (registered)
			sumh_sop_shadow_unregister_dh(inode->i_sb);
		goto unlock;
	}
	slot =
	    sumh_dh_find_slot(dir, name, len, full_name_hash(inode, name, len));
	if (slot) {
		if (slot->prepared == UINT_MAX ||
		    refcount_read(&slot->refs) >= INT_MAX - 4) {
			ret = -EOVERFLOW;
			goto unlock;
		}
	} else {
		if (refcount_read(&dir->refs) >= INT_MAX - 4) {
			ret = -EOVERFLOW;
			goto unlock;
		}
		slot = spare;
		spare = NULL;
		refcount_set(&slot->refs, 1); /* index */
		refcount_inc(&dir->refs);
		slot->dir = dir;
		slot->cache_parent = dget(parent->dentry);
		slot->name_len = len;
		slot->name_hash = full_name_hash(inode, name, len);
		memcpy(slot->name, name, len + 1);
		spin_lock_init(&slot->lock);
		INIT_LIST_HEAD(&slot->owners);
		INIT_LIST_HEAD(&slot->prepared_claims);
		INIT_LIST_HEAD(&slot->retire_node);
		INIT_LIST_HEAD(&slot->invalidate_node);
		slot->indexed = true;
		hlist_add_head_rcu(&slot->node, &dir->children);
	}
	refcount_inc(&slot->refs); /* caller claim */
	slot->prepared++;
	claim->slot = slot;
	list_add_tail(&claim->owner_node, &slot->prepared_claims);
	cached = sumh_dh_drop_cached(parent->dentry, name);
	if (cached) {
		struct sumh_dh_dop_meta *dm = sumh_dh_dop_lookup_locked(cached);

		if (dm)
			sumh_dh_queue_dop_retire(dm);
	}
	ret = 0;
unlock:
	if (ret && dir && hlist_empty(&dir->children)) {
		sumh_dh_detach_dir_locked(dir);
		list_add_tail(&dir->list, &retired.dirs);
	}
	mutex_unlock(&sumh_dh_lock);
	inode_unlock(inode);
	dput(cached);
	sumh_dh_retire_finish(&retired);
	if (ret && registered && !dir)
		sumh_sop_shadow_reap();
	if (ret)
		goto fail;
	kfree(spare);
	*out = claim;
	return 0;
fail:
	if (claim) {
		sumh_dh_payload_put(claim->payload);
		if (claim->observed_parent)
			dput(claim->observed_parent);
		kfree(claim);
	}
	kfree(spare);
	return ret;
}

int sumh_dh_prepare_parent(const struct path *parent, const char *name,
			   const struct sumh_dh_spec *spec,
			   struct sumh_dh_claim **out)
{
	if (!out || !spec)
		return -EINVAL;
	return sumh_dh_prepare_observed(parent, name, spec, NULL, out);
}

int SUMH_NOCFI sumh_dh_prepare_path(const char *visible,
				    const struct sumh_dh_spec *spec,
				    struct sumh_dh_claim **out,
				    struct path *native_parent)
{
	struct path parent = {};
	struct sumh_dh_spec top;
	char *parent_name = NULL, *probe = NULL, *segment = NULL, *vpath = NULL;
	const char *child, *after, *slash;
	size_t base, len;
	int ret;

	if (!out || !spec || !visible)
		return -EINVAL;
	*out = NULL;
	if (native_parent)
		memset(native_parent, 0, sizeof(*native_parent));
	ret = sumh_dh_split_parent(visible, &parent_name, &child);
	if (ret)
		return ret;
	probe = kstrdup(parent_name, GFP_KERNEL);
	if (!probe) {
		ret = -ENOMEM;
		goto out;
	}
	ret = kern_path(probe, LOOKUP_FOLLOW | LOOKUP_DIRECTORY, &parent);
	if (ret == -ENOENT && spec->kind == SUMH_DH_OWNER_MAPPING) {
		/* Retry the immediate parent once: it may have appeared since
		 * the failed observation. A successful retry is an ordinary
		 * mapping. */
		for (;;) {
			char *last;
			ret = kern_path(probe, LOOKUP_FOLLOW | LOOKUP_DIRECTORY,
					&parent);
			if (!ret || ret != -ENOENT)
				break;
			last = strrchr(probe, '/');
			if (!last || (last == probe && !probe[1]))
				break;
			if (last == probe)
				probe[1] = '\0';
			else
				*last = '\0';
		}
	}
	if (ret)
		goto out;
	if (!strcmp(probe, parent_name)) {
		if (spec->kind == SUMH_DH_OWNER_SU) {
			struct path existing;
			ret = kern_path(visible, 0, &existing);
			if (!ret) {
				ret = ksu_sucompat_vfs_is_path(&existing)
					  ? 0
					  : -EEXIST;
				path_put(&existing);
			} else if (ret == -ENOENT) {
				ret = 0;
			}
			if (ret)
				goto out_path;
		}
		ret = sumh_dh_prepare_observed(&parent, child, spec, NULL, out);
		if (!ret && native_parent) {
			*native_parent = parent;
			path_get(native_parent);
		}
	} else {
		base = !strcmp(probe, "/") ? 0 : strlen(probe);
		after = visible + base + 1;
		slash = strchr(after, '/');
		len = slash ? (size_t)(slash - after) : strlen(after);
		segment = kstrndup(after, len, GFP_KERNEL);
		vpath = kstrndup(visible, base + 1 + len, GFP_KERNEL);
		if (!segment || !vpath) {
			ret = -ENOMEM;
			goto out_path;
		}
		memset(&top, 0, sizeof(top));
		top.kind = SUMH_DH_OWNER_VTOPO;
		top.v_ino = sumh_vnode_vpath_ino(vpath);
		top.flags = SUMH_VNODE_F_DIR | SUMH_VNODE_F_VIRTUAL_DIR;
		ret = sumh_dh_prepare_observed(&parent, segment, &top, vpath,
					       out);
	}
out_path:
	path_put(&parent);
out:
	kfree(segment);
	kfree(vpath);
	kfree(probe);
	kfree(parent_name);
	return ret;
}

bool sumh_dh_claim_matches_parent(const struct sumh_dh_claim *claim,
				  const struct path *parent)
{
	return claim && parent && claim->state == DH_ATTACHED &&
	       claim->observed_parent == parent->dentry &&
	       claim->slot->dir->dir_inode == d_inode(parent->dentry) &&
	       !d_unlinked(claim->observed_parent);
}

int SUMH_NOCFI sumh_dirhijack_match_su(const char *visible,
				       const struct sumh_dh_claim *claim,
				       unsigned long ino)
{
	char *parent_name = NULL;
	const char *name;
	struct path parent;
	int ret;

	if (!claim || !ino)
		return 0;
	ret = sumh_dh_split_parent(visible, &parent_name, &name);
	if (ret)
		return ret;
	ret = kern_path(parent_name, LOOKUP_FOLLOW | LOOKUP_DIRECTORY, &parent);
	if (!ret) {
		mutex_lock(&sumh_dh_lock);
		ret = sumh_dh_claim_matches_parent(claim, &parent) &&
		      claim->kind == SUMH_DH_OWNER_SU &&
		      claim->payload->v_ino == ino &&
		      !strcmp(claim->slot->name, name);
		mutex_unlock(&sumh_dh_lock);
		path_put(&parent);
	}
	kfree(parent_name);
	return ret;
}

struct sumh_hide_binding {
	struct sumh_dh_claim *claim;
};

bool sumh_dirhijack_hide_matches(const struct sumh_hide_binding *binding,
				 const struct path *parent)
{
	return binding && sumh_dh_claim_matches_parent(binding->claim, parent);
}

int sumh_dirhijack_hide_get(const struct path *parent, const char *name,
			    struct sumh_hide_binding **result)
{
	struct sumh_dh_spec spec = {.kind = SUMH_DH_OWNER_MANAGED_HIDE};
	struct sumh_dh_change change;
	struct sumh_dh_retire retired;
	struct sumh_hide_binding *binding;
	int ret;

	*result = NULL;
	binding = kzalloc(sizeof(*binding), GFP_KERNEL);
	if (!binding)
		return -ENOMEM;
	ret = sumh_dh_prepare_parent(parent, name, &spec, &binding->claim);
	if (ret)
		goto fail;
	ret = sumh_dh_change_begin(binding->claim, NULL, &change);
	if (ret) {
		sumh_dh_abort(binding->claim);
		goto fail;
	}
	sumh_dh_retire_init(&retired, false);
	sumh_dh_change_commit(&change, &retired);
	sumh_dh_change_end();
	sumh_dh_retire_finish(&retired);
	*result = binding;
	return 0;
fail:
	kfree(binding);
	return ret;
}

void sumh_dirhijack_hide_put(struct sumh_hide_binding *binding)
{
	struct sumh_dh_retire retired;

	if (!binding)
		return;
	sumh_dh_retire_init(&retired, false);
	sumh_dh_detach(binding->claim, &retired);
	sumh_dh_retire_finish(&retired);
	sumh_dh_claim_put(binding->claim);
	kfree(binding);
}

bool sumh_dirhijack_hidden(struct inode *parent, const char *name, int len)
{
	struct qstr qname = QSTR_INIT(name, len);
	int idx = srcu_read_lock(&sumh_dh_srcu);
	bool hidden = sumh_dh_hidden_name(parent, &qname);

	srcu_read_unlock(&sumh_dh_srcu, idx);
	return hidden;
}

bool sumh_dirhijack_emits(struct inode *parent, const char *name, int len)
{
	struct sumh_dh_iop *meta;
	struct sumh_dh_child *child;
	struct sumh_dh_view view;
	bool emits = false;
	int idx = srcu_read_lock(&sumh_dh_srcu);

	meta = sumh_dh_iop_of(parent);
	if (!meta || !meta->dir || READ_ONCE(meta->dir->retiring))
		goto out;
	rcu_read_lock();
	child = sumh_dh_find_child(meta->dir, name, (u16)len,
				   full_name_hash(parent, name, len));
	if (sumh_dh_read_view(child, &view))
		emits = sumh_dh_child_emitted(&view, parent);
	rcu_read_unlock();
out:
	srcu_read_unlock(&sumh_dh_srcu, idx);
	return emits;
}

void sumh_dirhijack_clear(void)
{
	struct sumh_dh_dir *dir, *dtmp;
	struct sumh_dh_child *slot;
	struct hlist_node *tmp;
	struct sumh_dh_claim *claim, *ctmp;
	struct sumh_dh_retire retired;

	sumh_dh_retire_init(&retired, true);
	mutex_lock(&sumh_dh_lock);
	list_for_each_entry_safe (dir, dtmp, &sumh_dh_dirs, list) {
		hlist_for_each_entry_safe(slot, tmp, &dir->children, node)
		{
			list_for_each_entry_safe (
			    claim, ctmp, &slot->prepared_claims, owner_node) {
				list_del_init(&claim->owner_node);
				slot->prepared--;
				claim->state = DH_INVALIDATED;
			}
			list_for_each_entry_safe (claim, ctmp, &slot->owners,
						  owner_node) {
				if (READ_ONCE(sumh_dh_ready) &&
				    claim->kind == SUMH_DH_OWNER_SU)
					continue;
				sumh_dh_detach_locked(claim, &retired);
			}
			sumh_dh_retire_empty_slot(slot, &retired);
		}
	}
	mutex_unlock(&sumh_dh_lock);
	sumh_dh_retire_finish(&retired);
	flush_work(&sumh_dh_reap_dops_work);
}
/* SUMH_DH_OWNERS_END */

int sumh_dirhijack_init(void)
{
	unsigned long addr = sumh_lookup_name("fuse_dentry_revalidate");
	int ret = -EOPNOTSUPP;

#ifdef CONFIG_ARM64
	if (addr) {
		sumh_dh_fuse_probe.addr = (kprobe_opcode_t *)addr;
		sumh_dh_fuse_probe.pre_handler = sumh_dh_fuse_revalidate_pre;
		ret = register_kprobe(&sumh_dh_fuse_probe);
	}
#endif
	WRITE_ONCE(sumh_dh_fuse_registered, !ret);
	if (ret)
		pr_warn("sumh: FUSE hide cache guard unavailable: %d\n", ret);

	hash_init(sumh_dh_dops);
	WRITE_ONCE(sumh_dh_ready, true);
	pr_info("sumh: dirhijack initialized (FUSE cache guard=%d)\n",
		sumh_dh_fuse_registered);
	return 0;
}

void sumh_dirhijack_stop_new(void)
{
	WRITE_ONCE(sumh_dh_ready, false);
	if (sumh_dh_fuse_registered) {
		unregister_kprobe(&sumh_dh_fuse_probe);
		WRITE_ONCE(sumh_dh_fuse_registered, false);
	}
}

void sumh_dirhijack_exit(void)
{
	sumh_dirhijack_stop_new();
	sumh_dirhijack_clear();
	pr_info("sumh: dirhijack exited\n");
}
