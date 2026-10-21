#ifndef _SUMH_FOP_OVERRIDE_H
#define _SUMH_FOP_OVERRIDE_H

#include <linux/fs.h>

typedef int (*sumh_fop_iterate_client_fn)(struct file *file,
					  struct dir_context *ctx,
					  const struct file_operations *orig,
					  void *data);

int sumh_fop_override_init(void);
bool sumh_fop_override_available(void);
void sumh_fop_override_stop_new(void);
void sumh_fop_override_exit(void);
/* Retire inode bindings without active iterate clients, without permanently
 * disabling future installs. Retained client bindings keep their inode and
 * superblock references until a later clear or permanent stop retires them.
 * Dynamic fops and their original-owner reference remain allocated until
 * module exit so already-open directory files stay safe. */
void sumh_fop_override_clear(void);

/* Sleepable. The caller must retain a live native path or equivalent inode
 * and superblock protection throughout the call. A newly installed binding
 * owns an inode and active superblock reference, but no mount reference.
 * An unborn or inactive superblock returns -ESHUTDOWN. Bindings remain cached
 * until clear/stop, so their active references can defer filesystem shutdown.
 */
int sumh_fop_install(struct inode *inode);
/* Bind one specialized iterate client to an installed inode shadow.  The data
 * pointer is release-published with @client and is protected by the client SRCU
 * domain while callbacks execute. */
int sumh_fop_bind_iterate_client(struct inode *inode,
				 sumh_fop_iterate_client_fn client, void *data);
/* Withdraw a matching client.  Call synchronize_iterate_clients() before
 * freeing @data or rebinding this inode to different client data. */
void sumh_fop_unbind_iterate_client(struct inode *inode, void *data);
void sumh_fop_synchronize_iterate_clients(void);

#endif /* _SUMH_FOP_OVERRIDE_H */
