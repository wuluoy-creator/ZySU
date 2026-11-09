#!/usr/bin/env python3
"""Run production preinit parsers, boot protection and kallsyms matching with API doubles.

No device actions are performed. These checks do not replace kernel or boot testing.
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import sys

sys.dont_write_bytecode = True
from test_sumh_vfs_helpers import COMMON, ROOT, function, run_test

RUNNER = "kernel/preinit/runner/runner.c"
STRINGS = r'''
static char *strchr(const char *s, int ch) {
    do { if (*s == ch) return (char *)s; } while (*s++); return NULL;
}
static char *strstr(const char *s, const char *key) {
    for (; *s; s++) if (!strncmp(s, key, strlen(key))) return (char *)s;
    return *key ? NULL : (char *)s;
}
static char *strcpy(char *d, const char *s) { memcpy(d, s, strlen(s) + 1); return d; }
static long atol(const char *s) {
    long n = 0, sign = 1; if (*s == '-') { sign = -1; s++; }
    while (*s >= '0' && *s <= '9') n = n * 10 + *s++ - '0'; return sign * n;
}
'''


def runner_source():
    preamble = r'''
#define OWNER_UID 0
#define S_IWGRP 0020
#define S_IWOTH 0002
#define S_ISDIR(m) (((m) & 0170000) == 0040000)
#define S_ISREG(m) (((m) & 0170000) == 0100000)
#define O_WRONLY 1
#define O_CREAT 2
#define O_TRUNC 4
#define O_NOFOLLOW 8
#define O_CLOEXEC 16
#define O_RDONLY 32
#define O_DIRECTORY 64
#define EINTR 4
#define PATH_LEN 512
#define BOOTCOUNT_LIMIT 3
#define BOOTCOUNT_NAME ".preinit_bootcount"
#define CONF_MAX (64 * 1024)
#define CONF_NAME "preinit.conf"
#define MAX_TOKENS 128
#define LINE_MAX_LEN 4096
#define klog(...) ((void)0)
struct stat { unsigned st_uid; unsigned st_mode; long long st_size; };
struct module_ctx { const char *id; const char *dir; };
static char conf_buf[CONF_MAX + 1];
static char counter[16] = "0";
static const char *config;
static size_t read_pos;
static int errno;
static int open_fault, sync_fault, write_fault, write_interrupt, writes, closes;
static unsigned mode = 0644, owner;
static int actions;
static char action_names[8][32];
static int snprintf(char *out, size_t size, const char *format, ...) {
    __builtin_va_list args; __builtin_va_start(args, format);
    char buf[512]; size_t n = 0;
    if (!strcmp(format, "%ld\n")) {
        long value = __builtin_va_arg(args, long);
        char digits[20]; int i = 0;
        do { digits[i++] = (char)('0' + value % 10); value /= 10; } while (value);
        while (i) buf[n++] = digits[--i]; buf[n++] = '\n';
    } else {
        const char *a = __builtin_va_arg(args, const char *);
        const char *b = __builtin_va_arg(args, const char *);
        while (*a) buf[n++] = *a++; buf[n++] = '/'; while (*b) buf[n++] = *b++;
    }
    __builtin_va_end(args);
    if (size) { size_t take = n < size - 1 ? n : size - 1; memcpy(out, buf, take); out[take] = 0; }
    return (int)n;
}
static int read_file(const char *path, char *out, size_t size) {
    (void)path; size_t n = strlen(counter); if (n >= size) n = size - 1;
    memcpy(out, counter, n); out[n] = 0; return (int)n;
}
static int open(const char *path, int flags, ...) {
    (void)path; if (open_fault) return -1;
    if (flags & O_DIRECTORY) return 2;
    if (flags & O_WRONLY) { counter[0] = 0; return 1; }
    read_pos = 0; return 3;
}
static ssize_t write(int fd, const char *in, size_t size) {
    (void)fd;
    if (write_interrupt) { write_interrupt = 0; errno = EINTR; return -1; }
    if (write_fault) { errno = 5; return -1; }
    size_t n = strlen(counter); size_t take = size > 1 ? 1 : size;
    memcpy(counter + n, in, take); counter[n + take] = 0; writes++; return take;
}
static int fsync(int fd) { (void)fd; return sync_fault ? -1 : 0; }
static int close(int fd) { (void)fd; closes++; return 0; }
static int lstat(const char *p, struct stat *s) { (void)p; s->st_mode=0040755; s->st_uid=owner; return 0; }
static int fstat(int fd, struct stat *s) { (void)fd; s->st_mode=0100000|mode; s->st_uid=owner; s->st_size=strlen(config); return 0; }
static int chdir(const char *dir) { (void)dir; return 0; }
static ssize_t read(int fd, char *out, size_t size) {
    (void)fd; size_t n = strlen(config) - read_pos; if (n > size) n = size;
    memcpy(out, config + read_pos, n); read_pos += n; return n;
}
static int run_action(const struct module_ctx *m, int argc, char **argv) {
    (void)m; (void)argc; if (actions >= 8) return -1;
    strcpy(action_names[actions++], argv[0]); return !strcmp(argv[0], "bad") ? -1 : 0;
}
'''
    methods = "\n".join(function(RUNNER, name) for name in [
        "static bool is_space", "static bool safe_owner", "static int write_all",
        "static bool find_boot_mode", "static bool bootcount_check_and_bump",
        "static int tokenize", "static void run_module"])
    body = r'''
int main(void) {
    char text[256], value[32], *args[8];
    strcpy(text, "exec bin/tool \"two words\" key=\"a b\" \\\"quoted\\\" \\\\literal");
    CHECK(tokenize(text, args, 8) == 6);
    CHECK(!strcmp(args[2], "two words") && !strcmp(args[3], "key=a b"));
    CHECK(!strcmp(args[4], "\"quoted\"") && !strcmp(args[5], "\\literal"));
    strcpy(text, "exec \"unterminated"); CHECK(tokenize(text, args, 8) == -1);
    strcpy(text, "a b c"); CHECK(tokenize(text, args, 3) == -1);
    strcpy(text, "   \t\r"); CHECK(tokenize(text, args, 8) == 0 && args[0] == NULL);
    CHECK(find_boot_mode("xandroidboot.mode=bad androidboot.mode = \"charger\"\n", value, sizeof(value)));
    CHECK(!strcmp(value, "charger"));
    CHECK(find_boot_mode("androidboot.mode=normal other=1", value, sizeof(value)) && !strcmp(value, "normal"));
    CHECK(!find_boot_mode("androidboot.modeX=charger", value, sizeof(value)));
    struct stat st = {0, 0755, 0}; CHECK(safe_owner(&st));
    st.st_mode = 0775; CHECK(!safe_owner(&st)); st.st_mode = 0755; st.st_uid=2000; CHECK(!safe_owner(&st));
    for (int i=1; i<=3; ++i) { CHECK(bootcount_check_and_bump("/metadata/ksu")); CHECK(atol(counter)==i); }
    CHECK(!bootcount_check_and_bump("/metadata/ksu") && atol(counter)==4);
    int prior = writes; CHECK(!bootcount_check_and_bump("/metadata/ksu") && writes==prior);
    strcpy(counter, "-7"); write_interrupt=1; CHECK(bootcount_check_and_bump("/metadata/ksu") && atol(counter)==1);
    strcpy(counter, "0"); open_fault=1; CHECK(!bootcount_check_and_bump("/metadata/ksu")); open_fault=0;
    write_fault=1; CHECK(!bootcount_check_and_bump("/metadata/ksu")); write_fault=0;
    strcpy(counter, "0"); sync_fault=1; CHECK(!bootcount_check_and_bump("/metadata/ksu")); sync_fault=0;
    config="\n# comment\nexec first\n\nwrite second\n";
    run_module("/preinit", "mod"); CHECK(actions==2 && !strcmp(action_names[1], "write"));
    actions=0; config="bad?\nexec next\n"; run_module("/preinit", "mod"); CHECK(actions==2);
    actions=0; config="bad\nexec never\n"; run_module("/preinit", "mod"); CHECK(actions==1);
    actions=0; config="exec \"broken\nwrite never\n"; run_module("/preinit", "mod"); CHECK(actions==0);
    mode=0666; config="exec forbidden\n"; run_module("/preinit", "mod"); CHECK(actions==0); mode=0644;
    owner=2000; run_module("/preinit", "mod"); CHECK(actions==0);
    return 0;
}
'''
    return COMMON + STRINGS + preamble + methods + body


def symbols_source():
    text = (ROOT / "kernel/infra/symbol_resolver.c").read_text(encoding="utf-8")
    methods = text[text.index("struct ksu_batch_entry"):text.index("void __init ksu_init_symbol_resolver")]
    preamble = r'''
typedef unsigned int u32;
#define __nocfi
#define GFP_KERNEL 0
#define KSYM_NAME_LEN 512
#define ENOMEM 12
#define EINTR 4
#define KERNEL_VERSION(a,b,c) (((a)<<16)|((b)<<8)|(c))
#define LINUX_VERSION_CODE KERNEL_VERSION(6,6,0)
#define current NULL
static bool signal_pending;
#define fatal_signal_pending(task) signal_pending
static unsigned char arena[65536];
static size_t used;
static int alloc_failure;
static void *kzalloc(size_t n, int flags) {
    (void)flags; if (alloc_failure || n > sizeof(arena)-used) return NULL;
    void *p=arena+used; used+=(n+15)&~(size_t)15; memset(p,0,n); return p;
}
static void *kvmalloc_array(size_t count,size_t size,int flags) { return kzalloc(count*size,flags); }
#define kfree(p) ((void)(p))
#define kvfree(p) ((void)(p))
static void sort(void *data,size_t n,size_t size,int (*cmp)(const void*,const void*),void *swap) {
    (void)swap; unsigned char *p=data;
    for(size_t i=0;i<n;i++) for(size_t j=i+1;j<n;j++) if(cmp(p+i*size,p+j*size)>0)
      for(size_t k=0;k<size;k++) { unsigned char c=p[i*size+k];p[i*size+k]=p[j*size+k];p[j*size+k]=c; }
}
static void *bsearch(const void *key,const void *data,size_t n,size_t size,int (*cmp)(const void*,const void*)) {
    const unsigned char *p=data; size_t low=0, high=n;
    while(low<high) { size_t mid=low+(high-low)/2;int c=cmp(key,p+mid*size);if(!c)return (void*)(p+mid*size);if(c<0)high=mid;else low=mid+1; } return NULL;
}
struct test_symbol { const char *name; unsigned long addr; };
static struct test_symbol symbols[]={{"dup",100},{"dup",200},{"alpha.llvm.1",300},{"beta$part",400},{"zero",0}};
static int walk(int (*cb)(void*,const char*,unsigned long),void *ctx) {
    for(size_t i=0;i<sizeof(symbols)/sizeof(symbols[0]);i++) { int ret=cb(ctx,symbols[i].name,symbols[i].addr);if(ret)return ret; } return 0;
}
static int indexed(int (*cb)(void*,unsigned long),const char *name,void *ctx) {
    for(size_t i=0;i<sizeof(symbols)/sizeof(symbols[0]);i++) if(!strcmp(name,symbols[i].name)) {int ret=cb(ctx,symbols[i].addr);if(ret)return ret;}return 0;
}
static int (*kallsyms_on_each_symbol_fn)(int (*)(void*,const char*,unsigned long),void*)=walk;
static int (*kallsyms_on_each_match_symbol_fn)(int (*)(void*,unsigned long),const char*,void*)=indexed;
'''
    body = r'''
int main(void) {
    const char *names[]={"beta", "dup", "alpha", "dup", "missing", NULL, "zero"}; unsigned long out[7];
    CHECK(ksu_lookup_kernel_symbols(names,out,7)==4);
    CHECK(out[0]==400 && out[1]==100 && out[2]==300 && out[3]==100 && !out[4] && !out[5] && !out[6]);
    used=0;kallsyms_on_each_match_symbol_fn=NULL;
    CHECK(ksu_lookup_kernel_symbols(names,out,7)==4 && out[1]==100);
    used=0;kallsyms_on_each_symbol_fn=NULL;kallsyms_on_each_match_symbol_fn=indexed;
    CHECK(ksu_lookup_kernel_symbols(names,out,7)==2 && out[1]==100 && !out[0]);
    used=0;signal_pending=true;CHECK(ksu_lookup_kernel_symbols(names,out,7)==-EINTR);signal_pending=false;
    used=0;alloc_failure=1;CHECK(ksu_lookup_kernel_symbols(names,out,7)==-ENOMEM);
    return 0;
}
'''
    return COMMON + STRINGS + preamble + methods + body


def arch_source(version):
    arch = re.sub(r"^#include[^\n]*\n", "", (ROOT / "kernel/include/arch.h").read_text(encoding="utf-8"), flags=re.M)
    preamble = f"\n#define __aarch64__ 1\n#define KERNEL_VERSION(a,b,c) (((a)<<16)|((b)<<8)|(c))\n#define LINUX_VERSION_CODE {version}\n"
    body = r'''
struct pt_regs { unsigned long long regs[31], sp, pc, orig_x0; };
int main(void) {
    struct pt_regs regs={0};
    PT_REGS_PARM1(&regs)=11; regs.orig_x0=22;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(7,3,0)
    CHECK(PT_REGS_SYSCALL_PARM1(&regs)==22);
    PT_REGS_SYSCALL_PARM1(&regs)=33;
    CHECK(regs.orig_x0==33 && regs.regs[0]==11);
#else
    CHECK(PT_REGS_SYSCALL_PARM1(&regs)==11);
    PT_REGS_SYSCALL_PARM1(&regs)=33;
    CHECK(regs.regs[0]==33 && regs.orig_x0==22);
#endif
    CHECK(PT_REGS_RC(&regs)==regs.regs[0]);
    return 0;
}
'''
    return COMMON+preamble+arch+body


def hook_source():
    preamble = r'''
typedef long long loff_t;
#define __init
#define __user
#define INIT_PATH "/system/bin/init"
#define O_CLOEXEC 1
#define LOOKUP_FOLLOW 1
#define likely(x) (x)
#define READ_ONCE(v) (v)
#define xchg(p,v) __extension__({ __typeof__(*(p)) old=*(p);*(p)=(v);old; })
#define pr_info(...) ((void)0)
#define pr_err(...) ((void)0)
#define IS_ERR(p) ((p)==(void*)-1)
#define PTR_ERR(p) (-5)
#define untagged_addr(p) (p)
#define get_user(out,p) ((out)=*(p), 0)
struct file { int refs; } staged;
struct path { int unused; };
struct pt_regs { const char *filename; };
static struct file *ksu_preinit_file;
static bool ksu_preinit_enabled=true;
static int fd_error, path_error, stack_error, closes, released, exec_calls;
static long exec_result;
static char user_fd_path[32];
static const char *expected_original;
static long strncpy_from_user(char *dst,const char *src,size_t n) {
    size_t count=strnlen(src,n); memcpy(dst,src,count); if(count<n)dst[count]=0;return count;
}
static int get_unused_fd_flags(int flags) { (void)flags; return fd_error ? -24 : 8; }
static void fput(struct file *file) { file->refs--; released++; }
static void fd_install(int fd,struct file *file) { (void)fd;(void)file; }
static void ksu_close_fd(int fd) { (void)fd; closes++; fput(&staged); }
static int kern_path(const char *s,int flags,struct path *p) { (void)s;(void)flags;(void)p;return path_error; }
static void path_put(struct path *p) { (void)p; }
static int snprintf(char *out,size_t n,const char *fmt,int fd) { (void)n;(void)fmt;(void)fd;strcpy(out,"/proc/self/fd/8");return 15; }
static void *userspace_stack_buffer(const void *s,size_t n) {
    if(stack_error)return NULL;memcpy(user_fd_path,s,n);return user_fd_path;
}
static long original_syscall(const struct pt_regs *regs) {
    exec_calls++;
    if (regs->filename==expected_original) return 0;
    return exec_result;
}
static long (*ksu_syscall_table[])(const struct pt_regs *)={original_syscall};
'''
    methods="\n".join(function("kernel/preinit/preinit.c", name) for name in [
        "void ksu_preinit_release", "static bool is_selinux_setup", "bool ksu_preinit_handle_execve"])
    body=r'''
static void reset(void) {
    staged.refs=1;ksu_preinit_file=&staged;closes=released=exec_calls=0;
    fd_error=path_error=stack_error=0;exec_result=0;
}
int main(void) {
    const char *args[]={INIT_PATH,"selinux_setup",NULL};long result=-99;
    struct pt_regs regs={INIT_PATH};expected_original=regs.filename;
    CHECK(!ksu_preinit_handle_execve(NULL,NULL,0,&regs,&result));
    reset();regs.filename="/other";CHECK(!ksu_preinit_handle_execve(&regs.filename,args,0,&regs,&result));
    CHECK(ksu_preinit_file==&staged && !exec_calls);regs.filename=expected_original;
    args[1]="second_stage";CHECK(!ksu_preinit_handle_execve(&regs.filename,args,0,&regs,&result));args[1]="selinux_setup";
    fd_error=1;CHECK(!ksu_preinit_handle_execve(&regs.filename,args,0,&regs,&result));
    CHECK(!ksu_preinit_file && released==1 && !exec_calls);
    reset();path_error=1;CHECK(!ksu_preinit_handle_execve(&regs.filename,args,0,&regs,&result));CHECK(closes==1 && staged.refs==0);
    reset();stack_error=1;CHECK(!ksu_preinit_handle_execve(&regs.filename,args,0,&regs,&result));CHECK(regs.filename==expected_original && closes==1);
    reset();exec_result=-8;CHECK(ksu_preinit_handle_execve(&regs.filename,args,0,&regs,&result));
    CHECK(result==0 && exec_calls==2 && closes==1 && regs.filename==expected_original);
    reset();CHECK(ksu_preinit_handle_execve(&regs.filename,args,0,&regs,&result));
    CHECK(result==0 && exec_calls==1 && !closes && !ksu_preinit_file && !strcmp(regs.filename,"/proc/self/fd/8"));
    CHECK(!ksu_preinit_handle_execve(NULL,NULL,0,&regs,&result));
    reset();ksu_preinit_release();ksu_preinit_release();CHECK(released==1 && !ksu_preinit_file);
    return 0;
}
'''
    # The kernel uses LP64; Windows unsigned long is only 32 bits.
    if os.name == "nt":
        methods = methods.replace("unsigned long", "__UINTPTR_TYPE__")
    return COMMON+STRINGS+preamble+methods+body


def initrc_source():
    preamble=r'''
struct dentry { bool regular; const char *name; };
struct path { struct dentry *dentry; const char *name; };
struct file { struct path f_path; };
struct task { const char *comm; } task;
#define current (&task)
#define d_is_reg(d) ((d)->regular)
#define IS_ERR(p) ((p)==(void*)-1)
static char *d_path(const struct path *p,char *buf,size_t n) { (void)buf;(void)n;return (char*)p->name; }
'''
    body=r'''
int main(void) {
    struct dentry d={true,"custom-source.rc"};
    struct file f={{&d,"/system/etc/init/hw/init.rc"}};task.comm="init";
    CHECK(is_init_rc(&f));
    f.f_path.name="/data/init.rc";CHECK(!is_init_rc(&f));
    f.f_path.name="/system/etc/init/hw/init.rc";d.regular=false;CHECK(!is_init_rc(&f));d.regular=true;
    task.comm="other";CHECK(!is_init_rc(&f));task.comm="init";
    f.f_path.name=(void*)-1;CHECK(!is_init_rc(&f));return 0;
}
'''
    return COMMON+preamble+function("kernel/runtime/ksud_integration.c","static bool is_init_rc")+body


if __name__ == "__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC") or shutil.which("clang") or shutil.which("cc"))
    args=parser.parse_args()
    if not args.cc: parser.error("a C compiler is required; pass --cc")
    compiler=shutil.which(args.cc) or str(Path(args.cc).resolve())
    run_test(compiler,"preinit-runner",runner_source())
    run_test(compiler,"preinit-symbols",symbols_source())
    run_test(compiler,"syscall-arg1-before-7.3",arch_source((6<<16)|(18<<8)))
    run_test(compiler,"syscall-arg1-7.3",arch_source((7<<16)|(3<<8)))
    run_test(compiler,"preinit-exec-fallback",hook_source())
    run_test(compiler,"initrc-bind-mount",initrc_source())
