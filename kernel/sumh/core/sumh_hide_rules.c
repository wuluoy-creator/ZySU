#include <linux/refcount.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>

#include "sumh_dirhijack.h"
#include "sumh_hide_events.h"
#include "sumh_hide_rules.h"
#include "sumh_runtime.h"

struct managed_hide_rule {
	struct list_head list;
	refcount_t refs;
	u64 id;
	u64 generation;
	u64 processed;
	bool deleting;
	u32 state;
	int error;
	struct sumh_hide_binding *binding;
	struct sumh_hide_watch *watches;
	char *parent;
	char *name;
	char path[];
};

/* lifecycle admission -> mutation -> dirhijack -> sop/fop. The resolver
 * drops mutation for path lookup and never acquires lifecycle admission. */
DEFINE_MUTEX(sumh_mutation_mutex);
static LIST_HEAD(hide_rules);
static struct sumh_hide_events hide_events;
static struct workqueue_struct *hide_queue;
static struct task_struct *hide_resolver;
static atomic64_t hide_event_seq = ATOMIC64_INIT(1);
static u64 hide_next_id;
static bool hide_stopping;
static bool hide_initialized;
static bool hide_attempted;
static DEFINE_SPINLOCK(hide_schedule_lock);
static atomic_t hide_jobs = ATOMIC_INIT(0);

static void hide_rule_put(struct managed_hide_rule *rule)
{
	if (refcount_dec_and_test(&rule->refs)) {
		kfree(rule->parent);
		kfree(rule);
	}
}

static void hide_workfn(struct work_struct *work);
static DECLARE_WORK(hide_work, hide_workfn);

static void hide_event(void *data)
{
	unsigned long flags;

	atomic64_inc(&hide_event_seq);
	spin_lock_irqsave(&hide_schedule_lock, flags);
	if (!hide_stopping && READ_ONCE(sumh_enabled)) {
		atomic_inc(&hide_jobs);
		if (!queue_work(hide_queue, &hide_work))
			atomic_dec(&hide_jobs);
	}
	spin_unlock_irqrestore(&hide_schedule_lock, flags);
}

bool sumh_hide_rules_resolving(void)
{
	return current == READ_ONCE(hide_resolver);
}

static void hide_workfn(struct work_struct *work)
{
	struct managed_hide_rule *rule, *selected;
	struct sumh_hide_binding *candidate;
	struct sumh_hide_watch *watches;
	struct path parent;
	u64 sequence, generation, cursor = 0;
	bool resolved, same;
	int ret;

	for (;;) {
		mutex_lock(&sumh_mutation_mutex);
		if (hide_stopping || !READ_ONCE(sumh_enabled))
			goto done;
		sequence = atomic64_read(&hide_event_seq);
		ret = sumh_hide_events_ack(&hide_events);
		selected = NULL;
		list_for_each_entry (rule, &hide_rules, list) {
			if (ret < 0) {
				rule->error = ret;
				rule->state = SUMH_USER_HIDE_ERROR;
			}
			if (rule->processed != sequence && rule->id > cursor &&
			    !selected)
				selected = rule;
		}
		if (!selected && cursor && ret >= 0) {
			cursor = 0;
			list_for_each_entry (rule, &hide_rules, list) {
				if (rule->processed != sequence) {
					selected = rule;
					break;
				}
			}
		}
		if (ret < 0 || !selected)
			goto done;
		rule = selected;
		cursor = rule->id;
		refcount_inc(&rule->refs);
		generation = rule->generation;
		rule->state = SUMH_USER_HIDE_BINDING;
		mutex_unlock(&sumh_mutation_mutex);

		memset(&parent, 0, sizeof(parent));
		watches = NULL;
		WRITE_ONCE(hide_resolver, current);
		ret = sumh_hide_events_watch_path(
		    &hide_events, rule->parent, rule->name, &parent, &watches);
		WRITE_ONCE(hide_resolver, NULL);

		mutex_lock(&sumh_mutation_mutex);
		if (hide_stopping || rule->deleting ||
		    rule->generation != generation || !READ_ONCE(sumh_enabled))
			goto next;
		candidate = NULL;
		resolved = !ret;
		same = resolved &&
		       sumh_dirhijack_hide_matches(rule->binding, &parent);
		if (!ret || ret == -ENOENT || !rule->watches) {
			sumh_hide_events_unwatch(rule->watches);
			rule->watches = watches;
			watches = NULL;
		}
		if (resolved && !same)
			ret = sumh_dirhijack_hide_get(&parent, rule->name,
						      &candidate);
		if (candidate || ret == -ENOENT || ret == -ENOTDIR ||
		    (resolved && !same)) {
			sumh_dirhijack_hide_put(rule->binding);
			rule->binding = candidate;
		}
		rule->state = !ret	       ? SUMH_USER_HIDE_BOUND
			      : ret == -ENOENT ? SUMH_USER_HIDE_PENDING
					       : SUMH_USER_HIDE_ERROR;
		rule->error = ret;
		rule->processed = sequence;
	next:
		mutex_unlock(&sumh_mutation_mutex);
		if (parent.dentry)
			path_put(&parent);
		sumh_hide_events_unwatch(watches);
		hide_rule_put(rule);
		cond_resched();
	}
done:
	mutex_unlock(&sumh_mutation_mutex);
	atomic_dec(&hide_jobs);
}

