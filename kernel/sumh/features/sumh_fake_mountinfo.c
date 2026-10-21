#include "sumh_fake_mountinfo.h"
#include "sumh_entrypoints.h"
#include "sumh_path_policy.h"
#include "sumh_proc_memory.h"
#include "sumh_runtime.h"
#ifdef SUMH_EMBEDDED
#include "infra/mount_policy.h"
#endif

#include <linux/mnt_namespace.h>
#include <linux/nsproxy.h>
#include <linux/overflow.h>
#include <linux/pid.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/sort.h>
#include <linux/uio.h>
#include <linux/vmalloc.h>

static bool fake_mi_initialized;
static atomic64_t fake_mi_view_gen = ATOMIC64_INIT(1);
static DECLARE_WAIT_QUEUE_HEAD(fake_mi_view_wait);

typedef int (*sumh_mi_show_fn)(struct seq_file *, struct vfsmount *);
static sumh_mi_show_fn sumh_mi_mountinfo_raw;
static sumh_mi_show_fn sumh_mi_mounts_raw;

/* This prefix is shared by proc_mounts across supported kernels. */
struct sumh_proc_mounts_prefix {
	struct mnt_namespace *ns;
	struct path root;
	sumh_mi_show_fn show;
};

bool sumh_fake_mi_native_view(struct file *file)
{
	struct seq_file *seq = file ? file->private_data : NULL;
	struct sumh_proc_mounts_prefix *pm = seq ? seq->private : NULL;
	struct task_struct *init;
	struct pid *pid;
	bool native_view = false;

	if (!current->nsproxy ||
	    (file && (!pm || pm->ns != current->nsproxy->mnt_ns)))
		return false;
	pid = find_get_pid(1);
	if (!pid)
		return false;
	init = get_pid_task(pid, PIDTYPE_PID);
	put_pid(pid);
	if (!init)
		return false;
	task_lock(init);
	native_view =
	    init->nsproxy && init->nsproxy->mnt_ns == current->nsproxy->mnt_ns;
	task_unlock(init);
	put_task_struct(init);
	return native_view;
}

/* Module-owned callbacks provide typed entry points for indirect calls. */
static SUMH_NOCFI int sumh_mi_show_mountinfo(struct seq_file *seq,
					     struct vfsmount *mnt)
{
	return sumh_mi_mountinfo_raw(seq, mnt);
}

static SUMH_NOCFI int sumh_mi_show_mounts(struct seq_file *seq,
					  struct vfsmount *mnt)
{
	return sumh_mi_mounts_raw(seq, mnt);
}

#define SUMH_MI_INITIAL_SIZE 65536
#define SUMH_MI_MAX_SIZE (1024 * 1024)

static DEFINE_MUTEX(sumh_mi_snapshot_lock);

static char *sumh_mi_alloc_buffer(size_t capacity)
{
	char *buffer;

	if (!sumh_proc_memory_reserve(SUMH_PROC_MEMORY_SNAPSHOT, capacity))
		return NULL;
	buffer = kvmalloc(capacity, GFP_KERNEL);
	if (!buffer) {
		sumh_proc_memory_alloc_failed(SUMH_PROC_MEMORY_SNAPSHOT);
		sumh_proc_memory_release(SUMH_PROC_MEMORY_SNAPSHOT, capacity);
	}
	return buffer;
}

static void sumh_mi_free_buffer(char *buffer, size_t capacity)
{
	if (!buffer)
		return;
	kvfree(buffer);
	sumh_proc_memory_release(SUMH_PROC_MEMORY_SNAPSHOT, capacity);
}

void sumh_fake_mi_put_snapshot(struct sumh_mi_snapshot *snapshot)
{
	if (!snapshot || !refcount_dec_and_test(&snapshot->refs))
		return;
	sumh_mi_free_buffer(snapshot->data, snapshot->data_capacity);
	sumh_mi_free_buffer(snapshot->mounts, snapshot->mounts_capacity);
	kfree(snapshot);
	sumh_proc_memory_release(SUMH_PROC_MEMORY_SNAPSHOT, sizeof(*snapshot));
}

