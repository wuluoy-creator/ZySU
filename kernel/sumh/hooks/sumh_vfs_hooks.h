#ifndef _SUMH_VFS_HOOKS_H
#define _SUMH_VFS_HOOKS_H

struct dir_context;
struct file;
struct sumh_filldir_wrapper;

/* NULL means intentional bypass; ERR_PTR preserves a required preparation
 * failure without advancing orig_ctx. A valid result is owned by the caller
 * and must be released with finish after iteration. Never pass ERR_PTR to
 * emit or finish.
 */
struct sumh_filldir_wrapper *
sumh_iterate_prepare_wrapper(struct file *file, struct dir_context *orig_ctx);
void sumh_iterate_emit_injected(struct sumh_filldir_wrapper *wrapper);
void sumh_iterate_rewind_injected(struct dir_context *ctx);
void sumh_iterate_finish_wrapper(struct sumh_filldir_wrapper *wrapper);

#endif