bool sumh_hide_rules_available(void)
{
	int ret;

	if (hide_stopping || !sumh_dirhijack_enabled())
		return false;
	if (hide_initialized)
		return !READ_ONCE(hide_events.error);
	/* Keep a failed probe in legacy mode for this load. A later capability
	 * change would mix unowned legacy user HIDE entries with managed ones.
	 */
	if (hide_attempted)
		return false;
	hide_attempted = true;
	hide_queue = alloc_ordered_workqueue("sumh_hide", WQ_MEM_RECLAIM);
	if (!hide_queue)
		return false;
	ret = sumh_hide_events_open(&hide_events, hide_event, NULL);
	if (ret) {
		destroy_workqueue(hide_queue);
		hide_queue = NULL;
		pr_warn_ratelimited("sumh: managed hide unavailable: %d\n",
				    ret);
		return false;
	}
	hide_initialized = true;
	return true;
}

void sumh_hide_rules_changed(void)
{
	if (hide_initialized)
		hide_event(NULL);
}

static void hide_remove(struct managed_hide_rule *rule)
{
	rule->deleting = true;
	rule->generation++;
	list_del(&rule->list);
	sumh_hide_events_unwatch(rule->watches);
	sumh_dirhijack_hide_put(rule->binding);
	rule->binding = NULL;
	hide_rule_put(rule);
}

void sumh_hide_rules_clear(void)
{
	struct managed_hide_rule *rule, *tmp;

	atomic64_inc(&hide_event_seq);
	list_for_each_entry_safe (rule, tmp, &hide_rules, list)
		hide_remove(rule);
}

static void hide_stop_new(void)
{
	unsigned long flags;

	spin_lock_irqsave(&hide_schedule_lock, flags);
	WRITE_ONCE(hide_stopping, true);
	spin_unlock_irqrestore(&hide_schedule_lock, flags);
	if (hide_initialized)
		sumh_hide_events_stop(&hide_events);
}

void sumh_hide_rules_stop(void)
{
	hide_stop_new();
	if (!hide_initialized)
		return;
	mutex_unlock(&sumh_mutation_mutex);
	if (cancel_work_sync(&hide_work))
		atomic_dec(&hide_jobs);
	mutex_lock(&sumh_mutation_mutex);
	sumh_hide_rules_clear();
	destroy_workqueue(hide_queue);
	sumh_hide_events_close(&hide_events);
	hide_initialized = false;
	hide_queue = NULL;
}

bool sumh_hide_rules_quiesce(void)
{
	hide_stop_new();
	if (cancel_work(&hide_work))
		atomic_dec(&hide_jobs);
	if (atomic_read(&hide_jobs))
		return false;
	/* No producer can queue and no resolver can block now. Drain the
	 * callback epilogue before retiring its scope and module callbacks. */
	sumh_hide_rules_stop();
	return true;
}

int sumh_hide_rule_inventory(u64 cursor,
			     struct sumh_rule_inventory_entry *entry,
			     bool *more)
{
	struct managed_hide_rule *rule;
	u64 seen = 0;
	int count = 0;
	size_t length;

	*more = false;
	list_for_each_entry (rule, &hide_rules, list) {
		if (seen++ < cursor)
			continue;
		if (count) {
			*more = true;
			break;
		}
		length = strnlen(rule->path, sizeof(entry->path1));
		if (length == sizeof(entry->path1))
			return -EOVERFLOW;
		entry->id_kind = SUMH_INVENTORY_ID_MANAGED_DEFINITION;
		entry->owner = SUMH_INVENTORY_OWNER_USER_HIDE;
		entry->id = rule->id;
		entry->definition_generation = rule->generation;
		entry->path1_len = length;
		memcpy(entry->path1, rule->path, length);
		count = 1;
	}
	return cursor > seen ? -EINVAL : count;
}