static SUMH_NOCFI int sumh_mi_read_snapshot(struct file *file,
					    const struct file_operations *ops,
					    char **buffer, size_t *length,
					    size_t *capacity)
{
	loff_t pos = 0;

	(*length) = 0;
	for (;;) {
		struct kiocb iocb;
		struct iov_iter iter;
		struct kvec vec;
		char overflow;
		ssize_t ret;

		if ((*length) == *capacity && *capacity < SUMH_MI_MAX_SIZE) {
			size_t size =
			    min_t(size_t, *capacity * 2, SUMH_MI_MAX_SIZE);
			char *data = sumh_mi_alloc_buffer(size);

			if (!data)
				return -ENOMEM;
			memcpy(data, (*buffer), (*length));
			sumh_mi_free_buffer(*buffer, *capacity);
			(*buffer) = data;
			*capacity = size;
		}
		vec.iov_base =
		    (*length) == *capacity ? &overflow : (*buffer) + (*length);
		vec.iov_len =
		    (*length) == *capacity ? 1 : *capacity - (*length);
		iov_iter_kvec(&iter, READ, &vec, 1, vec.iov_len);
		init_sync_kiocb(&iocb, file);
		iocb.ki_pos = pos;
		ret = ops->read_iter ? ops->read_iter(&iocb, &iter)
				     : seq_read_iter(&iocb, &iter);
		if (ret < 0)
			return ret;
		if (!ret)
			return (*length) ? 0 : -EIO;
		if ((*length) == *capacity)
			return -EFBIG;
		if (ret > vec.iov_len || iocb.ki_pos <= pos)
			return -EIO;
		(*length) += ret;
		pos = iocb.ki_pos;
	}
}

struct sumh_mi_prop_ref {
	size_t start;
	size_t end;
	u32 id;
};

static int sumh_mi_collect_props(const char *data, size_t len,
				 struct sumh_mi_prop_ref *refs, size_t *count)
{
	size_t line = 0, found = 0;

	while (line < len) {
		const char *newline = memchr(data + line, '\n', len - line);
		size_t end = newline ? (size_t)(newline - data) : len;
		size_t cursor = line;
		unsigned int field;
		bool separator = false;

		for (field = 0; field < 6; field++) {
			const char *space =
			    memchr(data + cursor, ' ', end - cursor);

			if (!space || space == data + cursor)
				return -EINVAL;
			cursor = space - data + 1;
		}
		while (cursor < end) {
			const char *space =
			    memchr(data + cursor, ' ', end - cursor);
			size_t token_end = space ? (size_t)(space - data) : end;
			size_t token_len = token_end - cursor;
			size_t prefix = 0, i;
			u32 id = 0;

			if (!token_len)
				return -EINVAL;
			if (token_len == 1 && data[cursor] == '-') {
				separator = true;
				break;
			}
			if (token_len >= 7 &&
			    !memcmp(data + cursor, "shared:", 7))
				prefix = 7;
			else if (token_len >= 7 &&
				 !memcmp(data + cursor, "master:", 7))
				prefix = 7;
			else if (token_len >= 15 &&
				 !memcmp(data + cursor, "propagate_from:", 15))
				prefix = 15;
			if (prefix) {
				if (prefix == token_len)
					return -EINVAL;
				for (i = cursor + prefix; i < token_end; i++) {
					u32 digit = data[i] - '0';

					if (digit > 9 ||
					    id > (~0U - digit) / 10)
						return -EINVAL;
					id = id * 10 + digit;
				}
				if (!id)
					return -EINVAL;
				if (refs) {
					if (found >= *count)
						return -ENOSPC;
					refs[found] = (struct sumh_mi_prop_ref){
					    .start = cursor + prefix,
					    .end = token_end,
					    .id = id};
				}
				found++;
			}
			cursor = token_end + 1;
		}
		if (!separator)
			return -EINVAL;
		line = end + 1;
	}
	*count = found;
	return 0;
}

