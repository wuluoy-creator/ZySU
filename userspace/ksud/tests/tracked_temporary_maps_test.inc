// Included by the state fixture: actual Journal/capture/Maps helper/client,
// injected filesystem and ioctl boundary. No live kernel or durability claim.
namespace {
std::vector<std::string> tracked_args() {
    return {"sumh","maps","remove","18446744073709551615","4294967295",
        "--instance","18446744073709551615","--generation","9007199254740993"};
}
std::string tracked_revision(const std::vector<std::string>& args) {
    auto value=JsonValue::array();for(const auto& arg:args)value.a.emplace_back(arg);
    return state::revision_for_bytes("sumhp-operation-v1\n"+stringify_json(value));
}
void tracked_response(const std::vector<std::string>& args,int error=0,int bits=-1,int capability_error=0) {
    constexpr int supported=SUMH_FEATURE_RULE_INVENTORY|SUMH_FEATURE_TEMPORARY_MAPS_DELETE;
    if(bits==-1)bits=supported;
    tracked_ioctls.push_back({SUMH_IOC_GET_FEATURES,[=](void* p) {
        *static_cast<int*>(p)=bits;errno=capability_error;return capability_error?-1:0;
    }});
    if(capability_error || (bits&supported)!=supported)return;
    maps_temporary::DeleteRequest expected;assert(maps_temporary::parse_tracked_delete(args,expected));
    tracked_ioctls.push_back({SUMH_IOC_DELETE_TEMPORARY_MAPS,[=](void* p) {
        const auto& request=*static_cast<sumh_maps_delete_arg*>(p);
        check(request.size==40 && request.version==1 && request.target_ino==expected.ino && request.target_dev==expected.dev);
        check(request.expected_instance_id==expected.instance && request.expected_generation==expected.generation);
        errno=error;return error?-1:0;
    }});
}
CommandResult tracked_execute(const std::vector<std::string>& args,const std::string& id,const std::string& revision) {
    return run_command_capture(args,id,revision,CaptureRoute::LocalTemporaryMaps);
}
void test_tracked_lifecycle() {
    reset();const auto args=tracked_args();const auto revision=tracked_revision(args);
    const auto id=reservation(revision,"sumh maps remove");
    check(tracked_delete_calls==0 && tracked_feature_calls==0 && kernel_reads==0 && command_calls==0);
    state::Ledger before;std::string error;check(state::read_operation_history(before,error)=="valid");
    before.verified["kernel_debug"]={revision,"previous-boot","1","2"};put("state_ledger.json",state::encode_ledger(before));
    const auto config=files.at(key("config.json")).content;
    tracked_response(args);const auto result=tracked_execute(args,id,revision);
    check(result.command_executed && result.operation_recorded && result.operation_status=="command_completed");
    check(result.command_exit_code==0 && result.command_errno==0 && tracked_delete_calls==1 && tracked_feature_calls==1);
    check(tracked_ioctls.empty() && command_calls==0 && kernel_reads==0 && files.at(key("config.json")).content==config);
    check(std::none_of(opened.begin(),opened.end(),[](const auto& item){return item.first==key("config.json");}));
    const auto raw=decoded(result.stdout_text);
    check(raw.o.at("resource").s=="temporary_maps_mutation" && raw.o.at("operation_id").s==id && raw.o.at("history_supported").b);
    check(!raw.o.at("saved").b && !raw.o.at("observer_verified").b && raw.o.at("acknowledged").b);
    check(written.size()==4 && written[2].first==key("operation_resources.json") && written[3].first==key("state_ledger.json"));
    state::Ledger after;check(state::read_operation_history(after,error)=="valid" && after.verified.size()==1);
    check(after.verified.at("kernel_debug").boot_id=="previous-boot");
    const auto writes=write_calls,prepares=prepare_calls;
    auto observed=state::query_operation_resource(id);
    check(observed.exit_code==0 && stringify_json(observed.value.o.at("result"))==stringify_json(raw));
    check(write_calls==writes && prepare_calls==prepares && kernel_reads==0 && tracked_feature_calls==1 && tracked_delete_calls==1);
    check(query({"operation","show",id}).first==0 && query({"operation","wait",id,"0"}).first==0);
    const auto again=tracked_execute(args,id,revision);
    check(!again.command_executed && again.operation_status=="already_started_or_finished" && tracked_delete_calls==1 && write_calls==writes);
    auto changed=args;changed[3]="1";
    check(tracked_execute(changed,id,tracked_revision(changed)).operation_status=="request_conflict" && tracked_delete_calls==1);
    boot_id="next-boot";
    check(tracked_execute(args,id,revision).operation_status=="boot_mismatch" && tracked_delete_calls==1);
    observed=state::query_operation_resource(id);
    check(observed.exit_code==0 && !observed.value.o.at("same_boot").b && observed.value.o.at("origin_boot_id").s=="boot-a");
    boot_id.clear();check(state::query_operation_resource(id).value.o.at("status").s=="context_changed_or_unknown");
    check(tracked_delete_calls==1 && kernel_reads==0);
}
void test_tracked_invalid_and_start_faults() {
    const auto args=tracked_args();const auto revision=tracked_revision(args);
    for(const auto& invalid:std::vector<std::vector<std::string>>{{},{"sumh","maps","list-temporary"},{"config","apply"},
        {"sumh","maps","remove","01","0","--instance","1","--generation","1"}}) {
        reset();const auto result=tracked_execute(invalid,std::string(32,'a'),revision);
        check(!result.command_executed && tracked_delete_calls==0 && tracked_feature_calls==0 && command_calls==0 && write_calls==0);
    }
    for(const auto& id:std::vector<std::string>{"","bad",std::string(32,'A')}) {
        reset();check(!tracked_execute(args,id,revision).command_executed && write_calls==0 && tracked_delete_calls==0);
    }
    reset();const auto unknown=tracked_execute(args,std::string(32,'a'),revision);
    check(!unknown.command_executed && unknown.operation_status=="not_retained" && write_calls==0 && tracked_delete_calls==0);
    for(bool landed:{false,true}) {
        reset();const auto id=reservation(revision,"sumh maps remove");fail_write=2;fail_after_replace=landed;
        const auto result=tracked_execute(args,id,revision);
        check(!result.command_executed && result.operation_status=="journal_unavailable" && tracked_feature_calls==0 && tracked_delete_calls==0);
        fail_write=0;fail_after_replace=false;
        if(landed) check(!tracked_execute(args,id,revision).command_executed && tracked_delete_calls==0);
        else {tracked_response(args);check(tracked_execute(args,id,revision).command_executed && tracked_delete_calls==1);}
    }
    reset();auto id=reservation(revision,"sumh maps remove");corrupt_readback_write=2;
    check(!tracked_execute(args,id,revision).command_executed && tracked_delete_calls==0);
    reset();id=reservation(revision,"sumh maps remove");held_lock=999;
    check(!tracked_execute(args,id,revision).command_executed && tracked_delete_calls==0);held_lock=-1;
    reset();id=reservation(revision,"sumh maps remove");boot_id="different";
    check(tracked_execute(args,id,revision).operation_status=="boot_mismatch" && tracked_feature_calls==0);
    reset();id=reservation(revision,"sumh maps remove");prepare_throws=true;
    auto exceptional=tracked_execute(args,id,revision);
    check(!exceptional.command_executed && exceptional.operation_status=="not_started" && tracked_delete_calls==0);
    reset();id=reservation(revision,"sumh maps remove");throw_after_write=2;
    exceptional=tracked_execute(args,id,revision);
    check(!exceptional.command_executed && exceptional.operation_status=="not_started" && tracked_delete_calls==0);
    throw_after_write=0;
    check(query({"operation","show",id}).second.o.at("records").a.front().o.at("phase").s=="started");
    check(!tracked_execute(args,id,revision).command_executed && tracked_delete_calls==0);
    reset();id=reservation(revision,"sumh maps remove");
    for(int i=0;i<16;++i)(void)reservation(revision,"sumh maps remove");
    check(tracked_execute(args,id,revision).operation_status=="not_retained" && tracked_delete_calls==0);
}
void test_tracked_native_outcomes() {
    const auto args=tracked_args();const auto revision=tracked_revision(args);
    for(int number:{0,ESTALE,ENOENT,EINVAL,EOVERFLOW,ENOMEM,ESHUTDOWN,EFAULT,EOPNOTSUPP,ENOTTY,EACCES,EPERM,EIO,EINTR,EBUSY}) {
        reset();const auto id=reservation(revision,"sumh maps remove");tracked_response(args,number);
        const auto result=tracked_execute(args,id,revision);const auto raw=decoded(result.stdout_text);
        const bool unknown=number==EIO || number==EINTR || number==EBUSY;
        check(result.command_executed && result.operation_recorded && result.command_errno==number && result.command_exit_code==(number?1:0));
        check(raw.o.at("status").s==(!number?"deleted":unknown?"outcome_unknown":"not_applied"));
        check(raw.o.at("ioctl_attempted").b && raw.o.at("acknowledged").b==!number && tracked_delete_calls==1);
        const auto observed=state::query_operation_resource(id);
        check(observed.exit_code==0 && observed.value.o.at("result").o.at("errno").n==number && kernel_reads==0);
        check(!tracked_execute(args,id,revision).command_executed && tracked_delete_calls==1 && tracked_ioctls.empty());
    }
    for(int bits:{0,SUMH_FEATURE_RULE_INVENTORY,SUMH_FEATURE_TEMPORARY_MAPS_DELETE}) {
        reset();const auto id=reservation(revision,"sumh maps remove");tracked_response(args,0,bits);
        const auto result=tracked_execute(args,id,revision);const auto raw=decoded(result.stdout_text);
        check(result.operation_recorded && raw.o.at("status").s=="not_applied" && !raw.o.at("ioctl_attempted").b);
        check(raw.o.at("errno").n==EOPNOTSUPP && tracked_delete_calls==0 && tracked_feature_calls==1 && state::query_operation_resource(id).exit_code==0);
    }
    for(int number:{EIO,EINTR,ENOENT,EACCES,ENOTTY}) {
        reset();const auto id=reservation(revision,"sumh maps remove");tracked_response(args,0,0,number);
        const auto result=tracked_execute(args,id,revision);const auto raw=decoded(result.stdout_text);
        check(result.operation_recorded && !raw.o.at("ioctl_attempted").b && !raw.o.at("acknowledged").b && raw.o.at("errno").n==number);
        check(tracked_delete_calls==0 && tracked_feature_calls==1 && state::query_operation_resource(id).exit_code==0);
    }
}
void test_tracked_resource_and_finish_faults() {
    const auto args=tracked_args();const auto revision=tracked_revision(args);
    for(int stage:{3,4})for(bool landed:{false,true}) {
        reset();const auto id=reservation(revision,"sumh maps remove");fail_write=stage;fail_after_replace=landed;tracked_response(args);
        const auto result=tracked_execute(args,id,revision);
        check(result.command_executed && tracked_delete_calls==1 && result.operation_recorded==(stage==3));
        check(result.operation_status==(stage==3?"command_completed":"outcome_unknown"));
        const auto observed=state::query_operation_resource(id);
        check(observed.value.o.at("status").s==(stage==3&&!landed?"resource_unavailable":stage==4&&!landed?"not_finished":"observed"));
        fail_write=0;fail_after_replace=false;
        check(!tracked_execute(args,id,revision).command_executed && tracked_delete_calls==1 && tracked_ioctls.empty());
    }
    reset();const auto id=reservation(revision,"sumh maps remove");corrupt_readback_write=3;tracked_response(args);
    check(tracked_execute(args,id,revision).operation_recorded && tracked_delete_calls==1);
    check(state::query_operation_resource(id).exit_code==5);
    check(!tracked_execute(args,id,revision).command_executed && tracked_delete_calls==1);
}
void test_tracked_resource_codec() {
    using namespace state;
    reset();const auto args=tracked_args();const auto revision=tracked_revision(args);const auto id=reservation(revision,"sumh maps remove");
    tracked_response(args);const auto execution=tracked_execute(args,id,revision);const auto receipt=decoded(execution.stdout_text);
    const Operation operation{id,"sumh maps remove","started","boot-a",0,0,"",revision};
    OperationResource resource;OperationResources resources;std::string error;
    check(make_operation_resource(operation,args,execution.stdout_text,true,0,0,resource,error));
    const auto store=encode_operation_resources({resource});int schema=-1;
    check(parse_operation_resources(store,resources,error,&schema) && schema==2 && resources.size()==1);
    for(const auto& change:std::vector<std::function<void(JsonValue&)>>{
        [](auto& v){v.o["resource"]="temporary_maps";},[](auto& v){v.o["action"]="clear";},
        [](auto& v){v.o["operation_id"]=std::string(32,'f');},[](auto& v){v.o["history_supported"]=false;},
        [](auto& v){v.o["saved"]=true;},[](auto& v){v.o["observer_verified"]=true;},
        [](auto& v){v.o["acknowledged"]=false;},[](auto& v){v.o["ioctl_attempted"]=false;},
        [](auto& v){v.o["status"]="not_applied";},[](auto& v){v.o["error_code"]="outcome_unknown";},
        [](auto& v){v.o["errno"]=EIO;},[](auto& v){v.o["request"]=JsonValue{};},
        [](auto& v){v.o["request"].o["target_ino"]="1";},[](auto& v){v.o["request"].o["target_dev"]="0";},
        [](auto& v){v.o["request"].o["expected_instance_id"]="1";},[](auto& v){v.o["request"].o["expected_generation"]="1";},
        [](auto& v){v.o["request"].o["target_ino"]="01";},[](auto& v){v.o["request"].o["target_dev"]="4294967296";},
        [](auto& v){v.o["request"].o["expected_instance_id"]="0";},[](auto& v){v.o["extra"]=true;}}) {
        auto altered=receipt;change(altered);
        check(!make_operation_resource(operation,args,stringify_json(altered),true,0,0,resource,error));
        auto persisted=decoded(store);persisted.o["receipts"].a.front().o["result"]=altered;
        check(!parse_operation_resources(stringify_json(persisted),resources,error));
    }
    for(const auto* field:{"request_revision","boot_id","name","command_errno","command_exit_code"}) {
        auto changed=decoded(store);auto& row=changed.o["receipts"].a.front();
        if(std::string(field)=="command_errno" || std::string(field)=="command_exit_code")row.o[field]=1;
        else row.o[field]=std::string(field)=="request_revision"?"sha256-v1:"+std::string(64,'f'):"";
        check(!parse_operation_resources(stringify_json(changed),resources,error));
    }
    auto old=decoded(store);old.o["schema_version"]=1;schema=99;
    check(!parse_operation_resources(stringify_json(old),resources,error,&schema) && schema==99);
    check(!make_operation_resource(operation,args,execution.stdout_text,false,0,0,resource,error));
    check(!make_operation_resource(operation,args,execution.stdout_text,true,1,EIO,resource,error));
    check(!make_operation_resource(operation,args,execution.stdout_text+"{}",true,0,0,resource,error));
    auto different=args;different[3]="0";
    check(!make_operation_resource(operation,different,execution.stdout_text,true,0,0,resource,error));
}
void test_tracked_schema_migration() {
    using namespace state;
    reset();const auto hide_revision=revision_for_bytes("old hide intent");const auto hide_id=reservation(hide_revision,"hide disable");
    command_output=stringify_json(hide_resource_fixture());
    check(run_command_capture({"hide","disable",operation(1).id,revision_for_bytes("hide revision")},hide_id,hide_revision).operation_recorded);
    auto old=decoded(files.at(key("operation_resources.json")).content);old.o["schema_version"]=1;
    const auto original=stringify_json(old)+"\n \n";put("operation_resources.json",original);
    const auto seed=files;const auto args=tracked_args();const auto revision=tracked_revision(args);
    const auto old_writes=write_calls;check(query_operation_resource(hide_id).exit_code==0 && write_calls==old_writes);
    check(!files.count(key("operation_resources.schema1.bak")));
    for(int mode=0;mode<9;++mode) {
        reset();files=seed;random_calls=30;const auto id=reservation(revision,"sumh maps remove");
        if(mode==1)put("operation_resources.schema1.bak","unrelated backup");
        if(mode==2){put("operation_resources.schema1.bak",original);files[key("operation_resources.schema1.bak")].links=2;}
        if(mode==3)put("operation_resources.schema1.bak",original);
        if(mode==4 || mode==5){fail_write=3;fail_after_replace=mode==5;}
        if(mode==6)corrupt_readback_write=3;
        int source_reads=0;
        if(mode==7 || mode==8)on_open=[&](const std::string& path) {
            if(path==key("operation_resources.json") && ++source_reads==(mode==7?2:3))put("operation_resources.json",original+" ");
        };
        tracked_response(args);const auto result=tracked_execute(args,id,revision);on_open={};
        check(result.operation_recorded && tracked_delete_calls==1 && kernel_reads==0);
        const bool migrated=mode==0 || mode==3;
        check(decoded(files.at(key("operation_resources.json")).content).o.at("schema_version").n==(migrated?2:1));
        if(migrated) {
            check(files.at(key("operation_resources.schema1.bak")).content==original);
            check(query_operation_resource(id).exit_code==0 && query_operation_resource(hide_id).exit_code==0);
            OperationResources parsed;std::string error;int schema=0;
            check(parse_operation_resources(files.at(key("operation_resources.json")).content,parsed,error,&schema) && schema==2 && parsed.size()==2);
        } else {
            check(files.at(key("operation_resources.json")).content==(mode>=7?original+" ":original));
            check(query_operation_resource(id).exit_code==6);
            if(mode==1)check(files.at(key("operation_resources.schema1.bak")).content=="unrelated backup");
        }
        fail_write=corrupt_readback_write=0;fail_after_replace=false;
        check(!tracked_execute(args,id,revision).command_executed && tracked_delete_calls==1);
    }
    for(const auto& invalid:std::vector<std::string>{"{\"schema_version\":3,\"receipts\":[]}","{}"}) {
        reset();put("operation_resources.json",invalid);const auto id=reservation(revision,"sumh maps remove");tracked_response(args);
        check(tracked_execute(args,id,revision).operation_recorded && files.at(key("operation_resources.json")).content==invalid);
        check(!files.count(key("operation_resources.schema1.bak")) && tracked_delete_calls==1);
    }
}
void test_tracked_temporary_maps() {
    const auto before=extended_checks;
    for(const auto& group:std::vector<std::pair<const char*,std::function<void()>>>{
        {"lifecycle",test_tracked_lifecycle},{"start",test_tracked_invalid_and_start_faults},{"native",test_tracked_native_outcomes},
        {"finish",test_tracked_resource_and_finish_faults},{"codec",test_tracked_resource_codec},{"migration",test_tracked_schema_migration}}) {
        try {group.second();}catch(const std::exception& error) {
            std::cerr << "Tracked Maps group " << group.first << " after check " << extended_checks << ": " << error.what() << '\n';throw;
        }
    }
    std::cout << "Tracked temporary Maps: " << extended_checks-before << " actual-flow/codec/migration checks passed\n";
}
} // namespace
