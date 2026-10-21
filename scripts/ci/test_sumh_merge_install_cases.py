"""Cases for the actual production MERGE installation functions."""

TESTS = r"""
static void init(void){
    setup(S_IFDIR);sumh_merge_dirs[0].first=NULL;vnode_source=trylock_fail=false;
    dir_lock=lookup_child_error=lookup_children=marker_refs=0;
    open_error=iterate_error=opens=iterations=files_live=files_peak=0;
    batch_size=iteration_return=open_fail_at=child_error=checked_error=visible_error=0;
    no_progress=visible_children=replace_before_children=replace_before_checked=false;
    relative_lookups=resolved_captures=checked_captures=source_text_lookups=extra_count=0;
    iter_lock=0;old_i.i_rdev=new_i.i_rdev=link_i.i_rdev=visible_i.i_rdev=1;
    memset(files,0,sizeof(files));memset(dirs,0,sizeof(dirs));
    memset(extra_i,0,sizeof(extra_i));memset(extra_d,0,sizeof(extra_d));
    memset(extra_p,0,sizeof(extra_p));memset(&opened_path,0,sizeof(opened_path));
    memset(&first_child,0,sizeof(first_child));memset(&checked_leaf,0,sizeof(checked_leaf));
    followed_link=(struct path){&new_m,&new_d};new_i.i_mode=S_IFREG;
    dirs[0].path=captured;dirs[1].path=(struct path){&new_m,&new_d};dir_count=2;
    sumh_lookup_one_len=lookup_one;
}
static struct path node(umode_t mode){
    int n=extra_count++;extra_i[n]=(struct inode){mode,10+n,0,1};
    native_inode(&extra_i[n],5+n);
    extra_d[n]=(struct dentry){&extra_i[n],0};extra_p[n]=(struct path){&old_m,&extra_d[n]};
    if(S_ISDIR(mode))dirs[dir_count++].path=extra_p[n];return extra_p[n];
}
static void child(struct path parent,const char *name,unsigned type,struct path p){
    struct model_dir *d=find_dir(&parent);if(!d||d->count==16){bad++;return;}
    d->items[d->count++]=(struct model_item){name,(int)strlen(name),type,p};
    if(!first_child.dentry)first_child=p;
}
/* Production materialization requires an already published prefix owner.
   Register it through the real nested producer; publication assertions below
   remain deltas from this fixture setup, not total live MERGE counts. */
static int prepare_root(const char *prefix,const char *target,const struct path *p){
    struct path held={0};int before=publishes;
    int ret=sumh_register_nested_merge(prefix,target,p,&held);
    if(!ret){path_put(&held);publishes=before;}
    return ret;
}
static int materialize_owned(const char *prefix,const char *target,const struct path *p,int depth){
    if(!p||!p->dentry||depth<0||depth>8||!S_ISDIR(d_inode(p->dentry)->i_mode))return sumh_materialize_merge(prefix,target,p,depth);
    int ret=prepare_root(prefix,target,p);return ret?ret:sumh_materialize_merge(prefix,target,p,depth);
}
static int materialize_depth_owned(const char *prefix,const char *target,const struct path *p,int depth,struct sumh_mat_budget *budget){
    int ret=prepare_root(prefix,target,p);return ret?ret:sumh_materialize_merge_depth(prefix,target,p,depth,budget);
}
static int materialize(void){path_get(&captured);int ret=materialize_owned("/visible/file","/source",&captured,0);path_put(&captured);return ret;}
static void finish(void){cleanup();}
static int balanced(void){
    if(!clean()||dir_lock||marker_refs||iter_lock||files_live)return 0;
    for(int i=0;i<extra_count;i++)if(extra_d[i].refs)return 0;return 1;
}
static struct sumh_entry *entry(const char *src){struct sumh_entry *e;hlist_for_each_entry(e,&sumh_paths[0],node)if(!strcmp(e->src,src))return e;return NULL;}
static int capture_registration(void){
    struct path p={0};
    init();replace_on_lookup=true;dirs[1].marker=true;
    CHECK(sumh_capture_merge_target("/source",&p)==0);
    CHECK(path_equal(&p,&captured)&&target_lookups==1&&lookup_children==1);
    CHECK(materialize_owned("/visible/file","/source",&p,0)==0);
    CHECK(path_equal(&opened_path,&captured)&&target_lookups==1&&opens==1);
    path_put(&p);finish();CHECK(balanced());
    for(int variant=0;variant<8;variant++){
        init();int expected=0;
        switch(variant){
        case 0:sumh_lookup_one_len=NULL;expected=-EOPNOTSUPP;break;
        case 1:lookup_error=-ENOENT;expected=-ENOENT;break;
        case 2:old_i.i_mode=S_IFREG;expected=-ENOTDIR;break;
        case 3:old_d.inode=NULL;expected=-ENOTDIR;break;
        case 4:vnode_source=true;expected=-EOPNOTSUPP;break;
        case 5:trylock_fail=true;expected=-EAGAIN;break;
        case 6:lookup_child_error=-EIO;expected=-EIO;break;
        case 7:dirs[0].marker=true;expected=-EOPNOTSUPP;break;
        }
        p=captured;CHECK(sumh_capture_merge_target("/source",&p)==expected);CHECK(!p.mnt&&!p.dentry);
        finish();CHECK(balanced());
    }
    init();p=captured;CHECK(sumh_capture_merge_target(NULL,&p)==-EINVAL);CHECK(!p.mnt&&!p.dentry);
    CHECK(sumh_capture_merge_target("/source",NULL)==-EINVAL);finish();CHECK(balanced());
    init();replace_on_lookup=true;CHECK(submit_merge()==0);
    CHECK(publishes==1&&opens==1&&iterations==1&&target_lookups==1);
    CHECK(path_equal(&opened_path,&captured));finish();CHECK(balanced());
    init();CHECK(submit_merge()==0);int count=allocs;finish();CHECK(balanced());
    for(int i=3;i<=count;i++){
        init();fail_at=i;int ret=submit_merge();CHECK(ret==0||ret==-ENOMEM);
        if(!publishes)CHECK(opens==0);if(i==3||i==4)CHECK(ret==-ENOMEM&&!publishes);
        finish();CHECK(balanced());
    }
    init();CHECK(submit_merge()==0);current_target=captured;
    CHECK(submit_merge()==-EEXIST);CHECK(publishes==1&&opens==1);finish();CHECK(balanced());
    for(int variant=0;variant<3;variant++){
        init();if(variant==0)open_error=-EIO;else if(variant==1)iterate_error=-EIO;
        else{child(captured,"test",DT_REG,node(S_IFREG));capture_error=-EIO;}
        CHECK(submit_merge()==-EIO);CHECK(publishes==1);finish();CHECK(balanced());
    }
    init();path_get(&captured);CHECK(sumh_register_nested_merge("/visible/file","/source",&captured,&p)==0);
    CHECK(path_equal(&p,&captured)&&publishes==1);path_put(&p);
    lookup_error=-ENOENT;int lookups=target_lookups;
    /* Existing identity wins even if the newly supplied root is invalid. */
    CHECK(sumh_register_nested_merge("/visible/file","/source",NULL,&p)==0);
    CHECK(path_equal(&p,&captured)&&publishes==1&&target_lookups==lookups);path_put(&p);path_put(&captured);finish();CHECK(balanced());
    init();CHECK(sumh_register_nested_merge("/visible/file","/source",&captured,&p)==0);count=allocs;path_put(&p);finish();CHECK(balanced());
    for(int i=1;i<=count;i++){
        init();fail_at=i;int ret=sumh_register_nested_merge("/visible/file","/source",&captured,&p);
        CHECK(ret==0||ret==-ENOMEM);if(!ret){CHECK(publishes==1&&p.dentry);path_put(&p);}else CHECK(!publishes&&!p.dentry&&!p.mnt);
        finish();CHECK(balanced());
    }
    init();CHECK(sumh_register_nested_merge("/visible/file","/source",NULL,&p)==-EINVAL);CHECK(!publishes&&!p.dentry);finish();CHECK(balanced());
    init();path_get(&captured);
    CHECK(sumh_materialize_merge("/visible/file","/source",&captured,9)==-ELOOP);
    CHECK(sumh_materialize_merge("/visible/file","/source",&captured,-1)==-ELOOP);
    CHECK(sumh_materialize_merge("/visible/file","/source",NULL,0)==-EINVAL);
    old_i.i_mode=S_IFREG;CHECK(sumh_materialize_merge("/visible/file","/source",&captured,0)==-ENOTDIR);
    CHECK(!opens);path_put(&captured);finish();CHECK(balanced());return 0;
}
static int materialization(void){
    init();struct path a=node(S_IFREG),b=node(S_IFREG),c=node(S_IFREG);
    child(captured,"a",DT_UNKNOWN,a);child(captured,"b",DT_DIR,b);child(captured,"c",DT_REG,c);
    batch_size=1;replace_before_children=true;
    CHECK(materialize()==0);CHECK(iterations==4&&relative_lookups==3&&resolved_captures==3&&!source_text_lookups&&!target_lookups);
    CHECK(entry("/visible/file/a")->source_path.dentry==a.dentry);
    CHECK(entry("/visible/file/b")->type==DT_REG&&entry("/visible/file/c")->source_path.dentry==c.dentry);
    finish();CHECK(balanced());
    /* A stale d_type cannot prevent nested MERGE; all children stay relative
       to held objects after the root string changes. */
    init();a=node(S_IFDIR);b=node(S_IFREG);child(captured,"child",DT_UNKNOWN,a);child(a,"inside",DT_DIR,b);
    visible_children=true;replace_before_children=true;batch_size=1;
    CHECK(materialize()==0);CHECK(opens==2&&iterations==4&&files_peak==1&&relative_lookups==2);
    CHECK(!entry("/visible/file/child")&&entry("/visible/file/child/inside")->source_path.dentry==b.dentry);
    CHECK(!source_text_lookups&&!target_lookups&&publishes==2);int count=allocs;finish();CHECK(balanced());
    for(int i=1;i<=count;i++){
        init();a=node(S_IFDIR);b=node(S_IFREG);child(captured,"child",DT_UNKNOWN,a);child(a,"inside",DT_REG,b);
        visible_children=true;batch_size=1;fail_at=i;int ret=materialize();CHECK(ret==0||ret==-ENOMEM);
        finish();CHECK(balanced());
    }
    init();a=node(S_IFDIR);child(captured,"new",DT_UNKNOWN,a);CHECK(materialize()==0);
    CHECK(opens==1&&entry("/visible/file/new")->source_path.dentry==a.dentry&&shadow_flags==SUMH_VNODE_F_DIR);finish();CHECK(balanced());
    init();child(captured,"child",DT_DIR,captured);visible_children=true;CHECK(materialize()==-ELOOP);CHECK(opens==1&&publishes==1);finish();CHECK(balanced());
    for(int variant=0;variant<6;variant++){
        init();a=node(S_IFREG);child(captured,"a",DT_REG,a);int expected=-EIO;
        switch(variant){case 0:no_progress=true;break;case 1:iterate_error=-EIO;break;case 2:child_error=-EIO;break;
        case 3:extra_d[0].inode=NULL;expected=-ENOENT;break;case 4:shadow_error=-EIO;break;case 5:vnode_source=true;expected=-EOPNOTSUPP;break;}
        CHECK(materialize()==expected);if(variant==0||variant==1)CHECK(!relative_lookups&&!publishes);
        if(variant==4)CHECK(publishes==0);finish();CHECK(balanced());
    }
    init();iteration_return=1;child(captured,"a",DT_REG,node(S_IFREG));CHECK(materialize()==0);CHECK(iterations==2);finish();CHECK(balanced());
    /* Symlinks use the compatibility checked capture, never a rooted FOLLOW. */
    init();a=node(S_IFLNK);child(captured,"link",DT_REG,a);CHECK(materialize()==0);
    CHECK(checked_captures==1&&source_text_lookups==1&&path_equal(&checked_leaf,&a));
    CHECK(entry("/visible/file/link")->source_nofollow_path.dentry==a.dentry&&shadow_flags==SUMH_VNODE_F_LNK);finish();CHECK(balanced());
    init();a=node(S_IFLNK);child(captured,"link",DT_LNK,a);replace_before_checked=true;CHECK(materialize()==-ESTALE);CHECK(!publishes);finish();CHECK(balanced());
    /* Capture errors reach the materializer unchanged. A previously published
       root remains; no failing child is published and no global clear occurs. */
    const int checked_errors[]={-ENOENT,-ENOTDIR,-EACCES,-ELOOP,-EIO,-ENOMEM,-ESTALE};
    for(int root_published=0;root_published<2;root_published++)for(int prior_sibling=0;prior_sibling<2;prior_sibling++)for(int i=0;i<7;i++){
        init();struct path sibling={0};
        if(prior_sibling){sibling=node(S_IFREG);child(captured,"first",DT_REG,sibling);}
        child(captured,"link",DT_LNK,node(S_IFLNK));checked_error=checked_errors[i];
        CHECK((root_published?submit_merge():materialize())==checked_errors[i]);
        CHECK(checked_captures==1&&publishes==root_published+prior_sibling);
        CHECK(!entry("/visible/file/link")&&shadow_calls==prior_sibling);
        if(prior_sibling){
            struct sumh_entry *kept=entry("/visible/file/first");
            CHECK(kept&&path_equal(&kept->source_path,&sibling)&&path_equal(&shadow,&sibling));
        }
        if(root_published)CHECK(sumh_merge_dirs[0].first!=NULL);
        finish();CHECK(balanced());
    }
    init();a=node(S_IFCHR);extra_i[0].i_rdev=0;child(captured,"whiteout",DT_CHR,a);CHECK(materialize()==-EOPNOTSUPP);CHECK(!publishes);finish();CHECK(balanced());
    const int visible_errors[]={-EIO,-EACCES,-ENOENT,-ENOTDIR};
    for(int i=0;i<4;i++){
        init();a=node(S_IFDIR);child(captured,"child",DT_DIR,a);visible_error=visible_errors[i];
        CHECK(materialize()==(i<2?visible_errors[i]:0));CHECK(publishes==(i<2?0:1));finish();CHECK(balanced());
    }
    for(int i=0;i<2;i++){
        init();a=node(S_IFDIR);child(captured,"child",DT_DIR,a);visible_children=true;
        if(i==0)find_dir(&a)->marker=true;else open_fail_at=2;
        CHECK(materialize()==(i==0?-EOPNOTSUPP:-EIO));CHECK(publishes==i);finish();CHECK(balanced());
    }
    const umode_t modes[]={S_IFCHR,S_IFBLK,S_IFIFO};
    for(int i=0;i<3;i++){
        init();a=node(modes[i]);child(captured,"special",DT_UNKNOWN,a);CHECK(materialize()==0);
        CHECK(shadow_flags==SUMH_VNODE_F_SPECIAL&&entry("/visible/file/special")->type==S_DT(modes[i]));finish();CHECK(balanced());
    }
    return 0;
}
static void ctx_init(struct sumh_mat_ctx *mc,struct sumh_mat_budget *budget){memset(mc,0,sizeof(*mc));mc->budget=budget;mc->ctx.actor=sumh_mat_filldir;INIT_LIST_HEAD(&mc->names);}
static int bounds(void){
    const char badnul[]={'a',0,'b'};const char *invalid[]={"", "a/b",badnul,".replace",NULL,"a"};const int lengths[]={0,3,3,8,1,-1};
    for(int i=0;i<6;i++){
        init();struct sumh_mat_budget budget={0};struct sumh_mat_ctx mc;ctx_init(&mc,&budget);
        CHECK(!sumh_mat_filldir(&mc.ctx,invalid[i],lengths[i],0,1,DT_REG));
        CHECK(mc.error==(i==3?-EOPNOTSUPP:-EIO)&&!publishes&&!relative_lookups);sumh_mat_clear_names(&mc);CHECK(!budget.bytes);finish();CHECK(balanced());
    }
    for(int variant=0;variant<4;variant++){
        init();struct sumh_mat_budget budget={0};struct sumh_mat_ctx mc;ctx_init(&mc,&budget);
        if(variant==0)budget.records=SUMH_MERGE_MAX_RECORDS;
        if(variant==1)budget.names=SUMH_MERGE_MAX_NAMES;
        if(variant==2)budget.bytes=SUMH_MERGE_MEMORY_BUDGET;
        if(variant==3)fail_at=1;
        size_t before=budget.bytes;CHECK(!sumh_mat_filldir(&mc.ctx,"a",1,0,1,DT_REG));
        CHECK(mc.error==(variant==3?-ENOMEM:-E2BIG));sumh_mat_clear_names(&mc);CHECK(budget.bytes==before);finish();CHECK(balanced());
    }
    init();struct sumh_mat_budget budget={0};struct sumh_mat_ctx mc;ctx_init(&mc,&budget);
    CHECK(sumh_mat_filldir(&mc.ctx,".",1,0,1,DT_DIR));CHECK(sumh_mat_filldir(&mc.ctx,"..",2,0,1,DT_DIR));
    CHECK(budget.records==2&&!budget.names&&!budget.bytes);finish();CHECK(balanced());
    init();memset(&budget,0,sizeof(budget));ctx_init(&mc,&budget);char maxname[NAME_MAX+1];memset(maxname,'x',NAME_MAX);maxname[NAME_MAX]=0;
    CHECK(sumh_mat_filldir(&mc.ctx,maxname,NAME_MAX,0,1,DT_REG));CHECK(budget.names==1&&budget.bytes>NAME_MAX);
    CHECK(!sumh_mat_filldir(&mc.ctx,maxname,NAME_MAX+1,0,1,DT_REG));CHECK(mc.error==-EIO);
    sumh_mat_clear_names(&mc);CHECK(!budget.bytes);finish();CHECK(balanced());
    for(int variant=0;variant<2;variant++){
        init();memset(&budget,0,sizeof(budget));if(variant==0)budget.directories=SUMH_MERGE_MAX_TARGETS;else budget.passes=SUMH_MERGE_MAX_PASSES;
        path_get(&captured);CHECK(materialize_depth_owned("/visible/file","/source",&captured,0,&budget)==-E2BIG);path_put(&captured);
        CHECK(!publishes&&!relative_lookups&&!budget.bytes);finish();CHECK(balanced());
    }
    /* These limits must be shared with the nested call, never reset per dir. */
    for(int variant=0;variant<4;variant++){
        init();struct path a=node(S_IFDIR),b=node(S_IFREG);child(captured,"child",DT_DIR,a);child(a,"inside",DT_REG,b);visible_children=true;
        memset(&budget,0,sizeof(budget));
        if(variant==0)budget.records=SUMH_MERGE_MAX_RECORDS-1;
        if(variant==1)budget.names=SUMH_MERGE_MAX_NAMES-1;
        if(variant==2)budget.directories=SUMH_MERGE_MAX_TARGETS-1;
        if(variant==3)budget.passes=SUMH_MERGE_MAX_PASSES-2;
        path_get(&captured);CHECK(materialize_depth_owned("/visible/file","/source",&captured,0,&budget)==-E2BIG);path_put(&captured);
        CHECK(!entry("/visible/file/child/inside")&&!budget.bytes&&publishes==1);finish();CHECK(balanced());
    }
    /* Exact remaining workspace capacity accepts one entry and rejects the
       next without discarding the pre-existing accounting baseline. */
    init();memset(&budget,0,sizeof(budget));ctx_init(&mc,&budget);
    size_t reservation=SUMH_MERGE_MEMORY_BUDGET-(sizeof(struct sumh_mat_name)+2);budget.bytes=reservation;
    CHECK(sumh_mat_filldir(&mc.ctx,"a",1,0,1,DT_REG));CHECK(budget.bytes==SUMH_MERGE_MEMORY_BUDGET);
    CHECK(!sumh_mat_filldir(&mc.ctx,"b",1,0,1,DT_REG));CHECK(mc.error==-E2BIG);sumh_mat_clear_names(&mc);CHECK(budget.bytes==reservation);finish();CHECK(balanced());
    /* All queues are enumerated before first child publication. */
    init();child(captured,"first",DT_REG,node(S_IFREG));child(captured,".replace",DT_REG,node(S_IFREG));batch_size=1;
    CHECK(materialize()==-EOPNOTSUPP);CHECK(!relative_lookups&&!publishes);finish();CHECK(balanced());
    init();child(captured,"gone",DT_REG,(struct path){0});CHECK(materialize()==-ENOENT);CHECK(!publishes&&relative_lookups==1);finish();CHECK(balanced());
    init();struct path p=node(S_IFDIR);child(captured,"child",DT_DIR,p);visible_children=true;
    path_get(&captured);CHECK(materialize_owned("/visible/file","/source",&captured,8)==-ELOOP);path_put(&captured);
    CHECK(!publishes&&opens==1);finish();CHECK(balanced());
    /* Exact path bound and queue cleanup on join failure. */
    init();child(captured,"a",DT_REG,node(S_IFREG));static char longpath[SUMH_MAX_LEN_PATHNAME];memset(longpath,'x',sizeof(longpath));longpath[0]='/';longpath[sizeof(longpath)-1]=0;
    path_get(&captured);CHECK(materialize_owned(longpath,"/source",&captured,0)==-ENAMETOOLONG);path_put(&captured);CHECK(!publishes&&!relative_lookups);finish();CHECK(balanced());
    return 0;
}
int main(void){int ret=capture_registration();if(ret)return ret;ret=materialization();if(ret)return ret;return bounds();}
"""