static int sumh_mi_compare_ids(const void *a, const void *b)
{
	u32 left = *(const u32 *)a, right = *(const u32 *)b;

	return (left > right) - (left < right);
}

/* Only propagation numbers change; sorted positive IDs never grow in width. */
int sumh_mi_normalize_groups(char *data, size_t *len)
{
	struct sumh_mi_prop_ref *refs = NULL;
	u32 *ids = NULL;
	size_t count = 0, unique = 0, i, input = 0, output = 0;
	size_t refs_bytes, ids_bytes;
	int ret;

	ret = sumh_mi_collect_props(data, *len, NULL, &count);
	if (ret || !count)
		return ret;
	if (check_mul_overflow(count, sizeof(*refs), &refs_bytes) ||
	    check_mul_overflow(count, sizeof(*ids), &ids_bytes))
		return -EOVERFLOW;
	if (!sumh_proc_memory_reserve(SUMH_PROC_MEMORY_WORKSPACE, refs_bytes))
		return -ENOMEM;
	refs = kvmalloc_array(count, sizeof(*refs), GFP_KERNEL);
	if (!refs) {
		sumh_proc_memory_alloc_failed(SUMH_PROC_MEMORY_WORKSPACE);
		sumh_proc_memory_release(SUMH_PROC_MEMORY_WORKSPACE,
					 refs_bytes);
		return -ENOMEM;
	}
	if (!sumh_proc_memory_reserve(SUMH_PROC_MEMORY_WORKSPACE, ids_bytes)) {
		ret = -ENOMEM;
		goto out;
	}
	ids = kvmalloc_array(count, sizeof(*ids), GFP_KERNEL);
	if (!ids) {
		sumh_proc_memory_alloc_failed(SUMH_PROC_MEMORY_WORKSPACE);
		sumh_proc_memory_release(SUMH_PROC_MEMORY_WORKSPACE, ids_bytes);
		ret = -ENOMEM;
		goto out;
	}
	ret = sumh_mi_collect_props(data, *len, refs, &count);
	if (ret)
		goto out;
	for (i = 0; i < count; i++)
		ids[i] = refs[i].id;
	sort(ids, count, sizeof(*ids), sumh_mi_compare_ids, NULL);
	for (i = 0; i < count; i++)
		if (!unique || ids[i] != ids[unique - 1])
			ids[unique++] = ids[i];
	for (i = 0; i < count; i++) {
		size_t low = 0, high = unique, segment;
		char number[16];
		int width;

		while (low < high) {
			size_t mid = low + (high - low) / 2;

			if (ids[mid] < refs[i].id)
				low = mid + 1;
			else
				high = mid;
		}
		if (low == unique || ids[low] != refs[i].id) {
			ret = -EINVAL;
			goto out;
		}
		width = scnprintf(number, sizeof(number), "%u", (u32)low + 1);
		if (width > refs[i].end - refs[i].start) {
			ret = -EOVERFLOW;
			goto out;
		}
		segment = refs[i].start - input;
		memmove(data + output, data + input, segment);
		output += segment;
		memcpy(data + output, number, width);
		output += width;
		input = refs[i].end;
	}
	memmove(data + output, data + input, *len - input);
	*len = output + *len - input;
out:
	if (ids) {
		kvfree(ids);
		sumh_proc_memory_release(SUMH_PROC_MEMORY_WORKSPACE, ids_bytes);
	}
	kvfree(refs);
	sumh_proc_memory_release(SUMH_PROC_MEMORY_WORKSPACE, refs_bytes);
	return ret;
}

static bool sumh_mi_line_target(const char *line, size_t len,
				unsigned int field, const char **target,
				size_t *target_len)
{
	size_t start = 0;
	unsigned int i;

	for (i = 0; i <= field; i++) {
		const char *space = memchr(line + start, ' ', len - start);
		size_t end = space ? (size_t)(space - line) : len;

		if (start == end)
			return false;
		if (i == field) {
			*target = line + start;
			*target_len = end - start;
			return true;
		}
		if (!space)
			return false;
		start = end + 1;
	}
	return false;
}

