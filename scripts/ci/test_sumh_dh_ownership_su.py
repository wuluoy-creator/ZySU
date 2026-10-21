"""Actual public SU configuration/binding/refresh/exit on the actual DH core."""

from test_sumh_dh_ownership_source import ROOT, core, FIXTURES, read, no_includes

INPUTS = ("kernel/feature/sucompat_vfs.c", "kernel/feature/sucompat_vfs.h", "uapi/su_path.h")

SERVICES = r"""
typedef unsigned int __u32;
#define ULONG_MAX (~0UL)
#define MAY_WRITE 2
#define EROFS 30
#define KSU_FILE_CONTEXT "u:object_r:test:s0"
struct mnt_idmap {int unused;};
struct cred {int unused;};static struct cred model_cred;static const struct cred *ksu_cred=&model_cred;
static bool model_engine_ready=true,model_allowed=true;
static int cred_depth;
static bool sumh_is_ready(void){return model_engine_ready;}
static const struct cred *override_creds(const struct cred *c){(void)c;cred_depth++;return &model_cred;}
static void revert_creds(const struct cred *c){(void)c;if(cred_depth!=1)BAD();cred_depth--;}
static struct {unsigned int val;} current_uid(void){__typeof__(current_uid()) result={0};return result;}
static bool ksu_is_allow_uid_for_current(unsigned int uid){(void)uid;return model_allowed;}
static int ksu_sucompat_exec_file_open(struct file *f){(void)f;return 0;}
static int ksu_sucompat_exec_file_release(struct inode *i,struct file *f){(void)i;(void)f;return 0;}
static void inode_lock(struct inode *i);
static void inode_unlock(struct inode *i);
static int security_inode_notifysecctx(struct inode *i,void *s,size_t n){(void)i;(void)s;(void)n;return 0;}
/* Linux ARM64 uses 64-bit unsigned long; native Windows uses LLP64. The exact
   production initializer is retained, with this single host-only diagnostic
   suppression. Fixtures assign a host-representable starting inode before use.
   Tests cover checked ULONG_MAX overflow in the host width, not ARM64 identity
   width. Saved generated ARM source may be compiled independently. */
#if defined(_WIN64) && defined(__clang__)
#pragma clang diagnostic ignored "-Wshift-count-overflow"
#endif
"""


def prefix(root):
    return SERVICES + no_includes(read(root, INPUTS[2])) + no_includes(read(root, INPUTS[1]))


CASES = r"""
static struct ksu_su_path_config *su_config(const char *path){
    static struct ksu_su_path_config c;
    memset(&c,0,sizeof(c));c.version=KSU_SU_PATH_VERSION;c.size=sizeof(c);
    strscpy(c.path,path,sizeof(c.path));return &c;
}
static int invalidation_observed;
static void clear_su_reservation(struct mutex *lock){
    if(lock!=&sumh_dh_lock)return;
    struct sumh_dh_dir *dir;struct sumh_dh_child *slot;bool reserved=false;
    list_for_each_entry(dir,&sumh_dh_dirs,list)hlist_for_each_entry(slot,&dir->children,node)if(slot->prepared)reserved=true;
    if(!reserved)return;
    after_mutex_unlock=NULL;sumh_dirhijack_clear();invalidation_observed++;
}
static int su_public_cases(void){
    fixture_init();su_next_ino=10000;
    CHECK(ksu_sucompat_vfs_init()==0);
    CHECK(ksu_sucompat_vfs_refresh()==-ENOENT&&!ksu_sucompat_vfs_enabled());
    native_path("/system/bin",fixture_path(1),0);
    CHECK(ksu_sucompat_vfs_refresh()==0&&ksu_sucompat_vfs_enabled());
    unsigned long first=su_ino;CHECK(first&&ksu_sucompat_vfs_current_ino(first));
    CHECK(ksu_sucompat_vfs_refresh()==0&&su_ino==first);
    struct ksu_su_path_config *c=su_config("/two/custom-su");
    CHECK(ksu_sucompat_vfs_set_config(c)==0&&su_ino!=first);
    CHECK(!ksu_sucompat_vfs_current_ino(first)&&ksu_sucompat_vfs_current_ino(su_ino));
    unsigned long second=su_ino;
    native_path("/two",fixture_path(3),0);
    CHECK(ksu_sucompat_vfs_refresh()==0&&su_ino!=second&&su_binding->slot->dir->dir_inode==&fixture_inodes[3]);second=su_ino;
    ksu_cred=NULL;CHECK(ksu_sucompat_vfs_refresh()==-EAGAIN&&su_ino==second);ksu_cred=&model_cred;
    native_path("/one/existing",fixture_path(18),0);c=su_config("/one/existing");
    CHECK(ksu_sucompat_vfs_set_config(c)==-EEXIST&&su_ino==second);
    struct sumh_hide_binding *hidden=NULL;struct path parent=fixture_path(3);
    CHECK(sumh_dirhijack_hide_get(&parent,"custom-su",&hidden)==-EPERM&&!hidden&&su_ino==second);
    after_mutex_unlock=clear_su_reservation;invalidation_observed=0;c=su_config("/one/interrupted-su");
    CHECK(ksu_sucompat_vfs_set_config(c)==-EAGAIN&&invalidation_observed==1&&su_ino==second&&su_binding->state==DH_ATTACHED);
    CHECK(ksu_sucompat_vfs_set_config(c)==0&&su_ino!=second);second=su_ino;
    c=su_config("/missing/su");CHECK(ksu_sucompat_vfs_set_config(c)==-ENOENT&&su_ino==second);
    CHECK(!strcmp(su_path,"/one/interrupted-su"));
    model_allowed=false;CHECK(!ksu_sucompat_vfs_visible());model_allowed=true;CHECK(ksu_sucompat_vfs_visible());
    sumh_dirhijack_clear();CHECK(ksu_sucompat_vfs_current_ino(second));
    CHECK(ksu_sucompat_vfs_refresh()==0&&su_ino==second);
    su_next_ino=ULONG_MAX;c=su_config("/one/new-su");
    CHECK(ksu_sucompat_vfs_set_config(c)==-EOVERFLOW&&su_ino==second);
    ksu_sucompat_vfs_exit();
    CHECK(!su_ready&&!ksu_sucompat_vfs_enabled()&&su_ino==0);
    c=su_config("/one/post-stop");CHECK(ksu_sucompat_vfs_set_config(c)==0);
    CHECK(!ksu_sucompat_vfs_enabled()&&!strcmp(su_path,"/one/post-stop"));
    CHECK(ksu_sucompat_vfs_refresh()==-EOPNOTSUPP);
    ksu_sucompat_vfs_exit();
    sumh_dirhijack_exit();sumh_dh_drain_releases();sumh_fop_override_exit();sumh_sop_shadow_exit();
    CHECK(!bad&&!live_allocations&&!cred_depth&&fixture_sb.s_active==1);
    return 0;
}
int main(void){return su_public_cases();}
"""


def source(root=ROOT):
    return core(root, su=True) + no_includes(read(root, INPUTS[0])) + FIXTURES + CASES
