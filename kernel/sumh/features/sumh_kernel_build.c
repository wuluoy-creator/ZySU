#include <linux/build_bug.h>
#include <linux/errno.h>
#include <linux/rwsem.h>
#include <linux/string.h>
#include <linux/utsname.h>

#include "infra/symbol_resolver.h"
#include "sumh_kernel_build.h"

/* Resolve data symbols instead of requiring additional GKI module exports.
 * Use the target kernel's uts_namespace layout, never a private KMI overlay.
 * SUMH's control/lifecycle locks serialize all callers; uts_sem pairs with
 * the kernel's uname, sysctl and UTS namespace cloning paths.
 */
static struct uts_namespace *sumh_build_uts;
static struct rw_semaphore *sumh_build_sem;
static char sumh_build_original_release[SUMH_KERNEL_BUILD_MAX];
static char sumh_build_original_version[SUMH_KERNEL_BUILD_MAX];
static bool sumh_build_enabled;

static int sumh_kernel_build_validate_string(const char *value, bool required)
{
	size_t length = strnlen(value, SUMH_KERNEL_BUILD_MAX);
	size_t i;

	if (length == SUMH_KERNEL_BUILD_MAX)
		return -ENAMETOOLONG;
	if (required && !length)
		return -EINVAL;
	for (i = 0; i < length; i++) {
		unsigned char c = value[i];

		if (c < 0x20 || c == 0x7f)
			return -EINVAL;
	}
	/* Require one canonical C string, not a valid prefix hiding bad input.
	 */
	for (i = length + 1; i < SUMH_KERNEL_BUILD_MAX; i++)
		if (value[i])
			return -EINVAL;
	return 0;
}

int sumh_kernel_build_validate(const struct sumh_kernel_build_arg *arg)
{
	int ret;

	if (arg->size != sizeof(*arg) || arg->enable > 1 || arg->reserved[0] ||
	    arg->reserved[1] || arg->reserved_tail[0] || arg->reserved_tail[1])
		return -EINVAL;
	ret = sumh_kernel_build_validate_string(arg->release, arg->enable);
	if (ret)
		return ret;
	return sumh_kernel_build_validate_string(arg->version, arg->enable);
}

bool sumh_kernel_build_available(void)
{
	return sumh_build_uts && sumh_build_sem;
}

bool sumh_kernel_build_enabled(void)
{
	return READ_ONCE(sumh_build_enabled);
}

void sumh_kernel_build_init(void)
{
	struct uts_namespace *ns;
	struct rw_semaphore *sem;
	bool valid;

	BUILD_BUG_ON(sizeof(ns->name.release) != SUMH_KERNEL_BUILD_MAX);
	BUILD_BUG_ON(sizeof(ns->name.version) != SUMH_KERNEL_BUILD_MAX);
	BUILD_BUG_ON(sizeof(struct sumh_kernel_build_arg) != 148);
	if (sumh_kernel_build_available())
		return;
	ns = (void *)find_kernel_symbol_exact("init_uts_ns");
	sem = (void *)find_kernel_symbol_exact("uts_sem");
	if (!ns || !sem) {
		pr_info("sumh: kernel build spoof unavailable: UTS symbols "
			"missing\n");
		return;
	}
	down_read(sem);
	valid = !memcmp(ns->name.sysname, "Linux", sizeof("Linux")) &&
		strnlen(ns->name.release, SUMH_KERNEL_BUILD_MAX) > 0 &&
		strnlen(ns->name.release, SUMH_KERNEL_BUILD_MAX) <
		    SUMH_KERNEL_BUILD_MAX &&
		strnlen(ns->name.version, SUMH_KERNEL_BUILD_MAX) > 0 &&
		strnlen(ns->name.version, SUMH_KERNEL_BUILD_MAX) <
		    SUMH_KERNEL_BUILD_MAX;
	up_read(sem);
	if (!valid) {
		pr_warn("sumh: kernel build spoof unavailable: invalid UTS "
			"identity\n");
		return;
	}
	sumh_build_uts = ns;
	sumh_build_sem = sem;
}

int sumh_kernel_build_set(const struct sumh_kernel_build_arg *arg)
{
	struct new_utsname *name;
	int ret = sumh_kernel_build_validate(arg);

	if (ret)
		return ret;
	if (!arg->enable) {
		sumh_kernel_build_clear();
		return 0;
	}
	if (!sumh_kernel_build_available())
		return -EOPNOTSUPP;

	name = &sumh_build_uts->name;
	down_write(sumh_build_sem);
	if (!sumh_build_enabled) {
		memcpy(sumh_build_original_release, name->release,
		       sizeof(name->release));
		memcpy(sumh_build_original_version, name->version,
		       sizeof(name->version));
	}
	memcpy(name->release, arg->release, sizeof(name->release));
	memcpy(name->version, arg->version, sizeof(name->version));
	WRITE_ONCE(sumh_build_enabled, true);
	up_write(sumh_build_sem);
	return 0;
}

static int sumh_kernel_build_read(struct sumh_kernel_build_arg *arg,
				  bool original)
{
	if (!sumh_kernel_build_available())
		return -EOPNOTSUPP;
	memset(arg, 0, sizeof(*arg));
	arg->size = sizeof(*arg);
	down_read(sumh_build_sem);
	arg->enable = sumh_build_enabled;
	strscpy(arg->release,
		original && sumh_build_enabled ? sumh_build_original_release
					       : sumh_build_uts->name.release,
		sizeof(arg->release));
	strscpy(arg->version,
		original && sumh_build_enabled ? sumh_build_original_version
					       : sumh_build_uts->name.version,
		sizeof(arg->version));
	up_read(sumh_build_sem);
	return 0;
}

int sumh_kernel_build_get(struct sumh_kernel_build_arg *arg)
{
	return sumh_kernel_build_read(arg, false);
}

int sumh_kernel_build_get_original(struct sumh_kernel_build_arg *arg)
{
	return sumh_kernel_build_read(arg, true);
}

void sumh_kernel_build_clear(void)
{
	struct new_utsname *name;

	if (!sumh_kernel_build_available() || !sumh_build_enabled)
		return;
	name = &sumh_build_uts->name;
	down_write(sumh_build_sem);
	memcpy(name->release, sumh_build_original_release,
	       sizeof(name->release));
	memcpy(name->version, sumh_build_original_version,
	       sizeof(name->version));
	WRITE_ONCE(sumh_build_enabled, false);
	up_write(sumh_build_sem);
}

void sumh_kernel_build_exit(void)
{
	sumh_kernel_build_clear();
	sumh_build_uts = NULL;
	sumh_build_sem = NULL;
}