static bool sumh_mi_pair_matches(const struct sumh_mi_snapshot *snapshot)
{
	size_t mi = 0, mo = 0;

	while (mi < snapshot->len && mo < snapshot->mounts_len) {
		const char *mi_end =
		    memchr(snapshot->data + mi, '\n', snapshot->len - mi);
		const char *mo_end = memchr(snapshot->mounts + mo, '\n',
					    snapshot->mounts_len - mo);
		size_t mi_len = mi_end ? (size_t)(mi_end - snapshot->data) - mi
				       : snapshot->len - mi;
		size_t mo_len = mo_end
				    ? (size_t)(mo_end - snapshot->mounts) - mo
				    : snapshot->mounts_len - mo;
		const char *mi_target, *mo_target;
		size_t mi_target_len, mo_target_len;

		if (!sumh_mi_line_target(snapshot->data + mi, mi_len, 4,
					 &mi_target, &mi_target_len) ||
		    !sumh_mi_line_target(snapshot->mounts + mo, mo_len, 1,
					 &mo_target, &mo_target_len) ||
		    mi_target_len != mo_target_len ||
		    memcmp(mi_target, mo_target, mi_target_len))
			return false;
		mi += mi_len + !!mi_end;
		mo += mo_len + !!mo_end;
	}
	return mi == snapshot->len && mo == snapshot->mounts_len;
}

struct sumh_mi_row {
	u32 id;
	u32 parent;
	u8 state;
};

enum {
	SUMH_MI_UNVISITED,
	SUMH_MI_VISITING,
	SUMH_MI_VISIBLE,
	SUMH_MI_HIDDEN,
};

static size_t sumh_mi_line_size(const char *data, size_t len)
{
	const char *end = memchr(data, '\n', len);

	return end ? (size_t)(end - data) + 1 : len;
}

static int sumh_mi_line_id(const char *line, size_t len, unsigned int field,
			   u32 *id)
{
	const char *value;
	size_t size;
	char number[12];

	if (!sumh_mi_line_target(line, len, field, &value, &size) || !size ||
	    size >= sizeof(number) || value[0] < '0' || value[0] > '9')
		return -EINVAL;
	memcpy(number, value, size);
	number[size] = '\0';
	if (kstrtou32(number, 10, id) || !*id)
		return -EINVAL;
	return 0;
}

#ifndef SUMH_EMBEDDED
static bool sumh_mi_field_matches(const char *data, size_t len,
				  const char *text, bool path)
{
	size_t pos = 0;

	while (*text) {
		unsigned char c;

		if (pos == len)
			return false;
		c = data[pos++];
		if (c == '\\' && pos + 2 < len && data[pos] >= '0' &&
		    data[pos] <= '3' && data[pos + 1] >= '0' &&
		    data[pos + 1] <= '7' && data[pos + 2] >= '0' &&
		    data[pos + 2] <= '7') {
			c = ((data[pos] - '0') << 6) |
			    ((data[pos + 1] - '0') << 3) |
			    (data[pos + 2] - '0');
			pos += 3;
		}
		if (c != (unsigned char)*text++)
			return false;
	}
	return pos == len || (path && data[pos] == '/');
}

static bool sumh_mi_private_path(const char *data, size_t len)
{
	return sumh_mi_field_matches(data, len, "/adb", true) ||
	       sumh_mi_field_matches(data, len, "/data/adb", true);
}
#endif

