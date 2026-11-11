#ifndef __KSU_H_KERNEL_UMOUNT
#define __KSU_H_KERNEL_UMOUNT

#include <linux/list.h>
#include <linux/rwsem.h>
#include <linux/rcupdate.h>
#include <linux/types.h>

void ksu_kernel_umount_init(void);
void ksu_kernel_umount_exit(void);
bool ksu_is_webview_zygote_umount_enabled(void);
bool ksu_is_unshare_mnt_enabled(void);

// Handler function to be called from setresuid hook
int ksu_handle_umount(uid_t old_uid, uid_t new_uid);

// for the umount list
struct mount_entry {
	char *umountable;
	unsigned int flags;
	dev_t mount_dev;
	char *mount_root;
	char *mount_fstype;
	struct list_head list;
	struct rcu_head rcu;
};
extern struct list_head mount_list;
extern struct rw_semaphore mount_list_lock;

#endif // #ifndef __KSU_H_KERNEL_UMOUNT
