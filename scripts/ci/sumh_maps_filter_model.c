/* Kernel text helpers, with a scheduling point in the numeric parser. */
enum sumh_policy_scope {
  SUMH_POLICY_SCOPE_NONE,
  SUMH_POLICY_SCOPE_VIEW,
  SUMH_POLICY_SCOPE_SPOOF
};
#define MKDEV(ma, mi) (((unsigned long)(ma) << 20) | (mi))
#define MAJOR(dev) ((dev) >> 20)
#define MINOR(dev) ((dev) & 0xfffff)
static int parse_calls, parse_schedule;
static void (*parse_interleave)(void);
static bool isxdigit(int c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
         (c >= 'A' && c <= 'F');
}
static unsigned long simple_strtoul(const char *p, char **end,
                                    unsigned int base) {
  unsigned long value = 0;
  if (++parse_calls == parse_schedule && parse_interleave) {
    void (*callback)(void) = parse_interleave;
    parse_interleave = NULL;
    callback();
  }
  while (*p) {
    unsigned int digit = *p >= '0' && *p <= '9'   ? *p - '0'
                         : *p >= 'a' && *p <= 'f' ? *p - 'a' + 10
                         : *p >= 'A' && *p <= 'F' ? *p - 'A' + 10
                                                  : 99;
    if (digit >= base)
      break;
    value = value * base + digit;
    p++;
  }
  *end = (char *)p;
  return value;
}
static void format_put(char *out, size_t capacity, size_t *length, char c) {
  if (*length + 1 < capacity)
    out[*length] = c;
  (*length)++;
}
static void format_number(char *out, size_t cap, size_t *length,
                          unsigned long value, unsigned int base,
                          unsigned int width) {
  char digits[32];
  unsigned int n = 0;
  do {
    digits[n++] = "0123456789abcdef"[value % base];
    value /= base;
  } while (value);
  while (width > n) {
    format_put(out, cap, length, '0');
    width--;
  }
  while (n)
    format_put(out, cap, length, digits[--n]);
}
static int scnprintf(char *out, size_t cap, const char *format, ...) {
  __builtin_va_list args;
  __builtin_va_start(args, format);
  size_t length = 0;
  for (const char *p = format; *p; p++) {
    if (*p != '%') {
      format_put(out, cap, &length, *p);
      continue;
    }
    p++;
    unsigned int width = 0;
    while (*p >= '0' && *p <= '9') {
      width = width * 10 + *p - '0';
      p++;
    }
    bool wide = *p == 'l';
    if (wide)
      p++;
    if (*p == 's') {
      const char *s = __builtin_va_arg(args, const char *);
      while (*s)
        format_put(out, cap, &length, *s++);
    } else {
      unsigned long n = wide ? __builtin_va_arg(args, unsigned long)
                             : __builtin_va_arg(args, unsigned int);
      format_number(out, cap, &length, n, *p == 'x' ? 16 : 10, width);
    }
  }
  __builtin_va_end(args);
  if (cap)
    out[length < cap ? length : cap - 1] = 0;
  return length < cap ? (int)length : (int)cap - 1;
}
static bool contains(const char *bytes, size_t size, const char *needle) {
  size_t n = strlen(needle);
  for (size_t i = 0; i + n <= size; i++)
    if (!memcmp(bytes + i, needle, n))
      return true;
  return false;
}