static int sumh_mi_module_line(const char *line, size_t len, bool *hidden)
{
	const char *values[6], *separator;
	size_t sizes[6], after_len;
	unsigned int i;
	static const unsigned int fields[] = {2, 3, 4, 0, 1, 2};

	separator = strnstr(line, " - ", len);
	if (!separator)
		return -EINVAL;
	after_len = len - (separator + 3 - line);
	for (i = 0; i < ARRAY_SIZE(fields); i++) {
		if (!sumh_mi_line_target(i < 3 ? line : separator + 3,
					 i < 3 ? separator - line : after_len,
					 fields[i], &values[i], &sizes[i]))
			return -EINVAL;
	}
#ifdef SUMH_EMBEDDED
	{
		struct ksu_mount_fields mount = {
		    .dev = {values[0], sizes[0]},
		    .root = {values[1], sizes[1]},
		    .target = {values[2], sizes[2]},
		    .fstype = {values[3], sizes[3]},
		    .source = {values[4], sizes[4]},
		    .super = {values[5], sizes[5]},
		    .escaped = true,
		};

		*hidden = ksu_mount_is_module(&mount);
	}
#else
	*hidden = sumh_mi_private_path(values[1], sizes[1]) ||
		  sumh_mi_private_path(values[2], sizes[2]) ||
		  sumh_mi_private_path(values[4], sizes[4]) ||
		  sumh_mi_field_matches(values[4], sizes[4], "KSU", false) ||
		  sumh_mi_field_matches(values[4], sizes[4], "magisk", false) ||
		  sumh_mi_field_matches(values[4], sizes[4], "APatch", false);
	for (i = 0; !*hidden && i < sizes[5]; i++) {
		size_t end;

		if (values[5][i] != '/' ||
		    (i && values[5][i - 1] != '=' && values[5][i - 1] != ':' &&
		     values[5][i - 1] != ','))
			continue;
		for (end = i; end < sizes[5] && values[5][end] != ',' &&
			      values[5][end] != ':';
		     end++)
			;
		*hidden = sumh_mi_private_path(values[5] + i, end - i);
	}
#endif
	return 0;
}

static int sumh_mi_compare_rows(const void *a, const void *b)
{
	const struct sumh_mi_row *left = a, *right = b;

	return (left->id > right->id) - (left->id < right->id);
}

static struct sumh_mi_row *sumh_mi_find_row(struct sumh_mi_row *rows,
					    size_t count, u32 id)
{
	size_t low = 0, high = count;

	while (low < high) {
		size_t mid = low + (high - low) / 2;

		if (rows[mid].id < id)
			low = mid + 1;
		else
			high = mid;
	}
	return low < count && rows[low].id == id ? rows + low : NULL;
}

