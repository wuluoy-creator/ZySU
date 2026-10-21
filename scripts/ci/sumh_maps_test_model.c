/* Host kernel-service doubles. Included around the actual production store. */
typedef unsigned int __u32;
#define __aligned_u64 unsigned long long __attribute__((aligned(8)))
#define __user
#define EINVAL 22
#define ENOMEM 12
#define EFAULT 14
#define ENOENT 2
#define ENOSPC 28
#define EOVERFLOW 75
#define EOPNOTSUPP 95
#define GFP_KERNEL 0
#define READ_ONCE(v) (v)
#define WRITE_ONCE(v, n) ((v) = (n))
#ifndef SUMH_IOC_CLEAR_MAPS_RULES
#define SUMH_IOC_CLEAR_MAPS_RULES 24
#endif
#ifndef SUMH_MAPS_CONTROL_CHECK
#define SUMH_MAPS_CONTROL_CHECK() ((void)0)
#endif
#define u64_to_user_ptr(value) ((void *)(__UINTPTR_TYPE__)(value))
#define ARRAY_SIZE(v) (sizeof(v) / sizeof((v)[0]))
#define min(a, b) ((a) < (b) ? (a) : (b))
#define check_add_overflow(a, b, out) __builtin_add_overflow((a), (b), (out))
#define check_mul_overflow(a, b, out) __builtin_mul_overflow((a), (b), (out))
#define struct_size(p, member, n) (sizeof(*(p)) + sizeof((p)->member[0]) * (n))
static int model_bad, allocation_calls, allocation_fail_at, allocation_live;
static size_t physical_bytes, physical_peak;
static unsigned long long
    allocation_blocks[64][524288 / sizeof(unsigned long long)];
static size_t allocation_sizes[64];
static int input_copies, input_fail_at, output_copies, reply_fault,
    proxy_available = 1;
static struct sumh_proc_memory_stats stats(void) {
  struct sumh_proc_memory_stats s;
  size_t total = 0;
  sumh_proc_memory_snapshot(&s);
  for (unsigned int i = 0; i < SUMH_PROC_MEMORY_KINDS; i++)
    total += s.by_kind[i];
  if (total != s.used || s.used > SUMH_PROC_MEMORY_BUDGET ||
      physical_bytes > s.used)
    model_bad++;
  return s;
}
static void *tracked_allocate(size_t n, bool zero) {
  SUMH_MAPS_CONTROL_CHECK();
  if (memory_lock_depth)
    model_bad++;
  struct sumh_proc_memory_stats s = stats();
  if (n > s.used || physical_bytes > s.used - n)
    model_bad++;
  if (++allocation_calls == allocation_fail_at)
    return NULL;
  for (unsigned int i = 0; i < ARRAY_SIZE(allocation_blocks); i++)
    if (!allocation_sizes[i] && n && n <= sizeof(allocation_blocks[i])) {
      allocation_sizes[i] = n;
      allocation_live++;
      physical_bytes += n;
      if (physical_peak < physical_bytes)
        physical_peak = physical_bytes;
      memset(allocation_blocks[i], zero ? 0 : 0xa5, n);
      return allocation_blocks[i];
    }
  model_bad++;
  return NULL;
}
static void tracked_free(void *p) {
  if (!p)
    return;
  SUMH_MAPS_CONTROL_CHECK();
  if (memory_lock_depth)
    model_bad++;
  (void)stats();
  for (unsigned int i = 0; i < ARRAY_SIZE(allocation_blocks); i++)
    if (p == allocation_blocks[i] && allocation_sizes[i]) {
      memset(p, 0xdd, allocation_sizes[i]);
      physical_bytes -= allocation_sizes[i];
      allocation_sizes[i] = 0;
      allocation_live--;
      return;
    }
  model_bad++;
}
static void *kvmalloc(size_t n, int flags) {
  (void)flags;
  return tracked_allocate(n, false);
}
static void *kvzalloc(size_t n, int flags) {
  (void)flags;
  return tracked_allocate(n, true);
}
static void *kvmalloc_array(size_t n, size_t size, int flags) {
  size_t bytes;
  (void)flags;
  if (check_mul_overflow(n, size, &bytes))
    return NULL;
  return tracked_allocate(bytes, false);
}
static void *kvcalloc(size_t n, size_t size, int flags) {
  size_t bytes;
  (void)flags;
  if (check_mul_overflow(n, size, &bytes))
    return NULL;
  return tracked_allocate(bytes, true);
}
static void kvfree(void *p) { tracked_free(p); }
typedef struct {
  int value;
} refcount_t;
static void refcount_set(refcount_t *r, int n) {
  if (n != 1)
    model_bad++;
  r->value = n;
}
static void refcount_inc(refcount_t *r) {
  SUMH_MAPS_CONTROL_CHECK();
  if (r->value < 1 || memory_lock_depth != 1)
    model_bad++;
  r->value++;
}
static bool refcount_dec_and_test(refcount_t *r) {
  if (r->value < 1 || memory_lock_depth)
    model_bad++;
  return --r->value == 0;
}
static void *memchr(const void *p, int c, size_t n) {
  const unsigned char *s = p;
  for (size_t i = 0; i < n; i++)
    if (s[i] == (unsigned char)c)
      return (void *)(s + i);
  return NULL;
}
static int memcmp(const void *a, const void *b, size_t n) {
  const unsigned char *x = a, *y = b;
  for (size_t i = 0; i < n; i++)
    if (x[i] != y[i])
      return x[i] - y[i];
  return 0;
}
static int strscpy(char *dst, const char *src, size_t n) {
  size_t i = 0;
  if (!n)
    return -7;
  while (i + 1 < n && src[i]) {
    dst[i] = src[i];
    i++;
  }
  dst[i] = 0;
  return src[i] ? -7 : (int)i;
}
static void sort(void *base, size_t n, size_t size,
                 int (*cmp)(const void *, const void *),
                 void (*swap)(void *, void *, int)) {
  unsigned char *bytes = base;
  (void)swap;
  for (size_t i = 1; i < n; i++)
    for (size_t j = i; j && cmp(bytes + (j - 1) * size, bytes + j * size) > 0;
         j--)
      for (size_t k = 0; k < size; k++) {
        unsigned char t = bytes[(j - 1) * size + k];
        bytes[(j - 1) * size + k] = bytes[j * size + k];
        bytes[j * size + k] = t;
      }
}
static int copy_from_user(void *out, const void *in, size_t n) {
  SUMH_MAPS_CONTROL_CHECK();
  if (memory_lock_depth)
    model_bad++;
  if (++input_copies == input_fail_at)
    return 1;
  memcpy(out, in, n);
  return 0;
}
static int copy_to_user(void *out, const void *in, size_t n) {
  SUMH_MAPS_CONTROL_CHECK();
  output_copies++;
  if (memory_lock_depth)
    model_bad++;
  if (reply_fault)
    return 1;
  memcpy(out, in, n);
  return 0;
}
static bool sumh_proc_proxy_available(void) { return proxy_available; }