static void hide_snapshot(struct sumh_user_hide_arg *arg,
			  const struct managed_hide_rule *rule)
{
	memset(arg, 0, sizeof(*arg));
	arg->size = sizeof(*arg);
	if (!rule)
		return;
	arg->rule_id = rule->id;
	arg->generation = rule->generation;
	arg->management = READ_ONCE(sumh_enabled) ? SUMH_USER_HIDE_LIVE
						  : SUMH_USER_HIDE_SUSPENDED;
	arg->binding = rule->state;
	arg->error = rule->error;
	arg->path_len = strlen(rule->path);
	memcpy(arg->path, rule->path, arg->path_len + 1);
}

long sumh_hide_rules_ioctl(unsigned int cmd, void __user *user)
{
	struct sumh_user_hide_arg *arg;
	struct managed_hide_rule *rule, *match = NULL;
	char *slash;
	int ret = 0;

	if (hide_stopping)
		return -ESHUTDOWN;
	if (cmd == SUMH_IOC_USER_HIDE_CLEAR) {
		sumh_hide_rules_clear();
		return 0;
	}
	arg = memdup_user(user, sizeof(*arg));
	if (IS_ERR(arg))
		return PTR_ERR(arg);
	if (arg->size != sizeof(*arg) || arg->flags || arg->generation ||
	    arg->reserved_id || arg->management || arg->binding || arg->error ||
	    arg->reserved[0] || arg->reserved[1] ||
	    arg->path_len >= sizeof(arg->path) || arg->path[arg->path_len] ||
	    strnlen(arg->path, arg->path_len) != arg->path_len) {
		ret = -EINVAL;
		goto out;
	}
	if (cmd == SUMH_IOC_USER_HIDE_QUERY) {
		if (arg->path_len) {
			ret = -EINVAL;
			goto out;
		}
		list_for_each_entry (rule, &hide_rules, list) {
			if (rule->id > arg->rule_id) {
				match = rule;
				break;
			}
		}
		goto reply;
	}
	list_for_each_entry (rule, &hide_rules, list) {
		if (arg->rule_id ? rule->id == arg->rule_id
				 : !strcmp(rule->path, arg->path)) {
			match = rule;
			break;
		}
	}
	if (cmd == SUMH_IOC_USER_HIDE_DELETE) {
		if ((!arg->rule_id && !arg->path_len) ||
		    (arg->path_len && arg->path[0] != '/')) {
			ret = -EINVAL;
			goto out;
		}
		if (match) {
			if (arg->path_len && strcmp(match->path, arg->path)) {
				ret = -ESTALE;
				goto out;
			}
			hide_remove(match);
		}
		match = NULL;
		goto reply;
	}
	if (cmd == SUMH_IOC_USER_HIDE_RETRY) {
		if (!arg->rule_id || !match ||
		    (arg->path_len && strcmp(match->path, arg->path))) {
			ret = -ENOENT;
			goto out;
		}
		if (!sumh_hide_rules_available()) {
			ret = -EOPNOTSUPP;
			goto out;
		}
		goto retry;
	}
	if (cmd != SUMH_IOC_USER_HIDE_UPSERT || arg->rule_id ||
	    arg->path[0] != '/' || !(slash = strrchr(arg->path, '/')) ||
	    !slash[1] || strlen(slash + 1) > NAME_MAX ||
	    !strcmp(slash + 1, ".") || !strcmp(slash + 1, "..")) {
		ret = -EINVAL;
		goto out;
	}
	if (!sumh_hide_rules_available()) {
		ret = -EOPNOTSUPP;
		goto out;
	}
	if (!match) {
		if (hide_next_id == U64_MAX) {
			ret = -EOVERFLOW;
			goto out;
		}
		match = kzalloc(sizeof(*match) + arg->path_len + 1, GFP_KERNEL);
		if (!match) {
			ret = -ENOMEM;
			goto out;
		}
		match->parent = kstrndup(
		    arg->path, slash == arg->path ? 1 : slash - arg->path,
		    GFP_KERNEL);
		if (!match->parent) {
			kfree(match);
			ret = -ENOMEM;
			goto out;
		}
		memcpy(match->path, arg->path, arg->path_len + 1);
		match->name = match->path + (slash - arg->path) + 1;
		match->id = ++hide_next_id;
		refcount_set(&match->refs, 1);
		list_add_tail(&match->list, &hide_rules);
	}
retry:
	match->generation++;
	match->processed = 0;
	hide_event(NULL);
reply:
	hide_snapshot(arg, match);
	if (copy_to_user(user, arg, sizeof(*arg)))
		ret = -EFAULT;
out:
	kfree(arg);
	return ret;
}