/* Compact both files by ordinal; mount IDs and propagation stay native. */
static int sumh_mi_filter_native(struct sumh_mi_snapshot *snapshot)
{
	struct sumh_mi_row *rows, *row;
	size_t count = 0, mi = 0, mo = 0, mi_out = 0, mo_out = 0, i;
	size_t rows_bytes;
	int ret = -EINVAL;

	if (!sumh_mi_pair_matches(snapshot))
		return -EINVAL;
	while (mi < snapshot->len) {
		mi +=
		    sumh_mi_line_size(snapshot->data + mi, snapshot->len - mi);
		count++;
	}
	if (check_mul_overflow(count, sizeof(*rows), &rows_bytes))
		return -EOVERFLOW;
	if (!sumh_proc_memory_reserve(SUMH_PROC_MEMORY_WORKSPACE, rows_bytes))
		return -ENOMEM;
	rows = kvcalloc(count, sizeof(*rows), GFP_KERNEL);
	if (!rows) {
		sumh_proc_memory_alloc_failed(SUMH_PROC_MEMORY_WORKSPACE);
		sumh_proc_memory_release(SUMH_PROC_MEMORY_WORKSPACE,
					 rows_bytes);
		return -ENOMEM;
	}
	for (i = 0, mi = 0; i < count; i++) {
		const char *line = snapshot->data + mi;
		size_t size = sumh_mi_line_size(line, snapshot->len - mi);
		size_t len = size - (line[size - 1] == '\n');
		bool hidden;

		if (sumh_mi_line_id(line, len, 0, &rows[i].id) ||
		    sumh_mi_line_id(line, len, 1, &rows[i].parent) ||
		    sumh_mi_module_line(line, len, &hidden))
			goto out;
		rows[i].state = hidden ? SUMH_MI_HIDDEN : SUMH_MI_UNVISITED;
		mi += size;
	}
	sort(rows, count, sizeof(*rows), sumh_mi_compare_rows, NULL);
	for (i = 1; i < count; i++)
		if (rows[i - 1].id == rows[i].id)
			goto out;
	for (i = 0; i < count; i++) {
		u8 state;

		row = rows + i;
		while (row && row->state == SUMH_MI_UNVISITED) {
			row->state = SUMH_MI_VISITING;
			row = row->id == row->parent
				  ? NULL
				  : sumh_mi_find_row(rows, count, row->parent);
		}
		if (row && row->state == SUMH_MI_VISITING)
			goto out;
		state = row ? row->state : SUMH_MI_VISIBLE;
		row = rows + i;
		while (row && row->state == SUMH_MI_VISITING) {
			row->state = state;
			row = sumh_mi_find_row(rows, count, row->parent);
		}
	}
	for (mi = 0; mi < snapshot->len;) {
		size_t mi_size =
		    sumh_mi_line_size(snapshot->data + mi, snapshot->len - mi);
		size_t mo_size = sumh_mi_line_size(snapshot->mounts + mo,
						   snapshot->mounts_len - mo);
		u32 id;

		if (sumh_mi_line_id(snapshot->data + mi, mi_size, 0, &id))
			goto out;
		row = sumh_mi_find_row(rows, count, id);
		if (!row)
			goto out;
		if (row->state == SUMH_MI_VISIBLE) {
			memmove(snapshot->data + mi_out, snapshot->data + mi,
				mi_size);
			memmove(snapshot->mounts + mo_out,
				snapshot->mounts + mo, mo_size);
			mi_out += mi_size;
			mo_out += mo_size;
		}
		mi += mi_size;
		mo += mo_size;
	}
	if (mi_out && mo_out) {
		snapshot->len = mi_out;
		snapshot->mounts_len = mo_out;
		ret = 0;
	}
out:
	kvfree(rows);
	sumh_proc_memory_release(SUMH_PROC_MEMORY_WORKSPACE, rows_bytes);
	return ret;
}

static SUMH_NOCFI int sumh_mi_render(struct file *file,
				     const struct file_operations *ops,
				     sumh_mi_show_fn show, char **data,
				     size_t *len, size_t *capacity)
{
	struct seq_file *seq = file->private_data;
	struct sumh_proc_mounts_prefix *pm = seq->private;
	int ret;

	WRITE_ONCE(pm->show, show);
	ret = ops->llseek(file, 0, SEEK_SET);
	if (ret < 0)
		return ret;
	return sumh_mi_read_snapshot(file, ops, data, len, capacity);
}

int SUMH_NOCFI sumh_fake_mi_get_snapshot(struct file *file,
					 const struct file_operations *ops,
					 struct sumh_mi_snapshot **out)
{
	struct sumh_mi_snapshot *snapshot = NULL;
	struct seq_file *seq = file->private_data;
	struct sumh_proc_mounts_prefix *pm = NULL;
	sumh_mi_show_fn original_show = NULL;
	int attempt, ret = 0;

