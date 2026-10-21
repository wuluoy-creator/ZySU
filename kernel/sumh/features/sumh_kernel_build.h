#ifndef _SUMH_KERNEL_BUILD_H
#define _SUMH_KERNEL_BUILD_H

#include "sumh_base.h"

void sumh_kernel_build_init(void);
void sumh_kernel_build_exit(void);
bool sumh_kernel_build_available(void);
bool sumh_kernel_build_enabled(void);
int sumh_kernel_build_validate(const struct sumh_kernel_build_arg *arg);
int sumh_kernel_build_set(const struct sumh_kernel_build_arg *arg);
int sumh_kernel_build_get(struct sumh_kernel_build_arg *arg);
int sumh_kernel_build_get_original(struct sumh_kernel_build_arg *arg);
void sumh_kernel_build_clear(void);

#endif /* _SUMH_KERNEL_BUILD_H */