	*out = NULL;
	mutex_lock(&sumh_mi_snapshot_lock);
	if (!ops->llseek) {
		ret = -EOPNOTSUPP;
		goto unlock;
	}
	pm = seq ? seq->private : NULL;
	if (!pm || !pm->ns || !pm->root.mnt || !pm->root.dentry || !pm->show) {
		ret = -EINVAL;
		goto unlock;
	}
	original_show = READ_ONCE(pm->show);
	if (!sumh_proc_memory_reserve(SUMH_PROC_MEMORY_SNAPSHOT,
				      sizeof(*snapshot))) {
		ret = -ENOMEM;
		goto unlock;
	}
	snapshot = kzalloc(sizeof(*snapshot), GFP_KERNEL);
	if (!snapshot) {
		sumh_proc_memory_alloc_failed(SUMH_PROC_MEMORY_SNAPSHOT);
		sumh_proc_memory_release(SUMH_PROC_MEMORY_SNAPSHOT,
					 sizeof(*snapshot));
		ret = -ENOMEM;
		goto unlock;
	}
	refcount_set(&snapshot->refs, 1);
	snapshot->data = sumh_mi_alloc_buffer(SUMH_MI_INITIAL_SIZE);
	if (!snapshot->data) {
		ret = -ENOMEM;
		goto unlock;
	}
	snapshot->data_capacity = SUMH_MI_INITIAL_SIZE;
	snapshot->mounts = sumh_mi_alloc_buffer(SUMH_MI_INITIAL_SIZE);
	if (!snapshot->mounts) {
		ret = -ENOMEM;
		goto unlock;
	}
	snapshot->mounts_capacity = SUMH_MI_INITIAL_SIZE;
	for (attempt = 0; attempt < 3; attempt++) {
		unsigned long event;
#ifdef SUMH_EMBEDDED
		u64 policy = ksu_mount_policy_generation();

		if (policy & 1)
			continue;
#endif

		if (ops->poll)
			(void)ops->poll(file, NULL);
		event = READ_ONCE(seq->poll_event);
		ret = sumh_mi_render(file, ops, sumh_mi_show_mountinfo,
				     &snapshot->data, &snapshot->len,
				     &snapshot->data_capacity);
		if (ret)
			goto unlock;
		ret = sumh_mi_render(file, ops, sumh_mi_show_mounts,
				     &snapshot->mounts, &snapshot->mounts_len,
				     &snapshot->mounts_capacity);
		if (ret)
			goto unlock;
		if (ops->poll)
			(void)ops->poll(file, NULL);
		if (event != READ_ONCE(seq->poll_event) ||
		    !sumh_mi_pair_matches(snapshot))
			continue;
		/* Preserve this opened namespace's mount identities so
		 * statx/fdinfo anchors can resolve against its filtered table.
		 */
		ret = sumh_mi_filter_native(snapshot);
		if (ret)
			goto unlock;
		if (ops->poll)
			(void)ops->poll(file, NULL);
		if (event != READ_ONCE(seq->poll_event))
			continue;
#ifdef SUMH_EMBEDDED
		if (policy != ksu_mount_policy_generation())
			continue;
#endif
		break;
	}
	if (attempt == 3) {
		ret = -EAGAIN;
		goto unlock;
	}
	ret = sumh_mi_normalize_groups(snapshot->data, &snapshot->len);
	if (ret)
		goto unlock;
	*out = snapshot;
	snapshot = NULL;
unlock:
	if (original_show)
		WRITE_ONCE(pm->show, original_show);
	mutex_unlock(&sumh_mi_snapshot_lock);
	sumh_fake_mi_put_snapshot(snapshot);
	return ret;
}

void sumh_fake_mi_invalidate_all(void)
{
	atomic64_inc(&fake_mi_view_gen);
	wake_up_all(&fake_mi_view_wait);
}

u64 sumh_fake_mi_generation(void)
{
	return (u64)atomic64_read(&fake_mi_view_gen);
}

void sumh_fake_mi_poll_wait(struct file *file, struct poll_table_struct *wait)
{
	poll_wait(file, &fake_mi_view_wait, wait);
}

int sumh_fake_mi_init(void)
{
	sumh_mi_mountinfo_raw = (void *)sumh_lookup_name("show_mountinfo");
	sumh_mi_mounts_raw = (void *)sumh_lookup_name("show_vfsmnt");
	if (!sumh_mi_mountinfo_raw || !sumh_mi_mounts_raw)
		return -ENOSYS;
	atomic64_set(&fake_mi_view_gen, 1);
	WRITE_ONCE(fake_mi_initialized, true);
	return 0;
}

void sumh_fake_mi_exit(void)
{
	mutex_lock(&sumh_mi_snapshot_lock);
	WRITE_ONCE(fake_mi_initialized, false);
	mutex_unlock(&sumh_mi_snapshot_lock);
	sumh_fake_mi_invalidate_all();
}

bool sumh_fake_mi_active(void)
{
	return READ_ONCE(fake_mi_initialized);
}
